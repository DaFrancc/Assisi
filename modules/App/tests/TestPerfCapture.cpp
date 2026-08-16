/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// Tests for the capture statistics and the clock guard.
///
/// These are the part of the measurement instrument most worth testing, because
/// they are the part whose failures are invisible. A wrong percentile or a guard
/// that never fires does not crash or look odd — it produces a plausible number
/// that goes into the ledger and gets quoted at approval gates for months. The
/// rest of capture mode is plumbing whose failures announce themselves.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include <Assisi/App/PerfCapture.hpp>

using namespace Assisi::App;

namespace
{

// Each call advances the NVML sequence, so a sample is a fresh driver reading
// unless a test deliberately reuses one. Real NVML is polled far slower than the
// frame rate — see FramesPerReading below for the case that models.
uint64_t g_sequence = 0;

PerfSample Sample(double gpuMs, uint32_t clockMhz = 1900, uint32_t temperatureC = 60, bool valid = true)
{
    PerfSample sample;
    sample.cpuMs             = gpuMs * 0.5;
    sample.gpuMs             = gpuMs;
    sample.frameDeltaMs      = gpuMs;
    sample.coreClockMhz      = clockMhz;
    sample.temperatureC      = temperatureC;
    sample.telemetryValid    = valid;
    sample.telemetrySequence = ++g_sequence;
    return sample;
}

} // namespace

TEST_CASE("Percentile interpolates between ranks")
{
    const std::vector<double> values{1.0, 2.0, 3.0, 4.0};

    // Even count: the median sits between the middle pair rather than picking
    // one of them. Getting this wrong biases every published median by half a
    // sample's spacing, which is invisible and wrong.
    CHECK(Percentile(values, 0.5) == doctest::Approx(2.5));
    CHECK(Percentile(values, 0.0) == doctest::Approx(1.0));
    CHECK(Percentile(values, 1.0) == doctest::Approx(4.0));
    CHECK(Percentile(values, 0.25) == doctest::Approx(1.75));
    CHECK(Percentile(values, 0.75) == doctest::Approx(3.25));

    const std::vector<double> odd{1.0, 2.0, 3.0};
    CHECK(Percentile(odd, 0.5) == doctest::Approx(2.0));
}

TEST_CASE("Percentile does not depend on input order")
{
    const std::vector<double> ascending{1.0, 2.0, 3.0, 4.0, 5.0};
    const std::vector<double> shuffled{4.0, 1.0, 5.0, 3.0, 2.0};
    CHECK(Percentile(ascending, 0.5) == doctest::Approx(Percentile(shuffled, 0.5)));
    CHECK(Percentile(ascending, 0.25) == doctest::Approx(Percentile(shuffled, 0.25)));
}

TEST_CASE("Percentile survives degenerate input")
{
    CHECK(Percentile({}, 0.5) == doctest::Approx(0.0));

    const std::vector<double> single{7.0};
    CHECK(Percentile(single, 0.0) == doctest::Approx(7.0));
    CHECK(Percentile(single, 0.5) == doctest::Approx(7.0));
    CHECK(Percentile(single, 1.0) == doctest::Approx(7.0));

    // Out-of-range fractions clamp rather than reading off the end.
    const std::vector<double> values{1.0, 2.0, 3.0};
    CHECK(Percentile(values, -1.0) == doctest::Approx(1.0));
    CHECK(Percentile(values, 2.0) == doctest::Approx(3.0));
}

// The reason medians are published at all: a couple of huge frames must not move
// the headline number. If this ever fails, the instrument is reporting the
// compositor rather than the renderer.
TEST_CASE("The median ignores outliers that would dominate a mean")
{
    std::vector<double> values(100, 1.30);
    values.push_back(50.0); // a compositor hiccup
    values.push_back(80.0); // and another

    const Distribution distribution = Summarize(values);
    CHECK(distribution.median == doctest::Approx(1.30));
    CHECK(distribution.Iqr() == doctest::Approx(0.0));
    CHECK(distribution.max == doctest::Approx(80.0)); // still visible, just not in the headline
    CHECK(distribution.count == 102);
}

