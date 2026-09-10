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
/// function of where it is. Where it is comes from the scene — from a Sun
/// component and the clock beside it, or from the light's own authored aim; see
/// Runtime::ResolveSky.
///
/// **Two bodies scatter, and they are added rather than switched between.**
/// Scattering is linear in the beam, so the sun's contribution plus the moon's is
/// the exact answer — and it is the only one with no seam in it. Handing the sky
/// to "whichever body is brighter" would replace the sunset's afterglow, which
/// the twilight extension carries eighteen degrees below the horizon, with the
/// moon's beam in a single frame. At noon the moon's term is a fiftieth of the
/// sun's and at midnight the sun's is eleven orders down, so neither is ever
/// worth a branch.

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Celestial.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
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
inline constexpr glm::vec3 kEarthAirScattering{0.1752f, 0.4078f, 1.0f};

/// @brief Earth-ish Mie extinction per unit air mass — dust and water droplets.
///
/// Grey by default because Earth's haze is near enough wavelength-independent.
/// Colouring it is what a dust-dominated sky needs: a world whose dust absorbs
/// blue gets a butterscotch day and a blue sunset, which is Mars, and which no
/// amount of Rayleigh authoring reaches.
inline constexpr glm::vec3 kEarthHazeScattering{0.005f, 0.005f, 0.005f};

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
/// It is also what sets WHEN the sun turns golden. The reddening follows the
/// beam's path length, so a thicker atmosphere reaches any given warmth with the
/// sun still higher: at 0.07 the disk is barely warm until about 15 degrees, at
/// 0.14 it is golden by 30. Raising it does not cost the daytime sky — multiple
/// scattering keeps the horizon blue, and the zenith deepens rather than dulls.
///
/// **Zero is meaningful and reachable**: no atmosphere. The scattering term
/// vanishes, transmittance goes to one, and what is left is a black sky with an
/// unattenuated sun in it.
inline constexpr float kMinAirThickness = 0.0f;
inline constexpr float kMaxAirThickness = 0.5f;
inline constexpr float kDefaultAirThickness = 0.14f;

/// Asymmetry of the Mie phase function — how sharply haze throws light. Positive
/// scatters forward (the halo around a low sun), negative scatters back toward
/// the source, zero is uniform. Held off ±1, where the phase function's
/// denominator reaches zero.
inline constexpr float kMinHazeForwardness = -0.95f;
inline constexpr float kMaxHazeForwardness = 0.95f;
inline constexpr float kDefaultHazeForwardness = 0.76f;

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
inline constexpr float kMinSkyBounce = 0.0f;
inline constexpr float kMaxSkyBounce = 1.0f;
inline constexpr float kDefaultSkyBounce = 0.2f;

/// Overall multiplier on the sky's radiance. The scene target holds radiance and
/// the tone map is downstream, so this is an exposure of the sky against the
/// rest of the scene rather than a brightness in display terms.
///
/// The default is far from 1 because the model returns the FRACTION of the beam
/// a column of air scatters toward the eye, and for a clear zenith that fraction
/// is a few percent. This is what puts it on the same scale as a directional
/// light of intensity 1, which lights a white surface to about a quarter.
inline constexpr float kMinSkyExposure = 0.0f;
inline constexpr float kMaxSkyExposure = 100.0f;
inline constexpr float kDefaultSkyExposure = 10.0f;

/// What is left of the sky once the sun is gone. Some four orders below the
/// daytime zenith, which is the real ratio and the reason it can be added
/// unconditionally: at noon it is invisible, and no threshold anywhere has to
/// decide when night starts.
inline constexpr glm::vec3 kDefaultNightColor{0.00006f, 0.00010f, 0.00020f};

/// Angular radius of the sun's disk, in degrees. Earth's is about 0.27, but a
/// disk that small lands inside a single pixel at ordinary fields of view and
/// aliases into a crawling dot, so the default is larger.
inline constexpr float kMinSunSizeDegrees = 0.05f;
inline constexpr float kMaxSunSizeDegrees = 30.0f;
inline constexpr float kDefaultSunSizeDegrees = 0.5f;

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

/// Smallest disk radius, in **radians**, that anything divides by. A disk this
/// small covers no pixel at any field of view, so the only thing left to get
/// right about it is that the across-the-face coordinate stays finite.
inline constexpr float kMinDiskRadius = 1e-6f;

