/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file EnvironmentSettings.hpp
/// @brief The knobs on the sky's reflection probe.

#include <Assisi/Render/SpecularIbl.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace Assisi::Render
{

/// @name Ranges every EnvironmentSettings lane is held inside
/// @{

/// Face size of the probe, in texels. A power of two, so every mip halves
/// exactly. The low end still resolves the horizon; the high end is past where
/// a sky with no disk in it has anything left to show.
inline constexpr uint32_t kMinProbeResolution = 16;
inline constexpr uint32_t kMaxProbeResolution = 512;
inline constexpr uint32_t kDefaultProbeResolution = 128;

/// Samples per texel at the first blurred mip; each mip after it takes four
/// times as many, up to kMaxPrefilterSampleCount.
inline constexpr uint32_t kMinProbeSampleCount = 8;
inline constexpr uint32_t kMaxProbeSampleCount = kMaxPrefilterSampleCount;
inline constexpr uint32_t kDefaultProbeSampleCount = 64;

/// How far the sun or the moon may move, in degrees, before the probe is baked
/// again. Zero bakes on every change at all, which is the baseline this is
/// measured against. Half a degree is a quarter of the width of the sharpest
/// lobe the probe stores past its mirror mip, so nothing a surface reflects can
/// be seen to step.
inline constexpr float kMaxProbeRebakeDegrees = 10.0f;
inline constexpr float kDefaultProbeRebakeDegrees = 0.5f;
/// @}

/// @brief Whether the sky is reflected, and how finely.
///
/// A preference about the machine, not a fact about the world: a level does
/// not say how sharp its reflections are, so this is saved with the options
/// rather than in the level.
struct EnvironmentSettings
{
    /// Off draws the indirect term exactly as a sky with no probe does:
    /// hemisphere diffuse and no environment specular, allocated and baked
    /// nothing.
    bool enabled = true;
    uint32_t resolution = kDefaultProbeResolution;
    uint32_t sampleCount = kDefaultProbeSampleCount;
    float rebakeDegrees = kDefaultProbeRebakeDegrees;

    bool operator==(const EnvironmentSettings &) const = default;
};

/// @brief The same settings with every lane in range and the resolution on the
/// nearest power of two. Idempotent.
[[nodiscard]] inline EnvironmentSettings Sanitized(EnvironmentSettings settings)
{
    const EnvironmentSettings defaults;
    const uint32_t clamped = std::clamp(settings.resolution, kMinProbeResolution, kMaxProbeResolution);
    const uint32_t below = std::bit_floor(clamped);
    const uint32_t above = std::bit_ceil(clamped);
    settings.resolution = clamped - below <= above - clamped ? below : above;

    settings.sampleCount = std::clamp(settings.sampleCount, kMinProbeSampleCount, kMaxProbeSampleCount);
    settings.rebakeDegrees = std::isfinite(settings.rebakeDegrees)
                                 ? std::clamp(settings.rebakeDegrees, 0.0f, kMaxProbeRebakeDegrees)
                                 : defaults.rebakeDegrees;
    return settings;
}

} // namespace Assisi::Render
