/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/PerfCapture.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <format>
#include <utility>

#include <Assisi/Core/Logger.hpp>

#include <nlohmann/json.hpp>

namespace Assisi::App
{

double Percentile(std::span<const double> values, double fraction)
{
    if (values.empty())
    {
        return 0.0;
    }

    std::vector<double> sorted(values.begin(), values.end());
    std::sort(sorted.begin(), sorted.end());

    if (sorted.size() == 1)
    {
        return sorted.front();
    }

    const double clamped = std::clamp(fraction, 0.0, 1.0);
    const double rank    = clamped * static_cast<double>(sorted.size() - 1);
    const double floored = std::floor(rank);

    const std::size_t low  = static_cast<std::size_t>(floored);
    const std::size_t high = std::min(low + 1, sorted.size() - 1);
    const double weight  = rank - floored;

    return sorted[low] * (1.0 - weight) + sorted[high] * weight;
}

Distribution Summarize(std::span<const double> values)
{
    Distribution distribution;
    if (values.empty())
    {
        return distribution;
    }

    distribution.count  = static_cast<int32_t>(values.size());
    distribution.median = Percentile(values, 0.5);
    distribution.p25    = Percentile(values, 0.25);
    distribution.p75    = Percentile(values, 0.75);
    distribution.min    = *std::min_element(values.begin(), values.end());
    distribution.max    = *std::max_element(values.begin(), values.end());
    return distribution;
}

ClockGuard EvaluateClockGuard(std::span<const PerfSample> samples)
{
    ClockGuard guard;

    std::vector<double> clocks;
    uint32_t firstTemperature = 0;
    uint32_t lastTemperature  = 0;
    bool haveTemperature    = false;

    for (const PerfSample &sample : samples)
    {
        if (!sample.telemetryValid)
        {
            continue;
        }
        clocks.push_back(static_cast<double>(sample.coreClockMhz));
        if (!haveTemperature)
        {
            firstTemperature = sample.temperatureC;
            haveTemperature  = true;
        }
        lastTemperature = sample.temperatureC;
    }

    if (clocks.empty())
    {
        // Not a pass. Without telemetry there is no evidence the clock held, and
        // the honest report is that the guard could not run — a capture on a
        // machine with no NVML is still useful for A/B against another capture
        // on the same machine, but it must not be quoted as an absolute.
        guard.trustworthy = false;
        guard.reason      = "no GPU telemetry available, so clock stability could not be checked";
        return guard;
    }

    const double medianClock = Percentile(clocks, 0.5);
    const double lowest      = *std::min_element(clocks.begin(), clocks.end());
    const double highest     = *std::max_element(clocks.begin(), clocks.end());
    guard.clockDrift         = medianClock > 0.0 ? (highest - lowest) / medianClock : 0.0;
    guard.temperatureRiseC =
        static_cast<int32_t>(lastTemperature) - static_cast<int32_t>(firstTemperature);

    if (guard.clockDrift > kMaxClockDrift)
    {
        guard.reason = std::format("core clock drifted {:.1f}% ({:.0f}-{:.0f} MHz), over the {:.0f}% limit",
                                   guard.clockDrift * 100.0, lowest, highest, kMaxClockDrift * 100.0);
        return guard;
    }
    if (guard.temperatureRiseC > kMaxTemperatureRiseC)
    {
        guard.reason = std::format("temperature climbed {} C during the run, over the {} C limit",
                                   guard.temperatureRiseC, kMaxTemperatureRiseC);
        return guard;
    }

    guard.trustworthy = true;
    return guard;
}

PerfCapture::PerfCapture(PerfCaptureConfig config) : _config(std::move(config))
{
    _cpuMs.reserve(static_cast<std::size_t>(std::max(_config.frames, 0)));
    _gpuMs.reserve(static_cast<std::size_t>(std::max(_config.frames, 0)));
}

void PerfCapture::AddSample(const PerfSample &sample)
{
    _lastSampleAccepted = false;

    if (IsWarmingUp())
    {
        ++_warmupSeen;
        return;
    }
    if (IsComplete())
    {
        return;
    }

    _cpuMs.push_back(sample.cpuMs);
    _gpuMs.push_back(sample.gpuMs);
    _samples.push_back(sample);
    _lastSampleAccepted = true;
}

void PerfCapture::AddPassTiming(const char *name, double milliseconds)
{
    // Gated on the frame's own sample rather than on IsComplete: the sample that
    // fills the capture also makes IsComplete true, so testing that here would
    // silently drop the last measured frame's passes and leave every pass series
    // one frame shorter than the frame series beside it.
    if (name == nullptr || !_lastSampleAccepted)
    {
        return;
    }

    // Linear scan over ~10 passes, once per pass per frame. A map would cost
    // more in allocation than this costs in comparisons.
    for (PassSeries &series : _passes)
    {
        if (series.name == name)
        {
            series.milliseconds.push_back(milliseconds);
            return;
        }
    }
    _passes.push_back(PassSeries{name, {milliseconds}});
}

bool PerfCapture::IsComplete() const
{
    return static_cast<int32_t>(_cpuMs.size()) >= _config.frames;
}

void PerfCapture::SetRenderExtent(uint32_t width, uint32_t height)
{
    _width  = width;
    _height = height;
}

void PerfCapture::SetDeviceName(std::string name)
{
    _deviceName = std::move(name);
}

std::string PerfCapture::FormatReport() const
{
    const Distribution cpu = Summarize(_cpuMs);
    const Distribution gpu = Summarize(_gpuMs);
    const ClockGuard guard = EvaluateClockGuard(_samples);

    std::string report;
    report += std::format("\n=== perf capture: {} ===\n", _config.levelPath.empty() ? "(no level)" : _config.levelPath);
    report += std::format("  device      {}\n", _deviceName.empty() ? "(unknown)" : _deviceName);
    report += std::format("  resolution  {}x{}\n", _width, _height);
    report += std::format("  frames      {} measured, {} warm-up\n", cpu.count, _warmupSeen);

    // The protocol asks for at least 500 frames. A shorter run is legitimate
    // while iterating but must never be quoted, so it says so in the report
    // rather than only in the documentation.
    if (cpu.count < 500)
    {
        report += "  NOTE        fewer than 500 frames: usable for iteration, not for a ledger entry\n";
    }

    report += std::format("  gpu frame   median {:.3f} ms   IQR {:.3f}   min {:.3f}   max {:.3f}\n", gpu.median,
                          gpu.Iqr(), gpu.min, gpu.max);
    report += std::format("  cpu frame   median {:.3f} ms   IQR {:.3f}   min {:.3f}   max {:.3f}\n", cpu.median,
                          cpu.Iqr(), cpu.min, cpu.max);

    if (guard.trustworthy)
    {
        report += std::format("  clock guard PASS (drift {:.2f}%, temp {:+d} C)\n", guard.clockDrift * 100.0,
                              guard.temperatureRiseC);
    }
    else
    {
        report += std::format("  clock guard DISCARD - {}\n", guard.reason);
    }

    if (_passes.empty())
    {
        report += "  passes      (per-pass timing was off)\n";
    }
    else
    {
        report += "  passes      median ms (IQR) over frames measured\n";
        for (const PassSeries &series : _passes)
        {
            const Distribution pass = Summarize(series.milliseconds);
            report += std::format("    {:<16} {:.3f}  ({:.3f})  x{}\n", series.name, pass.median, pass.Iqr(),
                                  pass.count);
        }
    }
    return report;
}

bool PerfCapture::WriteReport() const
{
    if (_config.outputPath.empty())
    {
        return true;
    }

    const Distribution cpu = Summarize(_cpuMs);
    const Distribution gpu = Summarize(_gpuMs);
    const ClockGuard guard = EvaluateClockGuard(_samples);

    const auto describe = [](const Distribution &d)
                          {
                              return nlohmann::json{{"median", d.median}, {"p25", d.p25},   {"p75", d.p75},
                                                    {"iqr", d.Iqr()},     {"min", d.min},   {"max", d.max},
                                                    {"count", d.count}};
                          };

    nlohmann::json report;
    report["level"]           = _config.levelPath;
    report["device"]          = _deviceName;
    report["width"]           = _width;
    report["height"]          = _height;
    report["warmupFrames"]    = _warmupSeen;
    report["gpuFrameMs"]      = describe(gpu);
    report["cpuFrameMs"]      = describe(cpu);
    report["clockGuard"]      = {{"trustworthy", guard.trustworthy},
                                 {"clockDrift", guard.clockDrift},
                                 {"temperatureRiseC", guard.temperatureRiseC},
                                 {"reason", guard.reason}};

    nlohmann::json passes = nlohmann::json::object();
    for (const PassSeries &series : _passes)
    {
        passes[series.name] = describe(Summarize(series.milliseconds));
    }
    report["passes"] = passes;

    std::error_code error;
    const std::filesystem::path path{_config.outputPath};
    if (path.has_parent_path())
    {
        std::filesystem::create_directories(path.parent_path(), error);
    }

    std::ofstream out(path);
    if (!out)
    {
        Core::Log::Error("PerfCapture: could not open '{}' for writing.", _config.outputPath);
        return false;
    }
    out << report.dump(2) << '\n';
    return out.good();
}

} // namespace Assisi::App
