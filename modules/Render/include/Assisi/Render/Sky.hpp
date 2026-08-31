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
/// **Nothing here is Earth by construction.** The scattering coefficients are
/// parameters, not constants, so the air's composition is authored: a different
/// mix gives a different sky, and a different sunset, without a second model or
/// a mode switch. An atmosphere of zero optical depth is the airless case, and
/// it falls out as the limit rather than as a branch — black sky, hard white
/// sun, lit ground.
///
/// The sun arrives as SkySun every frame, so a sun that moves takes the sky with
/// it: the daylight colour, the sunset, and the fall to night are all one
/// function of where it is.

#include <Assisi/Math/GLM.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Assisi::Render
{

/// @brief Earth's Rayleigh scattering per channel, relative to blue.
///
/// Scattering by air molecules falls as the inverse fourth power of wavelength,
/// which is why Earth's daytime sky is blue and a low sun is red: blue is
/// scattered out of the direct beam almost six times as readily as red, so what
/// survives a long path is what is left over.
///
/// A default, not a law. Another world's air scatters in whatever proportion its
/// molecules do, and pushing green here gives a green noon and — with no further
/// authoring — a magenta sunset, because the same coefficient governs both.
inline constexpr glm::vec3 kEarthRayleighCoefficients{0.1752f, 0.4078f, 1.0f};

/// @brief Earth-ish Mie extinction per unit air mass — dust and water droplets.
///
/// Grey by default because Earth's haze is near enough wavelength-independent.
/// Colouring it is what a dust-dominated sky needs: a world whose dust absorbs
/// blue gets a butterscotch day and a blue sunset, which is Mars, and which no
/// amount of Rayleigh authoring reaches.
inline constexpr glm::vec3 kEarthMieCoefficients{0.005f, 0.005f, 0.005f};

/// @brief How fast the sun's air mass grows once it is below the horizon.
///
/// The Kasten-Young fit is only defined down to the horizon, and the sky needs
/// to keep going: this continues it as an exponential in the sun's depth below
/// it, chosen so the beam is extinguished over about the eighteen degrees that
/// separate sunset from full night. It is what makes dusk a fade rather than a
/// switch, and it is an extrapolation — no fit claims this region.
inline constexpr float kTwilightFalloff = 10.0f;

/// @brief Air mass looking along the horizon, relative to straight up.
///
/// A ray at the horizon passes through roughly thirty-five times the air a ray
/// at the zenith does. The number is the Kasten-Young fit evaluated at ninety
/// degrees; it is named because the twilight extension continues from it.
inline constexpr float kHorizonAirMass = 35.567f;

/// @brief Half-width of the band the sky and the ground blend across, in units
/// of the direction's vertical component. About half a degree — wide enough
/// that the horizon is not a stair-step, narrow enough to still read as a line.
inline constexpr float kHorizonSoftness = 0.01f;

/// @name Ranges every SkySettings lane is held inside
///
/// The settings reach a shader, and a non-finite value in any of these lanes
/// takes the whole frame with it.
/// @{

/// Upper bound on a scattering coefficient or a colour channel. Generous, since
/// these describe worlds rather than preferences; it exists to stop a hand-typed
/// value reaching a shader, not to express taste.
inline constexpr float kMaxSkyChannel = 100.0f;

/// Rayleigh optical depth at the zenith, scaling the coefficients above. The one
/// number that sets both how deep the daytime colour is and how far a low sun
/// shifts, since every other direction follows from the air mass along it.
///
/// **Zero is meaningful and reachable**: no atmosphere. The scattering term
/// vanishes, transmittance goes to one, and what is left is a black sky with an
/// unattenuated sun in it.
inline constexpr float kMinZenithOpticalDepth = 0.0f;
inline constexpr float kMaxZenithOpticalDepth = 0.5f;
inline constexpr float kDefaultZenithOpticalDepth = 0.07f;

/// Asymmetry of the Mie phase function — how sharply haze throws light. Positive
/// scatters forward (the halo around a low sun), negative scatters back toward
/// the source, zero is uniform. Held off ±1, where the phase function's
/// denominator reaches zero.
inline constexpr float kMinMieAsymmetry = -0.95f;
inline constexpr float kMaxMieAsymmetry = 0.95f;
inline constexpr float kDefaultMieAsymmetry = 0.76f;

/// How much of the light scattered OUT of the single-scattered beam arrives
/// anyway, having bounced again.
///
/// Single scattering treats a photon knocked out of the line of sight as lost,
/// and over the horizon's thirty-odd air masses that is badly wrong: it kills
/// the blue exactly where the sky is deepest, leaving the horizon green. Real
/// air is not a one-bounce medium. This is the second order and beyond, folded
/// into one term — attenuated by the sun's path but not the view's, and
/// isotropic, because by the time light has bounced twice it has forgotten which
/// way it came.
///
/// Zero is pure single scattering. It costs nothing on an airless world, where
/// there is no scattering to have a second order of.
inline constexpr float kMinMultipleScattering = 0.0f;
inline constexpr float kMaxMultipleScattering = 1.0f;
inline constexpr float kDefaultMultipleScattering = 0.2f;

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

/// Angular radius of the sun's disk, in degrees. Earth's is about 0.27, but a
/// disk that small lands inside a single pixel at ordinary fields of view and
/// aliases into a crawling dot, so the default is larger.
inline constexpr float kMinSunAngularRadiusDegrees = 0.05f;
inline constexpr float kMaxSunAngularRadiusDegrees = 30.0f;
inline constexpr float kDefaultSunAngularRadiusDegrees = 0.5f;

/// Width of the disk's edge fade, as a fraction of its radius.
///
/// The minimum is NOT zero, and that is a requirement rather than a limitation:
/// the disk is the brightest thing in an HDR frame by orders of magnitude, and a
/// hard edge on it aliases into a dot that crawls and flickers as the camera
/// turns. A fade of a few percent of the radius is what stops that.
inline constexpr float kMinSunEdgeSoftness = 0.02f;
inline constexpr float kMaxSunEdgeSoftness = 1.0f;
inline constexpr float kDefaultSunEdgeSoftness = 0.1f;

/// How much dimmer the disk's rim is than its centre.
///
/// A star is a ball of gas, not a sticker: a line of sight at the rim leaves
/// through cooler, higher material than one through the middle, so the rim is
/// genuinely darker. Zero is a flat disk, which is what a uniform circle looks
/// like and why one reads as fake. Earth's sun is about 0.6 in visible light.
inline constexpr float kMinSunLimbDarkening = 0.0f;
inline constexpr float kMaxSunLimbDarkening = 1.0f;
inline constexpr float kDefaultSunLimbDarkening = 0.6f;

/// Multiplier on the disk against the sky around it. Zero removes the disk
/// without touching the scattering that surrounds it.
inline constexpr float kMinSunDiskIntensity = 0.0f;
inline constexpr float kMaxSunDiskIntensity = 200.0f;
inline constexpr float kDefaultSunDiskIntensity = 20.0f;
/// @}

/// @brief The sky's look, as an author edits it.
///
/// Scene data rather than a user preference: it describes the world, not the
/// machine rendering it. It reaches the renderer from a Skybox component on the
/// scene's sun, so it is authored and saved per level.
///
/// Note what is NOT here: the sun's colour and intensity. Those are the
/// DirectionalLight's, because they are one physical quantity — the same value
/// lights every surface in the world, tints the scattering, and colours the
/// disk. A blue sun is a blue directional light, and everything follows.
/// @ref sunDiskColor tints the disk alone, for when that coherence is not what
/// is wanted.
struct SkySettings
{
    /// Scattering per channel by the air itself. See kEarthRayleighCoefficients.
    glm::vec3 rayleighCoefficients = kEarthRayleighCoefficients;
    /// How much air there is, scaling the above. Zero is an airless world.
    float zenithOpticalDepth = kDefaultZenithOpticalDepth;

    /// Extinction per channel by dust and droplets. See kEarthMieCoefficients.
    glm::vec3 mieCoefficients = kEarthMieCoefficients;
    float mieAsymmetry = kDefaultMieAsymmetry;

    /// See kDefaultMultipleScattering. Without it the horizon goes green.
    float multipleScattering = kDefaultMultipleScattering;

    /// Albedo of the ground half of the sphere, lit by the same sun as the sky.
    /// Not a floor or a real surface — the sky covers every direction, and this
    /// is what the ones pointing down get.
    glm::vec3 groundColor{0.11f, 0.10f, 0.09f};

    /// What is left when the sun has gone. Added rather than blended; see
    /// kDefaultNightColor for why that works.
    glm::vec3 nightColor = kDefaultNightColor;

    float intensity = kDefaultSkyIntensity;

    /// @name The disk
    /// @{
    float sunAngularRadiusDegrees = kDefaultSunAngularRadiusDegrees;
    float sunEdgeSoftness = kDefaultSunEdgeSoftness;
    float sunLimbDarkening = kDefaultSunLimbDarkening;
    float sunDiskIntensity = kDefaultSunDiskIntensity;
    /// Tint on the disk ALONE, multiplied over the sun's own colour. White
    /// leaves the disk agreeing with the light that lights the world; pushing it
    /// is the artistic escape hatch for a yellow sun over a neutrally-lit scene.
    /// Warm by default. A star's disk read against a blue sky looks yellow to
    /// the eye, and a pure white one reads as a hole rather than a sun.
    glm::vec3 sunDiskColor{1.0f, 0.95f, 0.82f};
    /// @}
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

/// @brief A colour or coefficient triple with every channel finite and in range.
[[nodiscard]] inline glm::vec3 SanitizedSkyChannels(const glm::vec3 &value, const glm::vec3 &fallback)
{
    return glm::vec3(ClampFiniteSky(value.r, 0.0f, kMaxSkyChannel, fallback.r),
                     ClampFiniteSky(value.g, 0.0f, kMaxSkyChannel, fallback.g),
                     ClampFiniteSky(value.b, 0.0f, kMaxSkyChannel, fallback.b));
}

/// @brief The same settings with every lane inside its range. Idempotent:
/// sanitizing a sanitized value changes nothing.
[[nodiscard]] inline SkySettings Sanitized(SkySettings settings)
{
    const SkySettings defaults;
    settings.rayleighCoefficients =
        SanitizedSkyChannels(settings.rayleighCoefficients, defaults.rayleighCoefficients);
    settings.zenithOpticalDepth = ClampFiniteSky(settings.zenithOpticalDepth, kMinZenithOpticalDepth,
                                                 kMaxZenithOpticalDepth, defaults.zenithOpticalDepth);
    settings.mieCoefficients = SanitizedSkyChannels(settings.mieCoefficients, defaults.mieCoefficients);
    settings.mieAsymmetry =
        ClampFiniteSky(settings.mieAsymmetry, kMinMieAsymmetry, kMaxMieAsymmetry, defaults.mieAsymmetry);
    settings.multipleScattering = ClampFiniteSky(settings.multipleScattering, kMinMultipleScattering,
                                                 kMaxMultipleScattering, defaults.multipleScattering);
    settings.groundColor = SanitizedSkyChannels(settings.groundColor, defaults.groundColor);
    settings.nightColor = SanitizedSkyChannels(settings.nightColor, defaults.nightColor);
    settings.intensity = ClampFiniteSky(settings.intensity, kMinSkyIntensity, kMaxSkyIntensity, defaults.intensity);
    settings.sunAngularRadiusDegrees =
        ClampFiniteSky(settings.sunAngularRadiusDegrees, kMinSunAngularRadiusDegrees, kMaxSunAngularRadiusDegrees,
                       defaults.sunAngularRadiusDegrees);
    settings.sunEdgeSoftness =
        ClampFiniteSky(settings.sunEdgeSoftness, kMinSunEdgeSoftness, kMaxSunEdgeSoftness, defaults.sunEdgeSoftness);
    settings.sunLimbDarkening = ClampFiniteSky(settings.sunLimbDarkening, kMinSunLimbDarkening, kMaxSunLimbDarkening,
                                               defaults.sunLimbDarkening);
    settings.sunDiskIntensity = ClampFiniteSky(settings.sunDiskIntensity, kMinSunDiskIntensity, kMaxSunDiskIntensity,
                                               defaults.sunDiskIntensity);
    settings.sunDiskColor = SanitizedSkyChannels(settings.sunDiskColor, defaults.sunDiskColor);
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
    sun.color = SanitizedSkyChannels(sun.color, glm::vec3(1.0f));
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
/// lengthening as the sun sinks, and this is what darkens and shifts the sky
/// through dusk instead of a separate night mode: the beam is simply
/// extinguished by more and more air until nothing of it arrives.
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
[[nodiscard]] inline glm::vec3 Transmittance(const glm::vec3 &coefficients, float airMass)
{
    return glm::exp(-coefficients * airMass);
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

/// @brief Henyey-Greenstein phase: haze, which throws light forward hard at a
/// positive asymmetry and back toward the source at a negative one. Averages one
/// over the sphere, like @ref RayleighPhase.
[[nodiscard]] inline float MiePhase(float cosTheta, float asymmetry)
{
    const float gg = asymmetry * asymmetry;
    const float denom = 1.0f + gg - 2.0f * asymmetry * cosTheta;
    // Clamped because denom reaches zero as the asymmetry approaches one looking
    // straight along the beam, and the pow would return infinity.
    return (1.0f - gg) / std::pow(std::max(denom, 1e-4f), 1.5f);
}

/// @brief The disk's brightness at @p angleToSun radians off its centre, in
/// [0, 1]. Zero everywhere outside it.
///
/// Two effects, and they are not the same one: the edge fade is antialiasing,
/// and the limb darkening is what a sphere looks like. A disk with the first and
/// not the second is a soft-edged sticker.
[[nodiscard]] inline float SunDiskProfile(float angleToSun, float radius, float edgeSoftness, float limbDarkening)
{
    const float edge = 1.0f - glm::smoothstep(radius * (1.0f - edgeSoftness), radius * (1.0f + edgeSoftness),
                                              angleToSun);
    if (edge <= 0.0f)
    {
        return 0.0f;
    }
    // How far across the visible face this line of sight lands, and then the
    // cosine of the angle it makes with the surface there. A ray at the rim
    // leaves through cooler material and carries less of it out.
    const float acrossFace = std::min(angleToSun / std::max(radius, 1e-6f), 1.0f);
    const float faceCosine = std::sqrt(std::max(1.0f - acrossFace * acrossFace, 0.0f));
    return edge * (1.0f - limbDarkening * (1.0f - faceCosine));
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

    // Extinction per channel, and the colourless part of it. The first says what
    // colour the air takes out; the second says how much of the beam it scatters
    // rather than transmits, which is a quantity and not a hue.
    const glm::vec3 rayleigh = settings.rayleighCoefficients * settings.zenithOpticalDepth;
    const float rayleighGrey = (rayleigh.r + rayleigh.g + rayleigh.b) / 3.0f;

    // What is left of the beam where it meets the ground: the sun's own colour,
    // and the whole reason a low sun shifts hue.
    const glm::vec3 beam = radiantSun * Transmittance(rayleigh, sunAirMass);

    // Light that reaches the eye crossed the atmosphere twice — in along the
    // beam, out along the view ray — and is extinguished over both. Attenuating
    // the sum rather than the beam alone is what keeps a sunset red on Earth:
    // over the short path of a noon zenith the colour is the scattering
    // coefficient's, and over the long path of a low sun the exponential wins
    // and what survives is whatever that coefficient scatters LEAST.
    const glm::vec3 attenuation = Transmittance(rayleigh, sunAirMass + viewAirMass);

    // How much air the view ray has to scatter in. Saturating toward one is why
    // the horizon is bright and the zenith, with a thirtieth of the air, is not.
    const float scattered = 1.0f - std::exp(-rayleighGrey * viewAirMass);

    // Haze, with its own extinction and its own colour. It shares the molecular
    // attenuation, which is what makes the halo around a setting sun take the
    // sun's colour rather than staying white.
    const glm::vec3 mie = (glm::vec3(1.0f) - Transmittance(settings.mieCoefficients, viewAirMass)) *
                          Transmittance(settings.mieCoefficients, sunAirMass + viewAirMass) *
                          MiePhase(cosGamma, settings.mieAsymmetry);

    // The second bounce onward, attenuated by the sun's path but not the view's
    // — which is the whole point, since it is at the horizon, where the
    // round-trip term has died, that the sky would otherwise turn green.
    const glm::vec3 multiScattered =
        beam * (settings.rayleighCoefficients * (scattered * settings.multipleScattering));

    const glm::vec3 sky =
        radiantSun * attenuation * (settings.rayleighCoefficients * (RayleighPhase(cosGamma) * scattered) + mie) +
        multiScattered + settings.nightColor;

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
    // than shining up through it.
    if (settings.sunDiskIntensity > 0.0f)
    {
        const float profile = SunDiskProfile(std::acos(cosGamma), glm::radians(settings.sunAngularRadiusDegrees),
                                             settings.sunEdgeSoftness, settings.sunLimbDarkening);
        radiance += beam * settings.sunDiskColor * (settings.sunDiskIntensity * profile * skyward);
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
    glm::vec4 cameraPosition{0.0f};                 ///< xyz = world-space eye, w unused
    glm::vec4 sunDirection{0.0f, 1.0f, 0.0f, 0.0f}; ///< xyz = unit direction TO the sun, w = disk radius (radians)
    glm::vec4 sunRadiance{1.0f};                    ///< xyz = colour * intensity, w = disk edge softness
    glm::vec4 rayleigh{0.0f};                       ///< xyz = coefficients * optical depth, w = their mean
    glm::vec4 mie{0.0f};                            ///< xyz = coefficients, w = asymmetry
    glm::vec4 groundColor{0.0f};                    ///< xyz = linear colour, w = sky intensity
    glm::vec4 nightColor{0.0f};                     ///< xyz = linear colour, w = disk intensity
    glm::vec4 sunDiskColor{1.0f};                   ///< xyz = disk tint, w = limb darkening
    glm::vec4 atmosphere{0.0f};                     ///< x = multiple scattering, yzw unused
};

/// @brief The constants for one sky draw, with everything sanitized on the way in.
[[nodiscard]] inline SkyConstants MakeSkyConstants(const glm::mat4 &invViewProjection, const glm::vec3 &cameraPosition,
                                                   const SkySun &rawSun, const SkySettings &rawSettings)
{
    const SkySettings settings = Sanitized(rawSettings);
    const SkySun sun = Sanitized(rawSun);

    // Premultiplied here rather than in the shader: it is the same product for
    // every pixel, and the mean beside it would otherwise be three more adds per
    // fragment for a value that cannot change across the draw.
    const glm::vec3 rayleigh = settings.rayleighCoefficients * settings.zenithOpticalDepth;
    const float rayleighGrey = (rayleigh.r + rayleigh.g + rayleigh.b) / 3.0f;

    return SkyConstants{
        .invViewProjection = invViewProjection,
        .cameraPosition = glm::vec4(cameraPosition, 0.0f),
        .sunDirection = glm::vec4(sun.directionToSun, glm::radians(settings.sunAngularRadiusDegrees)),
        .sunRadiance = glm::vec4(sun.color * sun.intensity, settings.sunEdgeSoftness),
        .rayleigh = glm::vec4(rayleigh, rayleighGrey),
        .mie = glm::vec4(settings.mieCoefficients, settings.mieAsymmetry),
        .groundColor = glm::vec4(settings.groundColor, settings.intensity),
        .nightColor = glm::vec4(settings.nightColor, settings.sunDiskIntensity),
        .sunDiskColor = glm::vec4(settings.sunDiskColor, settings.sunLimbDarkening),
        .atmosphere = glm::vec4(settings.multipleScattering, 0.0f, 0.0f, 0.0f)};
}

} // namespace Assisi::Render