TEST_CASE("Summarize reports the spread, not just the middle")
{
    std::vector<double> values;
    for (int32_t i = 0; i < 100; ++i)
    {
        values.push_back(1.0 + static_cast<double>(i) * 0.01);
    }

    const Distribution distribution = Summarize(values);
    CHECK(distribution.median == doctest::Approx(1.495));
    CHECK(distribution.min == doctest::Approx(1.0));
    CHECK(distribution.max == doctest::Approx(1.99));
    CHECK(distribution.Iqr() > 0.4);
    CHECK(distribution.Iqr() < 0.6);
}

TEST_CASE("Summarize of nothing is zeroed rather than undefined")
{
    const Distribution distribution = Summarize({});
    CHECK(distribution.count == 0);
    CHECK(distribution.median == doctest::Approx(0.0));
    CHECK(distribution.Iqr() == doctest::Approx(0.0));
}

TEST_CASE("The clock guard passes a steady run")
{
    std::vector<PerfSample> samples;
    for (int32_t i = 0; i < 600; ++i)
    {
        samples.push_back(Sample(1.30, /*clockMhz=*/ 1900 + static_cast<uint32_t>(i % 5), /*temperatureC=*/ 61));
    }

    const ClockGuard guard = EvaluateClockGuard(samples);
    CHECK(guard.trustworthy);
    CHECK(guard.reason.empty());
    CHECK(guard.clockDrift < 0.01);
    CHECK(guard.temperatureRiseC == 0);
}

// The failure this whole mechanism exists for: an unpinned GPU that boosted or
// throttled partway through, moving frame times by more than the feature under
// test costs.
TEST_CASE("The clock guard rejects a run whose core clock drifted")
{
    std::vector<PerfSample> samples;
    for (int32_t i = 0; i < 300; ++i)
    {
        samples.push_back(Sample(1.30, /*clockMhz=*/ 1900, /*temperatureC=*/ 61));
    }
    for (int32_t i = 0; i < 300; ++i)
    {
        samples.push_back(Sample(1.45, /*clockMhz=*/ 1700, /*temperatureC=*/ 63)); // throttled
    }

    const ClockGuard guard = EvaluateClockGuard(samples);
    CHECK_FALSE(guard.trustworthy);
    CHECK(guard.clockDrift > kMaxClockDrift);
    CHECK(guard.reason.find("clock") != std::string::npos);
}

// The distinction that makes the guard usable at all. A desktop GPU on a scene
// light enough to leave it partly idle swings between its idle floor and full
// boost every few frames, with no trend at all — the median over hundreds of
// frames is unaffected. Gating on peak-to-peak discarded every honest run on
// real hardware, which is how this case got written.
TEST_CASE("The clock guard tolerates boost jitter that does not trend")
{
    std::vector<PerfSample> samples;
    for (int32_t i = 0; i < 600; ++i)
    {
        // Alternating idle floor and full boost: a 69% peak-to-peak spread whose
        // two ends are statistically identical.
        const uint32_t clock = (i % 3 == 0) ? 555u : 1815u;
        samples.push_back(Sample(1.30, clock, /*temperatureC=*/ 61));
    }

    const ClockGuard guard = EvaluateClockGuard(samples);
    CHECK(guard.trustworthy);
    CHECK(guard.clockDrift < kMaxClockDrift);

    // The spread is still reported, because it does mean the frame times are
    // noisy even though the median is sound.
    CHECK(guard.clockSpread > 0.5);
}

TEST_CASE("The clock guard rejects a run whose temperature climbed")
{
    std::vector<PerfSample> samples;
    for (int32_t i = 0; i < 600; ++i)
    {
        // Steady clock, but heating steadily — the state just before a throttle,
        // where the early frames are honest and the late ones are not.
        const uint32_t temperature = 55 + static_cast<uint32_t>(i / 40);
        samples.push_back(Sample(1.30, /*clockMhz=*/ 1900, temperature));
    }

    const ClockGuard guard = EvaluateClockGuard(samples);
    CHECK_FALSE(guard.trustworthy);
    CHECK(guard.temperatureRiseC > kMaxTemperatureRiseC);
    CHECK(guard.reason.find("temperature") != std::string::npos);
}

