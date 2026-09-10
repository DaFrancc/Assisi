/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SkyProbeInputs.hpp
/// @brief What a bake of the sky's reflection probe is a function of, and when
/// two of them differ enough to bake again.

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Sky.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Assisi::Render
{

/// @brief How far apart two radiances of the same body may drift, relative to
/// the brighter, before the probe is baked again. A moon waxing or a sun
/// dimming through dusk changes by a little every frame, and two percent of a
/// reflection is below what the eye separates.
inline constexpr float kProbeRadianceTolerance = 0.02f;

/// @brief The sky a probe is baked from: sanitized, and with both disks at zero.
///
/// **The disks are left out** for the reason the ambient leaves them out: each
/// is a directional light, and its highlight is already on every surface from
/// the per-light BRDF. A disk in the probe would put a second sun in every
/// reflection, and on a rough surface smear it into a glow the light never had.
/// Leaving the moon's disk out is also what keeps its texture unread by a bake.
struct SkyProbeInputs
{
    SkySun sun;
    SkyMoon moon;
    SkySettings settings;
};

[[nodiscard]] inline SkyProbeInputs MakeSkyProbeInputs(const SkySun &sun, const SkyMoon &moon,
                                                       const SkySettings &settings)
{
    SkyProbeInputs inputs{.sun = Sanitized(sun), .moon = Sanitized(moon), .settings = Sanitized(settings)};
    inputs.settings.sunDiskIntensity = 0.0f;
    inputs.moon.diskIntensity = 0.0f;
    return inputs;
}

namespace Detail
{
// Every lane of SkySettings is either compared below or read only by the sun's
// disk, which the probe does not hold. A lane added without being sorted into
// one of the two would be a knob whose edits never reach the reflection.
inline constexpr size_t kSkySettingsFloatLanes = 23;
static_assert(sizeof(SkySettings) == kSkySettingsFloatLanes * sizeof(float),
              "A new SkySettings lane must join ProbeSettingsEqual.");

[[nodiscard]] inline bool ProbeSettingsEqual(const SkySettings &a, const SkySettings &b)
{
    return a.airScattering == b.airScattering && a.airThickness == b.airThickness &&
           a.hazeScattering == b.hazeScattering && a.hazeForwardness == b.hazeForwardness &&
           a.skyBounce == b.skyBounce && a.groundColor == b.groundColor && a.nightColor == b.nightColor &&
           a.exposure == b.exposure;
}

/// Whether two unit directions are further apart than @p toleranceDegrees.
/// Zero compares exactly, so an unmoved body never counts as moved.
[[nodiscard]] inline bool ProbeDirectionMoved(const glm::vec3 &a, const glm::vec3 &b, float toleranceDegrees)
{
    if (!(toleranceDegrees > 0.0f))
    {
        return a != b;
    }
    const float cosAngle = std::clamp(glm::dot(a, b), -1.0f, 1.0f);
    return cosAngle < std::cos(glm::radians(toleranceDegrees));
}

/// Whether two radiances differ by more than kProbeRadianceTolerance of the
/// brighter channel. @p exact compares them lane for lane instead.
[[nodiscard]] inline bool ProbeRadianceChanged(const glm::vec3 &a, const glm::vec3 &b, bool exact)
{
    if (exact)
    {
        return a != b;
    }
    const glm::vec3 difference = glm::abs(a - b);
    const glm::vec3 scale = glm::max(glm::abs(a), glm::abs(b));
    for (int32_t i = 0; i < 3; ++i)
    {
        if (difference[i] > kProbeRadianceTolerance * scale[i])
        {
            return true;
        }
    }
    return false;
}
} // namespace Detail

/// @brief Whether a probe baked from @p baked is out of date against @p current.
///
/// Either body moving past @p toleranceDegrees, or its radiance past
/// kProbeRadianceTolerance, and any change at all to the atmosphere. A zero
/// tolerance turns both bands off and compares exactly.
///
/// What only the disks read — their size, tint and the moon's image
/// orientation — is left out: the probe has no disks, so a change there changes
/// nothing it holds.
[[nodiscard]] inline bool SkyProbeInputsDiffer(const SkyProbeInputs &baked, const SkyProbeInputs &current,
                                               float toleranceDegrees)
{
    const bool exact = !(toleranceDegrees > 0.0f);
    return Detail::ProbeDirectionMoved(baked.sun.directionToSun, current.sun.directionToSun, toleranceDegrees) ||
           Detail::ProbeDirectionMoved(baked.moon.directionToMoon, current.moon.directionToMoon, toleranceDegrees) ||
           Detail::ProbeRadianceChanged(baked.sun.color * baked.sun.intensity,
                                        current.sun.color * current.sun.intensity, exact) ||
           Detail::ProbeRadianceChanged(baked.moon.color * baked.moon.intensity,
                                        current.moon.color * current.moon.intensity, exact) ||
           baked.moon.atmosphericTint != current.moon.atmosphericTint ||
           !Detail::ProbeSettingsEqual(baked.settings, current.settings);
}

} // namespace Assisi::Render