/// The moon shares the sun's ranges — a disk is a disk, and a moon larger than
/// its star is a legitimate world — but not its defaults. Half a degree is the
/// real moon's own size, which happens to be the sun's too.
inline constexpr float kDefaultMoonSizeDegrees = 0.5f;
/// Far below the sun's, because the disk is multiplied by an albedo photograph
/// averaging a little over half rather than by the surface of a star.
inline constexpr float kDefaultMoonDiskIntensity = 2.0f;

/// How much of the air's colour shift a low moon takes, by default.
///
/// **Well under one, and that is a correction rather than a preference.** The
/// physics says a low moon reddens exactly as hard as a low sun — same air, same
/// path, same exponential — and photographs of a moonrise bear that out. What
/// the physics does NOT say is that anyone sees it: real moonlight is about a
/// millionth of sunlight, which is below where colour vision works at all, so
/// the reddening happens almost entirely in the grey.
///
/// This engine cannot show that, because a moon at its real intensity is an
/// unplayably dark night. Moon::intensity is raised some four orders of
/// magnitude to compensate — and raising the brightness without touching the
/// chroma presents, at full daylight saturation, a shift that in life is seen
/// nearly colourless. That mismatch is what makes a low moon here read as a
/// small sunset.
///
/// The knob is the missing half of the intensity compromise. One is the honest
/// physics for a project that wants it.
inline constexpr float kDefaultMoonAtmosphericTint = 0.2f;
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
    /// Scattering per channel by the air itself. See kEarthAirScattering.
    glm::vec3 airScattering = kEarthAirScattering;
    /// How much air there is, scaling the above. Zero is an airless world.
    float airThickness = kDefaultAirThickness;

    /// Extinction per channel by dust and droplets. See kEarthHazeScattering.
    ///
    /// Honest up to about 0.06. Past that the model inverts the sky's gradient —
    /// the horizon goes DARKER than the zenith, where real haze brightens it. The
    /// cause is single scattering: thick haze extinguishes the beam over the
    /// horizon's long path, and what keeps real fog bright is the light bouncing
    /// inside it many times, which this has no way to follow. True overcast is
    /// out of reach for the same reason.
    glm::vec3 hazeScattering = kEarthHazeScattering;
    float hazeForwardness = kDefaultHazeForwardness;

    /// See kDefaultSkyBounce. Without it the horizon goes green.
    float skyBounce = kDefaultSkyBounce;

    /// Albedo of the ground half of the sphere, lit by the same sun as the sky.
    /// Not a floor or a real surface — the sky covers every direction, and this
    /// is what the ones pointing down get.
    glm::vec3 groundColor{0.11f, 0.10f, 0.09f};

    /// What is left when the sun has gone. Added rather than blended; see
    /// kDefaultNightColor for why that works.
    glm::vec3 nightColor = kDefaultNightColor;

    float exposure = kDefaultSkyExposure;

    /// @name The disk
    /// @{
    float sunSizeDegrees = kDefaultSunSizeDegrees;
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

/// @brief The moon the sky is scattering and drawing, as the scene supplies it
/// each frame.
///
/// A default-constructed one is **no moon at all**: both intensities are zero, so
/// it scatters nothing, draws nothing, and every function taking one gives bit
/// for bit what the moonless form gives. That is what lets the three-argument sky
/// stay the sky of a level with no moon in it rather than an older API kept
/// alive.
///
/// Unlike the sun, the moon carries its own size and tint. The sun's live in
/// SkySettings because the atmosphere and its star are one authored thing; a moon
/// is a body in the sky, and a level may have one without changing its air.
struct SkyMoon
{
    glm::vec3 directionToMoon{0.0f, 1.0f, 0.0f};

    /// Which way the top of the disk's image points, tangent to the sky at the
    /// moon. Sanitized re-projects it perpendicular to the direction, so a caller
    /// may hand over any vector that leans the right way.
    ///
    /// It is a whole vector rather than an angle because the frame it comes from
    /// — the ecliptic pole, projected — has no meaning to a shader; see
    /// Celestial.hpp's MoonImageUp, which is what produces it.
    glm::vec3 imageUp{0.0f, 0.0f, -1.0f};

