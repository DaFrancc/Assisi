/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file GpuTelemetry.hpp
/// @brief Optional NVIDIA GPU hardware telemetry (clocks, power, util, temp).

#include <memory>
#include <string>

namespace Assisi::Render
{

/// A snapshot of GPU hardware telemetry. Every numeric field is 0 and `valid`
/// is false when telemetry is unavailable (non-NVIDIA GPU, a driver without
/// NVML, a headless run, etc.) — always check `valid` before displaying.
struct GpuTelemetrySample
{
    bool               valid           = false;
    unsigned long long sequence        = 0;     ///< Increments on each fresh driver query; lets callers detect new samples (Poll() is throttled).
    std::string        name;                   ///< e.g. "NVIDIA GeForce RTX 3070".
    unsigned           coreClockMhz    = 0;     ///< Graphics clock.
    unsigned           memClockMhz     = 0;     ///< Memory clock (as reported by the driver, e.g. 7001 for GDDR6).
    unsigned           gpuUtilPct      = 0;     ///< % of the last window the GPU was busy.
    unsigned           memUtilPct      = 0;     ///< % of the last window memory was being read/written.
    bool               powerSupported  = false; ///< False when this GPU reports no power draw (common on laptops); show N/A, not 0.
    double             powerWatts      = 0.0;   ///< Board power draw (only meaningful when powerSupported).
    double             powerLimitWatts = 0.0;   ///< Enforced power limit, for context.
    unsigned           temperatureC    = 0;
    unsigned long long memUsedBytes    = 0;
    unsigned long long memTotalBytes   = 0;
};

/// Polls NVIDIA GPU telemetry through NVML, which is loaded dynamically at
/// runtime so there is no build- or link-time dependency on it and non-NVIDIA
/// systems simply report an invalid sample. The blocking NVML driver queries run
/// on a background thread that is spun up lazily on the first Poll() (so an app
/// that never opens the telemetry overlay never pays the ~100ms NVML init cost,
/// and even the init happens off the main thread). Poll() itself just returns the
/// latest sample the worker has published — it never touches the driver and never
/// blocks — so the render thread's frame time never absorbs an NVML round-trip.
class GpuTelemetry
{
  public:
    GpuTelemetry();
    ~GpuTelemetry();
    GpuTelemetry(const GpuTelemetry &)            = delete;
    GpuTelemetry &operator=(const GpuTelemetry &) = delete;

    /// The latest sample, re-querying the driver only once the throttle interval
    /// has elapsed since the last query. Lazily initialises NVML on first call.
    const GpuTelemetrySample &Poll();

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace Assisi::Render
