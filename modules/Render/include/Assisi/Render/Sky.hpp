/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Sky.hpp
/// @brief The analytic sky: its knobs, and the radiance they produce.
///
/// The model is single-scattering through an atmosphere whose depth along a ray
/// is an air-mass approximation rather than an integral, so a direction costs a
/// handful of exps and no loop. sky.frag transcribes SkyRadiance line for line;
/// the two must agree, and the constants below are the agreement.
///
/// The evaluation is here on the CPU as well as in the shader because the sky is
/// also a light source: the ambient term wants the colour of the sky above a
/// surface and of the ground below it, and that is a query, not a pixel.
///
/// Nothing here bakes the sun in. The sun arrives as SkySun every frame, so a
/// sun that moves takes the sky with it — the daylight colour, the sunset, and
/// the fall to night are all one function of where it is.

#include <Assisi/Math/GLM.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Assisi::Render
{

/// @brief Rayleigh scattering per channel, relative to blue.
///
/// Scattering by air molecules falls as the inverse fourth power of wavelength,
/// which is the entire reason the daytime sky is blue and a low sun is red: blue
/// is scattered out of the direct beam almost six times as readily as red, so
/// what survives a long path is what is left over. Normalised to blue because
/// the absolute coefficient is what SkySettings::zenithOpticalDepth sets.
inline constexpr glm::vec3 kRayleighRatio{0.1752f, 0.4078f, 1.0f};

/// @brief The mean of @ref kRayleighRatio — the colourless part of Rayleigh
/// extinction, which is what sets HOW MUCH of the beam a column of air scatters
/// while the ratio sets WHAT COLOUR comes out.
inline constexpr float kRayleighGrey = (0.1752f + 0.4078f + 1.0f) / 3.0f;

/// @brief Asymmetry of the Mie phase function — how sharply haze throws light
/// forward. Near 1 is a tight halo around the sun; 0 would scatter in every
/// direction equally and stop looking like haze at all.
inline constexpr float kMieAsymmetry = 0.76f;

/// @brief Air mass looking along the horizon, relative to straight up.
///
/// A ray at the horizon passes through roughly thirty-five times the air a ray
/// at the zenith does. The number is the Kasten-Young fit evaluated at ninety
/// degrees; it is named because the twilight extension below continues from it.
inline constexpr float kHorizonAirMass = 35.567f;

/// @brief How fast the sun's air mass grows once it is below the horizon.
///
/// The Kasten-Young fit is only defined down to the horizon, and the sky needs
/// to keep going: this continues it as an exponential in the sun's depth below
/// it, chosen so the beam is extinguished over about the eighteen degrees that
/// separate sunset from full night. It is what makes dusk a fade rather than a
/// switch, and it is an extrapolation — no fit claims this region.
inline constexpr float kTwilightFalloff = 10.0f;

/// @brief Half-width of the band the sky and the ground blend across, in units
/// of the direction's vertical component. About half a degree — wide enough
/// that the horizon is not a stair-step, narrow enough to still read as a line.
inline constexpr float kHorizonSoftness = 0.01f;

/// @name Ranges every SkySettings lane is held inside
///
/// The settings reach a shader, and a non-finite value in any of these lanes
/// takes the whole frame with it.
/// @{

/// Rayleigh optical depth at the zenith, in the blue channel — the one number
/// that sets both how deep the daytime blue is and how far a low sun reddens,
/// since every other direction follows from the air mass along it.
inline constexpr float kMinZenithOpticalDepth = 0.005f;
inline constexpr float kMaxZenithOpticalDepth = 0.5f;
inline constexpr float kDefaultZenithOpticalDepth = 0.07f;

/// Mie extinction per unit air mass — dust and water, which scatter every
/// wavelength alike. Raising it whitens the sky and grows the halo round the sun.
inline constexpr float kMinHaze = 0.0f;
inline constexpr float kMaxHaze = 0.1f;
inline constexpr float kDefaultHaze = 0.005f;

/// Overall multiplier on the sky's radiance. The scene target holds radiance and
/// the tone map is downstream, so this is an exposure of the sky against the
/// rest of the scene rather than a brightness in display terms.
///
/// The default is far from 1 because the model returns the FRACTION of the beam
/// a column of air scatters toward the eye, and for a clear zenith that fraction
/// is a few percent. This is what puts it on the same scale as a directional
/// light of intensity 1, which lights a white surface to about a quarter.
inline constexpr float kMinSkyIntensity = 0.0f;
inline constexpr float kMaxSkyIntensity = 100.0f;
inline constexpr float kDefaultSkyIntensity = 10.0f;

/// What is left of the sky once the sun is gone. Some four orders below the
/// daytime zenith, which is the real ratio and the reason it can be added
/// unconditionally: at noon it is invisible, and no threshold anywhere has to
/// decide when night starts.
inline constexpr glm::vec3 kDefaultNightColor{0.00006f, 0.00010f, 0.00020f};

/// Angular radius of the sun's disk, in degrees. The real sun is about 0.27, but
/// a disk that small lands inside a single pixel at ordinary fields of view and
/// aliases into a crawling dot, so the default is larger.
inline constexpr float kMinSunAngularRadiusDegrees = 0.05f;
inline constexpr float kMaxSunAngularRadiusDegrees = 10.0f;
inline constexpr float kDefaultSunAngularRadiusDegrees = 0.5f;

/// Multiplier on the disk against the sky around it. Zero removes the disk
/// without touching the scattering that surrounds it.
inline constexpr float kMinSunDiskIntensity = 0.0f;
inline constexpr float kMaxSunDiskIntensity = 200.0f;
inline constexpr float kDefaultSunDiskIntensity = 20.0f;
/// @}

/// @brief The sky's look, as an author edits it.
///
/// Scene-look data rather than a user preference: it describes the world, not
/// the machine rendering it, so it is not one of the settings options.json
/// carries.
struct SkySettings
{
    bool enabled = true;

    float zenithOpticalDepth = kDefaultZenithOpticalDepth;
    float haze = kDefaultHaze;

    /// Albedo of the ground half of the sphere, lit by the same sun as the sky.
    /// Not a floor or a real surface — the sky covers every direction, and this
    /// is what the ones pointing down get.
    glm::vec3 groundColor{0.11f, 0.10f, 0.09f};

    /// What is left when the sun has gone. Added rather than blended; see
    /// kDefaultNightColor for why that works.
    glm::vec3 nightColor = kDefaultNightColor;

    float intensity = kDefaultSkyIntensity;
    float sunAngularRadiusDegrees = kDefaultSunAngularRadiusDegrees;
    float sunDiskIntensity = kDefaultSunDiskIntensity;
};

/// @brief The sun the sky is scattering, as the scene supplies it each frame.
///
/// @ref directionToSun points AT the sun — the opposite of the direction a
/// directional light travels. Up is +Y.
struct SkySun
{
    glm::vec3 directionToSun{0.0f, 1.0f, 0.0f};
    glm::vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
};

/// @brief Clamps to [low, high], substituting @p fallback for a non-finite value.
///
/// std::clamp alone returns NaN unchanged — both of its comparisons are false —
/// and one NaN here reaches every pixel of the sky.
[[nodiscard]] inline float ClampFiniteSky(float value, float low, float high, float fallback)
{
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

/// @brief A colour with every channel finite and non-negative.
[[nodiscard]] inline glm::vec3 SanitizedColor(const glm::vec3 &color, const glm::vec3 &fallback)
{
    return glm::vec3(ClampFiniteSky(color.r, 0.0f, kMaxSkyIntensity, fallback.r),
                     ClampFiniteSky(color.g, 0.0f, kMaxSkyIntensity, fallback.g),
                     ClampFiniteSky(color.b, 0.0f, kMaxSkyIntensity, fallback.b));
}

/// @brief The same settings with every lane inside its range. Idempotent:
/// sanitizing a sanitized value changes nothing.
[[nodiscard]] inline SkySettings Sanitized(SkySettings settings)
{
    const SkySettings defaults;
    settings.zenithOpticalDepth = ClampFiniteSky(settings.zenithOpticalDepth, kMinZenithOpticalDepth,
                                                 kMaxZenithOpticalDepth, defaults.zenithOpticalDepth);
    settings.haze = ClampFiniteSky(settings.haze, kMinHaze, kMaxHaze, defaults.haze);
    settings.groundColor = SanitizedColor(settings.groundColor, defaults.groundColor);
    settings.nightColor = SanitizedColor(settings.nightColor, defaults.nightColor);
    settings.intensity = ClampFiniteSky(settings.intensity, kMinSkyIntensity, kMaxSkyIntensity, defaults.intensity);
    settings.sunAngularRadiusDegrees =
        ClampFiniteSky(settings.sunAngularRadiusDegrees, kMinSunAngularRadiusDegrees, kMaxSunAngularRadiusDegrees,
                       defaults.sunAngularRadiusDegrees);
    settings.sunDiskIntensity = ClampFiniteSky(settings.sunDiskIntensity, kMinSunDiskIntensity, kMaxSunDiskIntensity,
                                               defaults.sunDiskIntensity);
    return settings;
}

/// @brief A direction as a unit vector, falling back to straight up rather than
/// producing NaN for a zero-length one.
[[nodiscard]] inline glm::vec3 SafeSkyDirection(const glm::vec3 &direction)
{
    const float lengthSq = glm::dot(direction, direction);
    return lengthSq > 0.0f ? direction / std::sqrt(lengthSq) : glm::vec3(0.0f, 1.0f, 0.0f);
}

/// @brief The same sun with a unit direction and finite, non-negative radiance.
[[nodiscard]] inline SkySun Sanitized(SkySun sun)
{
    const bool finiteDirection = std::isfinite(sun.directionToSun.x) && std::isfinite(sun.directionToSun.y) &&
                                 std::isfinite(sun.directionToSun.z);
    sun.directionToSun = SafeSkyDirection(finiteDirection ? sun.directionToSun : glm::vec3(0.0f, 1.0f, 0.0f));
    sun.color = SanitizedColor(sun.color, glm::vec3(1.0f));
    sun.intensity = ClampFiniteSky(sun.intensity, 0.0f, kMaxSkyIntensity, 1.0f);
    return sun;
}

/// @brief Air mass along a ray, relative to straight up: how much atmosphere it
/// crosses before leaving.
///
/// The Kasten-Young fit down to the horizon, held at its horizon value below it.
/// Holding rather than continuing is deliberate — a downward ray leaves the
/// atmosphere through the ground, and what happens below the horizon is the
/// ground's business, not this function's. @ref SunAirMass is the one that has
/// to keep going.
///
/// @param cosZenith  The ray's vertical component; 1 is straight up.
[[nodiscard]] inline float ViewAirMass(float cosZenith)
{
    const float clamped = std::clamp(cosZenith, -1.0f, 1.0f);
    if (clamped <= 0.0f)
    {
        return kHorizonAirMass;
    }
    const float zenithDegrees = glm::degrees(std::acos(clamped));
    return 1.0f / (clamped + 0.15f * std::pow(93.885f - zenithDegrees, -1.253f));
}

/// @brief Air mass along the beam from the sun, continued below the horizon.
///
/// Above the horizon this is @ref ViewAirMass. Below it the path keeps
/// lengthening as the sun sinks, and this is what darkens and reddens the sky
/// through dusk instead of a separate night mode: the beam is simply extinguished
/// by more and more air until nothing of it arrives.
[[nodiscard]] inline float SunAirMass(float cosZenith)
{
    const float clamped = std::clamp(cosZenith, -1.0f, 1.0f);
    if (clamped >= 0.0f)
    {
        return ViewAirMass(clamped);
    }
    return kHorizonAirMass * std::exp(kTwilightFalloff * -clamped);
}

/// @brief Fraction of each channel that survives @p airMass of atmosphere.
[[nodiscard]] inline glm::vec3 Transmittance(float airMass, float zenithOpticalDepth)
{
    return glm::exp(-kRayleighRatio * (zenithOpticalDepth * airMass));
}

/// @brief Rayleigh phase: how molecular scattering is distributed about the
/// beam. Symmetric — as much comes back toward the sun as goes on past it.
///
/// Scaled to average one over the sphere rather than to integrate to one, so it
/// reads as "relative to scattering the same light in every direction" and the
/// Mie phase beside it is on the same footing.
[[nodiscard]] inline float RayleighPhase(float cosTheta)
{
    return 0.75f * (1.0f + cosTheta * cosTheta);
}

/// @brief Henyey-Greenstein phase: haze, which throws light forward hard. This
/// is the halo around a low sun. Averages one over the sphere, like
/// @ref RayleighPhase.
[[nodiscard]] inline float MiePhase(float cosTheta, float asymmetry)
{
    const float gg = asymmetry * asymmetry;
    const float denom = 1.0f + gg - 2.0f * asymmetry * cosTheta;
    // Clamped because denom reaches zero as asymmetry approaches 1 looking
    // straight at the sun, and the pow would return infinity.
    return (1.0f - gg) / std::pow(std::max(denom, 1e-4f), 1.5f);
}

/// @brief Radiance arriving from @p direction, in linear RGB.
///
/// Finite and non-negative for every direction and every sun position, including
/// a sun below the horizon and a direction pointing straight down.
///
/// @param direction  Unit vector; up is +Y. Need not be normalised.
[[nodiscard]] inline glm::vec3 SkyRadiance(const glm::vec3 &direction, const SkySun &rawSun,
                                           const SkySettings &rawSettings)
{
    const SkySettings settings = Sanitized(rawSettings);
    const SkySun sun = Sanitized(rawSun);
    const glm::vec3 ray = SafeSkyDirection(direction);

    const float cosGamma = std::clamp(glm::dot(ray, sun.directionToSun), -1.0f, 1.0f);
    const float sunAirMass = SunAirMass(sun.directionToSun.y);
    const float viewAirMass = ViewAirMass(ray.y);
    const glm::vec3 radiantSun = sun.color * sun.intensity;

    // What is left of the beam where it meets the ground: the sun's own colour,
    // and the whole reason a low sun is orange.
    const glm::vec3 beam = radiantSun * Transmittance(sunAirMass, settings.zenithOpticalDepth);

    // Light that reaches the eye has crossed the atmosphere twice — in along the
    // beam, out along the view ray — and is extinguished over both. Attenuating
    // the sum rather than the beam alone is what keeps a sunset red: over the
    // short path of a noon zenith the colour is the scattering coefficient's,
    // deep blue, and over the long path of a low sun the exponential wins and
    // what survives is red.
    const glm::vec3 attenuation = Transmittance(sunAirMass + viewAirMass, settings.zenithOpticalDepth);

    // How much air the view ray has to scatter in, which is a quantity and not a
    // colour — the colour is kRayleighRatio's job. Saturating toward one is why
    // the horizon is bright and the zenith, with a thirtieth of the air, is not.
    const float scattered = 1.0f - std::exp(-settings.zenithOpticalDepth * kRayleighGrey * viewAirMass);

    // Haze scatters every wavelength alike and throws it hard forward, so it is
    // grey where it is added and coloured only by the attenuation it shares with
    // the molecular term. That shared attenuation is what makes the halo around
    // a setting sun orange rather than white.
    const float mie = MiePhase(cosGamma, kMieAsymmetry) * (1.0f - std::exp(-settings.haze * viewAirMass)) *
                      std::exp(-settings.haze * (sunAirMass + viewAirMass));

    const glm::vec3 sky =
        radiantSun * attenuation * (kRayleighRatio * (RayleighPhase(cosGamma) * scattered) + glm::vec3(mie)) +
        settings.nightColor;

    // The ground reflects the same beam off a Lambertian albedo, foreshortened by
    // the sun's elevation. It gets the night colour too, so a moonless landscape
    // and the sky over it fall to the same floor instead of the ground going
    // black first.
    const glm::vec3 ground =
        settings.groundColor * (beam * (std::max(sun.directionToSun.y, 0.0f) * glm::one_over_pi<float>())) +
        settings.nightColor;

    const float skyward = glm::smoothstep(-kHorizonSoftness, kHorizonSoftness, ray.y);
    glm::vec3 radiance = glm::mix(ground, sky, skyward);

    // The disk, gated by the same blend so the sun sets behind the ground rather
    // than shining through it. Softened over a tenth of its radius, because an
    // HDR disk with a hard edge aliases into a flickering dot.
    if (settings.sunDiskIntensity > 0.0f)
    {
        const float radius = glm::radians(settings.sunAngularRadiusDegrees);
        const float cosOuter = std::cos(radius * 1.1f);
        const float cosInner = std::cos(radius * 0.9f);
        const float disk = glm::smoothstep(cosOuter, cosInner, cosGamma);
        radiance += beam * (settings.sunDiskIntensity * disk * skyward);
    }

    return glm::max(radiance * settings.intensity, glm::vec3(0.0f));
}

/// @brief The sky pass's constant buffer. Field order, types and packing match
/// sky.frag's SkyConstants block.
struct SkyConstants
{
    /// Inverse of projection * view, which turns a clip-space corner back into a
    /// world-space ray. Inverting the matrix the mesh pass drew with is what
    /// makes the sky land in the same place the geometry did, viewport flip and
    /// all, without this pass knowing anything about either.
    glm::mat4 invViewProjection{1.0f};
    glm::vec4 cameraPosition{0.0f};    ///< xyz = world-space eye, w unused
    glm::vec4 sunDirection{0.0f, 1.0f, 0.0f, 0.0f}; ///< xyz = unit direction TO the sun, w = cos of the disk's outer edge
    glm::vec4 sunRadiance{1.0f};       ///< xyz = colour * intensity, w = cos of the disk's inner edge
    glm::vec4 groundColor{0.0f};       ///< xyz = linear colour, w = zenith optical depth
    glm::vec4 nightColor{0.0f};        ///< xyz = linear colour, w = haze
    glm::vec4 params{0.0f};            ///< x = intensity, y = sun disk intensity, zw unused
};

/// @brief The constants for one sky draw, with everything sanitized on the way in.
[[nodiscard]] inline SkyConstants MakeSkyConstants(const glm::mat4 &invViewProjection, const glm::vec3 &cameraPosition,
                                                   const SkySun &rawSun, const SkySettings &rawSettings)
{
    const SkySettings settings = Sanitized(rawSettings);
    const SkySun sun = Sanitized(rawSun);
    const float radius = glm::radians(settings.sunAngularRadiusDegrees);

    return SkyConstants{
        .invViewProjection = invViewProjection,
        .cameraPosition = glm::vec4(cameraPosition, 0.0f),
        .sunDirection = glm::vec4(sun.directionToSun, std::cos(radius * 1.1f)),
        .sunRadiance = glm::vec4(sun.color * sun.intensity, std::cos(radius * 0.9f)),
        .groundColor = glm::vec4(settings.groundColor, settings.zenithOpticalDepth),
        .nightColor = glm::vec4(settings.nightColor, settings.haze),
        .params = glm::vec4(settings.intensity, settings.sunDiskIntensity, 0.0f, 0.0f)};
}

} // namespace Assisi::Render
