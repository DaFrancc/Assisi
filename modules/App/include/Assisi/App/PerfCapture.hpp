/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PerfCapture.hpp
/// @brief A repeatable frame-time capture: run a scene for N frames, publish
///        median + IQR, and say whether the hardware held still enough to
///        believe the numbers.
///
/// This exists because the rendering plan of record has no fixed frame budget.
/// Nothing is rejected on cost grounds; instead every feature publishes what it
/// costs, measured. That makes the measurement the load-bearing part, and a
/// measurement nobody can reproduce is not one.
///
/// @par Why medians and IQR rather than an average
/// A mean over a few hundred frames is dominated by a handful of outliers — a
/// compositor hiccup, a shader cache miss, the window manager waking up. Those
/// are real, but they are not the cost of the feature under test. The median
/// ignores them and the IQR says how noisy the run was, so a reader can tell a
/// tight 1.30 ms from a 1.30 ms that was really bouncing between 0.9 and 1.8.
///
/// @par Why the clock guard
/// The protocol cannot assume pinned GPU clocks: pinning needs root, laptop
/// drivers often refuse outright, and laptop parts thermally downclock under
/// sustained load. An unpinned GPU quietly changing its clock mid-run will move
/// frame times by more than most of the features being measured. So every
/// capture records NVML core clock and temperature alongside the frame times,
/// and a run whose clock drifted or whose temperature climbed is reported as
/// **untrustworthy rather than merely noisy** — the numbers are still printed,
/// but they are marked, because silently believing them is how a ledger fills up
/// with figures that cannot be reproduced.
///
/// @par Why A/B rather than absolutes
/// Only deltas measured within one session are meaningful. Two runs on different
/// days differ by driver state, background load and ambient temperature by more
/// than a feature usually costs. Capture mode therefore produces one side of a
/// comparison; the gate is two captures taken back to back.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Assisi::App
{

/// @brief One frame's measurements.
struct PerfSample
{
    double cpuMs = 0.0;
    double gpuMs = 0.0;

    /// NVML readings taken alongside. `telemetryValid` is false on a non-NVIDIA
    /// GPU or a driver without NVML, in which case the clock guard reports that
    /// it could not run rather than pretending the hardware was steady.
    bool telemetryValid   = false;
    uint32_t coreClockMhz   = 0;
    uint32_t temperatureC   = 0;
};

/// @brief The distribution of one measured series.
struct Distribution
{
    double median = 0.0;
    double p25    = 0.0;
    double p75    = 0.0;
    double min    = 0.0;
    double max    = 0.0;
    int32_t count  = 0;

    [[nodiscard]] double Iqr() const { return p75 - p25; }
};

/// @brief Linear-interpolated percentile of @p values, which need not be sorted.
///
/// @p fraction is clamped to [0, 1]. Interpolates between the two neighbouring
/// ranks (the definition numpy and R type 7 use), so an even-sized series has
/// the median between its middle pair rather than arbitrarily picking one.
/// Returns 0 for an empty series.
[[nodiscard]] double Percentile(std::span<const double> values, double fraction);

/// @brief Median, quartiles and range of @p values. Empty in, zeroed out.
[[nodiscard]] Distribution Summarize(std::span<const double> values);

/// @brief Whether the hardware held still enough for the capture to be believed.
struct ClockGuard
{
    /// False when the clock moved, the temperature climbed, or there was no
    /// telemetry to check. A capture that fails this is reported, not deleted —
    /// but it must not become a ledger entry.
    bool trustworthy = false;

    /// Peak-to-peak core clock as a fraction of the median. A GPU boosting or
    /// throttling mid-run shows up here before it shows up anywhere else.
    double clockDrift = 0.0;

    /// Temperature at the end of the run minus at the start, in Celsius. Rising
    /// temperature is the leading indicator of a throttle that has not yet
    /// happened, which is exactly the run whose early frames are fast and whose
    /// late frames are not.
    int32_t temperatureRiseC = 0;

    /// Human-readable reason when `trustworthy` is false; empty when it is true.
    std::string reason;
};

/// @brief Peak-to-peak core clock drift above this fraction of the median fails
/// the guard. 5% of a 1.3 ms frame is 0.065 ms — larger than several of the
/// features this instrument exists to measure.
inline constexpr double kMaxClockDrift = 0.05;

/// @brief Temperature rise across the capture, in Celsius, above which the run
/// fails the guard.
///
/// A judgement rather than a derived number: the plan says "thermals climbed"
/// without fixing a figure. 5 C is warm-up settling on a desktop part and is
/// tolerated; more than that on a run of a few seconds means the cooling has not
/// caught up and the clock is about to follow.
inline constexpr int32_t kMaxTemperatureRiseC = 5;

/// @brief Run the guard over a capture's samples.
[[nodiscard]] ClockGuard EvaluateClockGuard(std::span<const PerfSample> samples);

/// @brief What a capture run was asked to do.
struct PerfCaptureConfig
{
    /// Frames to measure. The protocol asks for at least 500; fewer is allowed
    /// (it is useful when iterating) but is flagged in the report.
    int32_t frames = 600;

    /// Frames to run and discard before measuring. Covers pipeline compilation,
    /// the first-use upload of every mesh and texture the scene touches, and the
    /// swapchain settling — all real costs, none of them steady-state.
    int32_t warmupFrames = 120;

    /// Where to write the machine-readable report. Empty writes none, and the
    /// human-readable summary still goes to the log.
    std::string outputPath;

    /// The level the numbers belong to, recorded in the report so a stray file
    /// can still say what it measured.
    std::string levelPath;
};

/// @brief Accumulates a capture run and reports on it.
///
/// Owned by Application, which feeds it one sample per frame and asks it when to
/// stop. Deliberately holds no rendering or windowing state: everything it needs
/// arrives through AddSample, which makes the statistics testable without a GPU.
class PerfCapture
{
public:
    explicit PerfCapture(PerfCaptureConfig config);

    /// @brief Record one frame. Frames inside the warm-up are counted and
    /// dropped.
    void AddSample(const PerfSample &sample);

    /// @brief Record one frame's per-pass GPU timings, if pass timing was on.
    /// Passes may appear or vanish between frames (an empty scene records no
    /// draw), so each series is summarised over the frames it actually appeared
    /// in and reports that count.
    ///
    /// @warning Call **after** this frame's AddSample. Pass timings are attached
    /// to whichever frame that call accepted, so timings offered before it land
    /// on the previous frame or are dropped.
    void AddPassTiming(const char *name, double milliseconds);

    /// @brief True once the requested frames have been measured.
    [[nodiscard]] bool IsComplete() const;

    /// @brief Frames measured so far, excluding warm-up.
    [[nodiscard]] int32_t MeasuredFrames() const { return static_cast<int32_t>(_cpuMs.size()); }

    /// @brief True while still inside the warm-up.
    [[nodiscard]] bool IsWarmingUp() const { return _warmupSeen < _config.warmupFrames; }

    /// @brief The human-readable summary, as written to the log.
    [[nodiscard]] std::string FormatReport() const;

    /// @brief Write the machine-readable report to the configured path.
    /// @return false if the path was set but could not be written.
    [[nodiscard]] bool WriteReport() const;

    /// @brief Extra context recorded into the report: the resolution actually
    /// rendered, which is the swapchain extent rather than whatever was
    /// requested — a window manager is free to hand back a different size, and
    /// a report that quoted the request would be quoting a resolution that was
    /// never rendered.
    void SetRenderExtent(uint32_t width, uint32_t height);

    /// @brief The GPU the capture ran on, for the report header.
    void SetDeviceName(std::string name);

private:
    struct PassSeries
    {
        std::string name;
        std::vector<double> milliseconds;
    };

    PerfCaptureConfig _config;
    int32_t _warmupSeen = 0;

    /// Whether the most recent AddSample kept its frame — see AddPassTiming.
    bool _lastSampleAccepted = false;

    std::vector<double> _cpuMs;
    std::vector<double> _gpuMs;
    std::vector<PerfSample> _samples;
    std::vector<PassSeries> _passes;

    uint32_t _width  = 0;
    uint32_t _height = 0;
    std::string _deviceName;
};

} // namespace Assisi::App