    /// The moon's light above the atmosphere, on the same scale as the sun's, so
    /// intensity 1 would be a second sun. Real moonlight is nearer 2.5e-6, which
    /// is unplayably dark; what ships is a compromise and is meant to be.
    glm::vec3 color{1.0f};
    float intensity = 0.0f;

    float sizeDegrees = kDefaultMoonSizeDegrees;

    /// A tint OVER the albedo texture, not a replacement for it. White leaves the
    /// photograph's own colour, which is what it is there for.
    glm::vec3 diskColor{1.0f};
    /// Zero draws no disk, which is also what a scene with no moon supplies.
    float diskIntensity = 0.0f;

    /// How much of the air's colour shift this moon takes, in [0, 1]. See
    /// kDefaultMoonAtmosphericTint for why it is not one.
    ///
    /// It changes hue and nothing else — the dimming is untouched at every value,
    /// so a low moon is exactly as dim whichever way this is set. What it does
    /// not do is soften the sky, the scattering or the sun; those stay physical.
    float atmosphericTint = kDefaultMoonAtmosphericTint;
};

/// @brief @p transmittance with @p tint of its colour shift left in, and all of
/// its dimming.
///
/// Mixing toward the transmittance's OWN luminance rather than toward white is
/// what makes this purely a hue control: luminance is linear, so the mix has the
/// same luminance at every tint, and turning the knob can never brighten or
/// darken a scene. At zero a body dims through the air without changing colour;
/// at one it reddens exactly as the physics says.
[[nodiscard]] inline glm::vec3 TintedTransmittance(const glm::vec3 &transmittance, float tint)
{
    // Returned untouched at full tint, and that has to be exact rather than
    // merely close: the sun always passes one, so a mix that reconstructed its
    // input to within an ulp would shift every existing sunset by that ulp.
    if (!(tint < 1.0f))
    {
        return transmittance;
    }
    const float grey = glm::dot(transmittance, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    return glm::mix(glm::vec3(grey), transmittance, std::max(tint, 0.0f));
}

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
    settings.airScattering =
        SanitizedSkyChannels(settings.airScattering, defaults.airScattering);
    settings.airThickness = ClampFiniteSky(settings.airThickness, kMinAirThickness,
                                           kMaxAirThickness, defaults.airThickness);
    settings.hazeScattering = SanitizedSkyChannels(settings.hazeScattering, defaults.hazeScattering);
    settings.hazeForwardness =
        ClampFiniteSky(settings.hazeForwardness, kMinHazeForwardness, kMaxHazeForwardness, defaults.hazeForwardness);
    settings.skyBounce = ClampFiniteSky(settings.skyBounce, kMinSkyBounce,
                                        kMaxSkyBounce, defaults.skyBounce);
    settings.groundColor = SanitizedSkyChannels(settings.groundColor, defaults.groundColor);
    settings.nightColor = SanitizedSkyChannels(settings.nightColor, defaults.nightColor);
    settings.exposure = ClampFiniteSky(settings.exposure, kMinSkyExposure, kMaxSkyExposure, defaults.exposure);
    settings.sunSizeDegrees =
        ClampFiniteSky(settings.sunSizeDegrees, kMinSunSizeDegrees, kMaxSunSizeDegrees,
                       defaults.sunSizeDegrees);
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
    sun.intensity = ClampFiniteSky(sun.intensity, 0.0f, kMaxSkyExposure, 1.0f);
    return sun;
}

