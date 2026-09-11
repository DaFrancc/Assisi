/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SsaoSettings.hpp
/// @brief The knobs on screen-space ambient occlusion.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Assisi::Render
{

/// @name Ranges every SsaoSettings lane is held inside
/// @{

/// Hemisphere samples per pixel. The floor still finds a crease once the blur
/// has averaged sixteen rotations of it; the ceiling is the kernel's capacity in
/// the pass's constant buffer.
inline constexpr uint32_t kMinSsaoSampleCount = 4;
inline constexpr uint32_t kMaxSsaoSampleCount = 32;
inline constexpr uint32_t kDefaultSsaoSampleCount = 12;

/// How far from a surface an occluder is looked for, in metres. Contact scale:
/// the floor is a seam between two tiles, the ceiling a doorway's worth of
/// crease, and past that the answer belongs to a GI provider rather than to
/// the depth buffer.
inline constexpr float kMinSsaoRadius = 0.05f;
inline constexpr float kMaxSsaoRadius = 4.0f;
inline constexpr float kDefaultSsaoRadius = 0.5f;

/// The exponent the visible fraction is raised to. One is the fraction itself.
inline constexpr float kMinSsaoStrength = 0.25f;
inline constexpr float kMaxSsaoStrength = 4.0f;
inline constexpr float kDefaultSsaoStrength = 1.0f;
/// @}

/// @brief Whether screen-space occlusion darkens the indirect term, and how.
///
/// A preference about the machine rather than a fact about the world, so it is
/// saved with the options and not in the level.
struct SsaoSettings
{
    /// Off holds no occlusion target and runs no hemisphere test, and the
    /// indirect term takes the expressions it had before occlusion existed. The
    /// depth prepass belongs to scene depth rather than to this, and still runs
    /// while anything else reads it.
    bool enabled = false;
    uint32_t sampleCount = kDefaultSsaoSampleCount;
    float radius = kDefaultSsaoRadius;
    float strength = kDefaultSsaoStrength;

    bool operator==(const SsaoSettings &) const = default;
};

/// @brief The same settings with every lane in range. Idempotent.
[[nodiscard]] inline SsaoSettings Sanitized(SsaoSettings settings)
{
    const SsaoSettings defaults;
    settings.sampleCount = std::clamp(settings.sampleCount, kMinSsaoSampleCount, kMaxSsaoSampleCount);
    settings.radius =
        std::isfinite(settings.radius) ? std::clamp(settings.radius, kMinSsaoRadius, kMaxSsaoRadius) : defaults.radius;
    settings.strength = std::isfinite(settings.strength)
                            ? std::clamp(settings.strength, kMinSsaoStrength, kMaxSsaoStrength)
                            : defaults.strength;
    return settings;
}

} // namespace Assisi::Render