// Absence of evidence is not evidence of stability. A machine with no NVML can
// still A/B against itself, but nothing from it may be quoted as an absolute, so
// the guard must fail rather than default to trusting.
TEST_CASE("The clock guard fails closed when there is no telemetry")
{
    std::vector<PerfSample> samples;
    for (int32_t i = 0; i < 600; ++i)
    {
        samples.push_back(Sample(1.30, /*clockMhz=*/ 0, /*temperatureC=*/ 0, /*valid=*/ false));
    }

    const ClockGuard guard = EvaluateClockGuard(samples);
    CHECK_FALSE(guard.trustworthy);
    CHECK(guard.reason.find("telemetry") != std::string::npos);

    CHECK_FALSE(EvaluateClockGuard({}).trustworthy);
}

TEST_CASE("PerfCapture discards warm-up frames and stops when full")
{
    PerfCaptureConfig config;
    config.frames       = 10;
    config.warmupFrames = 4;
    PerfCapture capture(config);

    // Warm-up frames are deliberately slow — pipeline compilation and first-use
    // uploads. Letting even one into the measured set moves the median.
    for (int32_t i = 0; i < 4; ++i)
    {
        CHECK(capture.IsWarmingUp());
        capture.AddSample(Sample(40.0));
    }
    CHECK_FALSE(capture.IsWarmingUp());
    CHECK(capture.MeasuredFrames() == 0);

    for (int32_t i = 0; i < 10; ++i)
    {
        CHECK_FALSE(capture.IsComplete());
        capture.AddSample(Sample(1.30));
    }
    CHECK(capture.IsComplete());
    CHECK(capture.MeasuredFrames() == 10);

    // Samples past the requested count are dropped rather than extending the run.
    capture.AddSample(Sample(99.0));
    CHECK(capture.MeasuredFrames() == 10);

    const std::string report = capture.FormatReport();
    CHECK(report.find("1.300") != std::string::npos); // the steady frames, not the warm-up ones
    CHECK(report.find("40.000") == std::string::npos);
}

TEST_CASE("PerfCapture groups per-pass timings by name")
{
    PerfCaptureConfig config;
    config.frames       = 4;
    config.warmupFrames = 0;
    PerfCapture capture(config);

    for (int32_t i = 0; i < 4; ++i)
    {
        capture.AddSample(Sample(1.30));
        capture.AddPassTiming("draw-scene", 0.80);
        capture.AddPassTiming("lighting", 0.12);
        // A pass that only appears in some frames is summarised over the frames
        // it appeared in — an empty scene records no draw, and averaging in
        // zeroes for the frames it was absent would understate what it costs
        // when it runs.
        if (i % 2 == 0)
        {
            capture.AddPassTiming("outlines", 0.05);
        }
    }

    const std::string report = capture.FormatReport();
    CHECK(report.find("draw-scene") != std::string::npos);
    CHECK(report.find("lighting") != std::string::npos);
    CHECK(report.find("outlines") != std::string::npos);
    CHECK(report.find("x4") != std::string::npos); // draw-scene and lighting
    CHECK(report.find("x2") != std::string::npos); // outlines
}

// A run too short to quote must say so in the report itself. Documenting it
// elsewhere is how a 60-frame number ends up in the ledger.
TEST_CASE("PerfCapture flags a run shorter than the protocol asks for")
{
    PerfCaptureConfig config;
    config.frames       = 50;
    config.warmupFrames = 0;
    PerfCapture shortRun(config);
    for (int32_t i = 0; i < 50; ++i)
    {
        shortRun.AddSample(Sample(1.30));
    }
    CHECK(shortRun.FormatReport().find("not for a ledger entry") != std::string::npos);

    config.frames = 500;
    PerfCapture fullRun(config);
    for (int32_t i = 0; i < 500; ++i)
    {
        fullRun.AddSample(Sample(1.30));
    }
    CHECK(fullRun.FormatReport().find("not for a ledger entry") == std::string::npos);
}