/// @brief The same moon with unit, mutually perpendicular direction and image up,
/// and finite radiance in range.
///
/// The re-projection of @ref SkyMoon::imageUp is not defensive tidying: the up
/// arrives from a different computation than the direction, and a pair that is
/// not perpendicular skews the disk's image rather than rotating it. An up that
/// arrives parallel to the direction has no information in it, and any
/// perpendicular is substituted — an arbitrary rotation of the picture, which is
/// the mildest wrong answer available.
[[nodiscard]] inline SkyMoon Sanitized(SkyMoon moon)
{
    const SkyMoon defaults;
    const bool finiteDirection = std::isfinite(moon.directionToMoon.x) && std::isfinite(moon.directionToMoon.y) &&
                                 std::isfinite(moon.directionToMoon.z);
    moon.directionToMoon = SafeSkyDirection(finiteDirection ? moon.directionToMoon : defaults.directionToMoon);

    const bool finiteUp =
        std::isfinite(moon.imageUp.x) && std::isfinite(moon.imageUp.y) && std::isfinite(moon.imageUp.z);
    const glm::vec3 up = finiteUp ? moon.imageUp : defaults.imageUp;
    const glm::vec3 tangent = up - moon.directionToMoon * glm::dot(moon.directionToMoon, up);
    const float tangentLength = std::sqrt(glm::dot(tangent, tangent));
    moon.imageUp =
        tangentLength > kMinTangentLength ? tangent / tangentLength : Detail::AnyPerpendicular(moon.directionToMoon);

    moon.color = SanitizedSkyChannels(moon.color, defaults.color);
    moon.intensity = ClampFiniteSky(moon.intensity, 0.0f, kMaxSkyExposure, defaults.intensity);
    moon.sizeDegrees =
        ClampFiniteSky(moon.sizeDegrees, kMinSunSizeDegrees, kMaxSunSizeDegrees, defaults.sizeDegrees);
    moon.diskColor = SanitizedSkyChannels(moon.diskColor, defaults.diskColor);
    moon.diskIntensity =
        ClampFiniteSky(moon.diskIntensity, kMinSunDiskIntensity, kMaxSunDiskIntensity, defaults.diskIntensity);
    moon.atmosphericTint = ClampFiniteSky(moon.atmosphericTint, 0.0f, 1.0f, defaults.atmosphericTint);
    return moon;
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
    const float acrossFace = std::min(angleToSun / std::max(radius, kMinDiskRadius), 1.0f);
    const float faceCosine = std::sqrt(std::max(1.0f - acrossFace * acrossFace, 0.0f));
    return edge * (1.0f - limbDarkening * (1.0f - faceCosine));
}

/// @brief How far across the moon's visible face a ray lands, in units of its
/// radius: zero at the centre, one at the limb, past one outside it.
///
/// The same coordinate SunDiskProfile computes internally, which is what makes
/// the texture's edge and the silhouette's edge the same edge. Deliberately not
/// clamped — the disk image is sampled with a clamping sampler, and letting the
/// coordinate run past one is how a ray in the profile's edge fade gets the
/// image's border rather than a smear of its rim.
[[nodiscard]] inline float MoonDiskAcross(float angleToMoon, float radius)
{
    return angleToMoon / std::max(radius, kMinDiskRadius);
}

/// @brief Where on the moon's disk image a ray lands, in [0, 1]² inside the disk
/// and past it outside. V is zero at the top, matching how a texture is uploaded.
///
/// The photograph is an orthographic view of a tidally locked hemisphere and this
/// coordinate is an orthographic image plane, so a pixel of image is a pixel of
/// moon with no resampling model in between: no equirectangular unwrap, no
/// arc tangent, and no far side stored for a face that never turns toward
/// anybody. The limb's foreshortening is already in the picture.
///
/// The CPU form of what sky.frag samples with, kept here so the handedness and
/// the sign of V — the two ways a moon comes out mirrored or upside down — can be
/// asserted rather than looked at.
[[nodiscard]] inline glm::vec2 MoonDiskUv(const glm::vec3 &ray, const SkyMoon &rawMoon, float radius)
{
    const SkyMoon moon = Sanitized(rawMoon);
    // forward × up = right, as a camera looking along the direction to the moon
    // with the image's top upward would have it.
    const glm::vec3 right = glm::cross(moon.directionToMoon, moon.imageUp);

    const float cosGamma = std::clamp(glm::dot(ray, moon.directionToMoon), -1.0f, 1.0f);
    const float across = MoonDiskAcross(std::acos(cosGamma), radius);
    const glm::vec3 offset = ray - moon.directionToMoon * cosGamma;
    const float offsetLength = std::sqrt(glm::dot(offset, offset));
    const glm::vec3 sideways = offsetLength > kMinTangentLength ? offset / offsetLength : glm::vec3(0.0f);

    return glm::vec2(0.5f + 0.5f * across * glm::dot(sideways, right),
                     0.5f - 0.5f * across * glm::dot(sideways, moon.imageUp));
}

