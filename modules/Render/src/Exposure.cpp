/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/Exposure.hpp>

#include <cmath>

namespace Assisi::Render
{
namespace
{

// The stops each counted bin spans.
constexpr float kBinStops =
    (kExposureHistogramMaxLog2 - kExposureHistogramMinLog2) / static_cast<float>(kExposureHistogramBins - 1u);

float FiniteOr(float value, float fallback)
{
    return Math::IsFinite(value) ? value : fallback;
}

float ClampStops(float value, float fallback)
{
    return std::clamp(FiniteOr(value, fallback), kMinExposureSettingStops, kMaxExposureSettingStops);
}

} // namespace

ExposureSettings Sanitized(ExposureSettings settings)
{
    const ExposureSettings defaults;
    settings.manualStops = ClampStops(settings.manualStops, defaults.manualStops);
    settings.automatic = std::clamp(FiniteOr(settings.automatic, defaults.automatic), 0.0f, 1.0f);
    settings.minStops = ClampStops(settings.minStops, defaults.minStops);
    settings.maxStops = std::max(ClampStops(settings.maxStops, defaults.maxStops), settings.minStops);
    settings.compensationStops = ClampStops(settings.compensationStops, defaults.compensationStops);
    settings.adaptationRate = std::clamp(FiniteOr(settings.adaptationRate, defaults.adaptationRate),
                                         kMinExposureAdaptationRate, kMaxExposureAdaptationRate);
    return settings;
}

uint32_t ExposureHistogramBin(float luminance)
{
    // A comparison with not-a-number is false, so it lands in bin 0 too.
    if (!(luminance >= std::exp2(kExposureHistogramMinLog2)))
    {
        return 0u;
    }
    const float position = (std::log2(luminance) - kExposureHistogramMinLog2) / kBinStops;
    const float bin = std::floor(std::min(position, static_cast<float>(kExposureHistogramBins - 2u))) + 1.0f;
    return static_cast<uint32_t>(bin);
}

float ExposureHistogramBinLog2(uint32_t bin)
{
    return kExposureHistogramMinLog2 + (static_cast<float>(bin) - 0.5f) * kBinStops;
}

float MeteredLog2Luminance(std::span<const uint32_t> bins)
{
    float counted = 0.0f;
    for (std::size_t bin = 1; bin < bins.size(); ++bin)
    {
        counted += static_cast<float>(bins[bin]);
    }
    const float low = counted * kExposureMeterLowShare;
    const float high = counted * kExposureMeterHighShare;

    float below = 0.0f;
    float sum = 0.0f;
    float kept = 0.0f;
    for (std::size_t bin = 1; bin < bins.size(); ++bin)
    {
        const float count = static_cast<float>(bins[bin]);
        // The part of this bin's pixels between the two shares.
        const float inside = std::max(std::min(below + count, high) - std::max(below, low), 0.0f);
        sum += inside * ExposureHistogramBinLog2(static_cast<uint32_t>(bin));
        kept += inside;
        below += count;
    }
    return kept > 0.0f ? sum / kept : std::log2(kExposureTargetLuminance);
}

float TargetExposureStops(float meteredLog2, const ExposureSettings &settings)
{
    const float aim = std::log2(kExposureTargetLuminance) - meteredLog2 + settings.compensationStops;
    return std::clamp(aim, settings.minStops, settings.maxStops);
}

float AdaptExposureStops(float current, float target, float rate, float seconds)
{
    const float covered = 1.0f - std::exp(-std::max(rate, 0.0f) * std::max(seconds, 0.0f));
    return current + (target - current) * covered;
}

} // namespace Assisi::Render