/// @brief How lit the moon's surface is where @p ray meets it, in [0, 1].
///
/// Flat across the lit face with a narrow terminator, not a cosine. Regolith is
/// retro-reflective and the real moon is very nearly uniform right out to the
/// limb; shading it by `max(dot(n, s), 0)` produces a billiard ball, which is the
/// single most recognisable way a rendered moon goes wrong. Where the terminator
/// falls is still the geometry — the sphere's own normal against the direction to
/// the sun — so the phase cannot drift out of step with the sky.
///
/// @param radius  The disk's angular radius in radians, as SunDiskProfile takes it.
[[nodiscard]] inline float MoonDiskLit(const glm::vec3 &ray, const glm::vec3 &toMoon, const glm::vec3 &toSun,
                                       float radius)
{
    const float cosGamma = std::clamp(glm::dot(ray, toMoon), -1.0f, 1.0f);
    // Clamped here, where the coordinate has to name a point ON the sphere: past
    // the limb there is no surface for a normal to belong to.
    const float across = std::min(MoonDiskAcross(std::acos(cosGamma), radius), 1.0f);
    const float faceCosine = std::sqrt(std::max(1.0f - across * across, 0.0f));

    const glm::vec3 offset = ray - toMoon * cosGamma;
    const float offsetLength = std::sqrt(glm::dot(offset, offset));
    const glm::vec3 sideways = offsetLength > kMinTangentLength ? offset / offsetLength : glm::vec3(0.0f);

    // Outward from the sphere's centre: back along the line of sight by the
    // cosine, sideways by how far across the face the ray landed.
    const glm::vec3 normal = sideways * across - toMoon * faceCosine;
    return glm::smoothstep(-kMoonTerminatorSoftness, kMoonTerminatorSoftness, glm::dot(normal, toSun));
}

/// @brief Everything in the air that takes light out of a straight line, per
/// channel.
///
/// The molecules AND what is suspended among them. Haze belongs here and not
/// only in the glow it adds: dust and water dim the sun as surely as air does,
/// and an atmosphere that scattered a bright halo while letting the beam through
/// undimmed would light an overcast noon like a clear one.
[[nodiscard]] inline glm::vec3 BeamExtinction(const SkySettings &settings)
{
    return settings.airScattering * settings.airThickness + settings.hazeScattering;
}

/// @brief The fraction of the sun's light, per channel, that survives the
/// atmosphere on its way to the ground.
///
/// A colour multiplier and nothing else: it dims as well as tints, because a
/// long path through air takes light out rather than merely reddening it, and
/// both come off the one exponential.
///
/// @param directionToSun  Unit vector pointing AT the sun; up is +Y.
[[nodiscard]] inline glm::vec3 SunlightTransmittance(const glm::vec3 &directionToSun,
                                                     const SkySettings &rawSettings)
{
    const SkySettings settings = Sanitized(rawSettings);
    return Transmittance(BeamExtinction(settings), SunAirMass(SafeSkyDirection(directionToSun).y));
}

/// @brief The sun's light where it reaches the ground, having crossed the
/// atmosphere on the way in.
///
/// The same quantity the sky scatters, which is the point of it being one
/// function: a world lit by this and a sky drawn from the same beam agree at
/// every time of day without anything keeping them in step. At noon it is very
/// nearly the sun's own colour; at sunset the air has taken most of the blue out
/// of it, so it is dimmer AND oranger, both from the one exponential.
[[nodiscard]] inline glm::vec3 SunlightAtGround(const SkySun &rawSun, const SkySettings &rawSettings)
{
    const SkySun sun = Sanitized(rawSun);
    return sun.color * sun.intensity * SunlightTransmittance(sun.directionToSun, rawSettings);
}

/// @brief What one celestial body contributes to one ray, before the horizon
/// blend and before either disk.
///
/// Split out because the sky has two bodies in it and they are added: scattering
/// is linear in the beam, so calling this twice and summing is not an
/// approximation of a two-body sky, it IS the two-body sky. The alternative —
/// handing the scattering to whichever body dominates — has a seam in it at the
/// handoff, and the seam falls exactly on the sunset.
struct SkyBodyRadiance
{
    /// What is left of the body's light where it meets the ground: its own colour
    /// shifted by its own path, and the whole reason a low sun goes orange. The
    /// same value a Skybox hands the directional light when asked to tint it.
    glm::vec3 beam{0.0f};
    /// The fraction of the beam that survived, on its own, after @p tint.
    glm::vec3 transmittance{1.0f};
    /// What the air sends toward the eye from this body's beam.
    glm::vec3 sky{0.0f};
    /// What the ground half of the sphere reflects of it.
    glm::vec3 ground{0.0f};
};

/// @brief @ref SkyBodyRadiance for one body.
///
/// @param viewAirMass  How much air the ray crosses. A property of the ray alone,
///                     so the caller computes it once and both bodies share it.
/// @param scattered    How much of a beam that air scatters. Likewise the ray's.
/// @param tint         How much of the air's colour shift this body takes. One is
///                     the physics and is what the sun always passes.
///
/// **The tint reaches every extinction term and no scattering coefficient**, and
/// that split is the whole of it. Extinction is what turns a low body orange:
/// it removes blue over a long path, and it is the term whose saturation is wrong
/// for a moon boosted four orders of magnitude past its real brightness. The
/// scattering coefficients are why the sky is BLUE — they are not a hue the air
/// imposed on the body, they are the air's own — so a moonlit sky stays blue at
/// every tint, while the sunset-coloured aureole a low moon throws around itself
/// fades with it.
[[nodiscard]] inline SkyBodyRadiance SkyBodyContribution(const glm::vec3 &ray, const glm::vec3 &toBody,
                                                         const glm::vec3 &radiant, const SkySettings &settings,
                                                         float viewAirMass, float scattered, float tint = 1.0f)
{
    const glm::vec3 extinction = BeamExtinction(settings);
    const float bodyAirMass = SunAirMass(toBody.y);
    const float cosGamma = std::clamp(glm::dot(ray, toBody), -1.0f, 1.0f);

    const glm::vec3 transmittance = TintedTransmittance(Transmittance(extinction, bodyAirMass), tint);
    const glm::vec3 beam = radiant * transmittance;

    // Light that reaches the eye crossed the atmosphere twice — in along the
    // beam, out along the view ray — and is extinguished over both. Attenuating
    // the sum rather than the beam alone is what keeps a sunset red on Earth:
    // over the short path of a noon zenith the colour is the scattering
    // coefficient's, and over the long path of a low sun the exponential wins
    // and what survives is whatever that coefficient scatters LEAST.
    const glm::vec3 attenuation = TintedTransmittance(Transmittance(extinction, bodyAirMass + viewAirMass), tint);

    // How much of the beam the haze scatters toward the eye, and which way it
    // throws it. Its extinction is not applied here — it is in `attenuation`
    // with the air's, so both in-scattered terms are dimmed by the same thing
    // they were dimmed by on the way in.
    const glm::vec3 mie = (glm::vec3(1.0f) - Transmittance(settings.hazeScattering, viewAirMass)) *
                          MiePhase(cosGamma, settings.hazeForwardness);

    // The second bounce onward, attenuated by the body's path but not the view's
    // — which is the whole point, since it is at the horizon, where the
    // round-trip term has died, that the sky would otherwise turn green.
    const glm::vec3 multiScattered = beam * (settings.airScattering * (scattered * settings.skyBounce));

    return SkyBodyRadiance{
        .beam = beam,
        .transmittance = transmittance,
        .sky = radiant * attenuation * (settings.airScattering * (RayleighPhase(cosGamma) * scattered) + mie) +
               multiScattered,
        // Lambertian, foreshortened by the body's elevation.
        .ground = settings.groundColor * (beam * (std::max(toBody.y, 0.0f) * glm::one_over_pi<float>()))};
}

/// @brief Radiance arriving from @p direction, in linear RGB, from a sky with a
/// sun and a moon in it.
///
/// Finite and non-negative for every direction and every position of either body,
/// including both below the horizon and a direction pointing straight down.
///
/// **This function is the specification and sky.frag is its transcription**, with
/// one deliberate gap: the shader multiplies the moon's disk by an albedo
/// photograph and nothing here can sample a texture, so the disk term below is
/// the shader's with that texture white. Every CPU caller that matters removes
/// the disks first — @ref AmbientFromSky zeroes both intensities before
/// integrating — so the gap reaches nothing today. A future CPU consumer that
/// wants the disk, a reflection probe say, has to load the texture itself or
/// accept the white moon.
///
/// @param direction  Unit vector; up is +Y. Need not be normalised.
[[nodiscard]] inline glm::vec3 SkyRadiance(const glm::vec3 &direction, const SkySun &rawSun, const SkyMoon &rawMoon,
                                           const SkySettings &rawSettings)
{
    const SkySettings settings = Sanitized(rawSettings);
    const SkySun sun = Sanitized(rawSun);
    const SkyMoon moon = Sanitized(rawMoon);
    const glm::vec3 ray = SafeSkyDirection(direction);

    // The ray's own share of the work, done once for both bodies. The colourless
    // part of the air's extinction says how much of a beam the air scatters
    // rather than transmits, which is a quantity and not a hue.
    const float viewAirMass = ViewAirMass(ray.y);
    const glm::vec3 airExtinction = settings.airScattering * settings.airThickness;
    const float greyExtinction = (airExtinction.r + airExtinction.g + airExtinction.b) / 3.0f;
    // Saturating toward one is why the horizon is bright and the zenith, with a
    // thirtieth of the air, is not.
    const float scattered = 1.0f - std::exp(-greyExtinction * viewAirMass);

    // The sun takes the whole of the air's colour shift, always: a sunset is seen
    // in daylight, at full colour, and is not something to soften. Only the moon
    // has a tint, and only because its brightness is a compromise.
    const SkyBodyRadiance fromSun = SkyBodyContribution(ray, sun.directionToSun, sun.color * sun.intensity, settings,
                                                        viewAirMass, scattered, 1.0f);
    const SkyBodyRadiance fromMoon =
        SkyBodyContribution(ray, moon.directionToMoon, moon.color * moon.intensity, settings, viewAirMass, scattered,
                            moon.atmosphericTint);

    // The night floor goes to both halves, so a moonless landscape and the sky
    // over it fall to the same floor instead of the ground going black first.
    const glm::vec3 sky = fromSun.sky + fromMoon.sky + settings.nightColor;
    const glm::vec3 ground = fromSun.ground + fromMoon.ground + settings.nightColor;

    const float skyward = glm::smoothstep(-kHorizonSoftness, kHorizonSoftness, ray.y);
    glm::vec3 radiance = glm::mix(ground, sky, skyward);

    // Both disks, gated by the same blend so a body sets behind the ground rather
    // than shining up through it.
    if (settings.sunDiskIntensity > 0.0f)
    {
        const float cosGamma = std::clamp(glm::dot(ray, sun.directionToSun), -1.0f, 1.0f);
        const float profile = SunDiskProfile(std::acos(cosGamma), glm::radians(settings.sunSizeDegrees),
                                             settings.sunEdgeSoftness, settings.sunLimbDarkening);
        radiance += fromSun.beam * settings.sunDiskColor * (settings.sunDiskIntensity * profile * skyward);
    }

    if (moon.diskIntensity > 0.0f)
    {
        const float radius = glm::radians(moon.sizeDegrees);
        const float angle = std::acos(std::clamp(glm::dot(ray, moon.directionToMoon), -1.0f, 1.0f));
        // The moon's limb darkening is zero: the photograph already carries
        // whatever the real limb does, and applying a second one would darken it
        // twice. The edge softness IS shared with the sun, because that is
        // antialiasing rather than a look.
        const float profile = SunDiskProfile(angle, radius, settings.sunEdgeSoftness, 0.0f);
        const float lit = MoonDiskLit(ray, moon.directionToMoon, sun.directionToSun, radius);
        // The beam is already tinted — SkyBodyContribution applied it — so the
        // disk, the aureole around it and the light on the ground all soften by
        // the same amount and cannot disagree.
        radiance += fromMoon.beam * moon.diskColor * (moon.diskIntensity * profile * lit * skyward);
    }

    return glm::max(radiance * settings.exposure, glm::vec3(0.0f));
}

/// @brief The same sky with no moon in it.
///
/// Not a compatibility shim: a level without a Moon component genuinely has no
/// moon, and this is the sky it gets. Bit for bit the four-argument form with a
/// default SkyMoon, whose radiance is an exact zero at every term.
[[nodiscard]] inline glm::vec3 SkyRadiance(const glm::vec3 &direction, const SkySun &rawSun,
                                           const SkySettings &rawSettings)
{
    return SkyRadiance(direction, rawSun, SkyMoon{}, rawSettings);
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
    /// xyz = scattering coefficients as authored, w = the optical depth scaling
    /// them. Deliberately NOT premultiplied: the coefficients are a ratio where
    /// they tint the in-scattered light and an extinction where they attenuate
    /// it, and a lane holding the product cannot say which it is — which is how
    /// the shader once read the tint fourteen times too dark.
    glm::vec4 airScattering{0.0f};
    glm::vec4 haze{0.0f};                           ///< xyz = how strongly haze scatters, w = its forwardness
    glm::vec4 groundColor{0.0f};                    ///< xyz = linear colour, w = sky intensity
    glm::vec4 nightColor{0.0f};                     ///< xyz = linear colour, w = disk intensity
    glm::vec4 sunDiskColor{1.0f};                   ///< xyz = disk tint, w = limb darkening
    glm::vec4 atmosphere{0.0f};                     ///< x = multiple scattering, yzw unused
    /// xyz = unit direction TO the moon, w = its disk radius (radians). The moon
    /// carries its own radius because it is a different body, not because the
    /// sun's would not fit: feeding it through the sun's lane draws it sun-sized.
    glm::vec4 moonDirection{0.0f, 1.0f, 0.0f, 0.0f};
    glm::vec4 moonRadiance{0.0f};                   ///< xyz = colour * intensity, w = disk intensity
    glm::vec4 moonDiskColor{1.0f};                  ///< xyz = tint over the albedo texture, w = atmospheric tint
    /// xyz = which way the disk image's top points, tangent to the sky at the
    /// moon; w unused. A vector rather than an angle because the frame it comes
    /// from means nothing here — the shader only has to stamp the picture.
    glm::vec4 moonUp{0.0f, 0.0f, -1.0f, 0.0f};
};

// std140 packs a mat4 as four vec4s and every vec4 on its own sixteen bytes, so
// this layout is contiguous with no padding and these offsets are the shader's.
// Asserted rather than trusted: a lane inserted in the wrong place here reads as
// a plausible sky with the wrong numbers in it, which nothing on screen names.
static_assert(sizeof(SkyConstants) == 272, "SkyConstants must match sky.frag's uniform block size");
static_assert(offsetof(SkyConstants, moonDirection) == 208, "moonDirection must follow the ninth vec4");
static_assert(offsetof(SkyConstants, moonUp) == 256, "moonUp must be the last lane");

/// @brief The constants for one sky draw, with everything sanitized on the way in.
[[nodiscard]] inline SkyConstants MakeSkyConstants(const glm::mat4 &invViewProjection, const glm::vec3 &cameraPosition,
                                                   const SkySun &rawSun, const SkyMoon &rawMoon,
                                                   const SkySettings &rawSettings)
{
    const SkySettings settings = Sanitized(rawSettings);
    const SkySun sun = Sanitized(rawSun);
    const SkyMoon moon = Sanitized(rawMoon);

    return SkyConstants{
        .invViewProjection = invViewProjection,
        .cameraPosition = glm::vec4(cameraPosition, 0.0f),
        .sunDirection = glm::vec4(sun.directionToSun, glm::radians(settings.sunSizeDegrees)),
        .sunRadiance = glm::vec4(sun.color * sun.intensity, settings.sunEdgeSoftness),
        .airScattering = glm::vec4(settings.airScattering, settings.airThickness),
        .haze = glm::vec4(settings.hazeScattering, settings.hazeForwardness),
        .groundColor = glm::vec4(settings.groundColor, settings.exposure),
        .nightColor = glm::vec4(settings.nightColor, settings.sunDiskIntensity),
        .sunDiskColor = glm::vec4(settings.sunDiskColor, settings.sunLimbDarkening),
        .atmosphere = glm::vec4(settings.skyBounce, 0.0f, 0.0f, 0.0f),
        .moonDirection = glm::vec4(moon.directionToMoon, glm::radians(moon.sizeDegrees)),
        .moonRadiance = glm::vec4(moon.color * moon.intensity, moon.diskIntensity),
        .moonDiskColor = glm::vec4(moon.diskColor, moon.atmosphericTint),
        .moonUp = glm::vec4(moon.imageUp, 0.0f)};
}

/// @brief The constants for a sky with no moon in it.
[[nodiscard]] inline SkyConstants MakeSkyConstants(const glm::mat4 &invViewProjection, const glm::vec3 &cameraPosition,
                                                   const SkySun &rawSun, const SkySettings &rawSettings)
{
    return MakeSkyConstants(invViewProjection, cameraPosition, rawSun, SkyMoon{}, rawSettings);
}

} // namespace Assisi::Render
