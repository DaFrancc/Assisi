/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SkyComponents.hpp
/// @brief The Skybox component — a sky, authored on the light that lights it.
///
/// Placement: Skybox goes on an entity that also carries a DirectionalLight, and
/// that light is the sun the sky scatters. A DirectionalLight on its own is just
/// a light; the sky is what the component opts into. See Runtime::ResolveSky for
/// the rules, including what a scene with several directional lights does.
///
/// Everything the sky's look needs is here and saved with the level, so a world
/// is authored in the editor rather than in engine code.

#include <Assisi/Prelude.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Sky.hpp>

#include <array>
#include <cstddef>

namespace Assisi::Runtime
{

/// @brief A named sky, for authors who want a place rather than a set of
/// coefficients.
///
/// These are conditions, not planets: what separates them is how clean the air
/// is, what is suspended in it, and what the ground under it reflects. A preset
/// writes every knob below, so picking one and reading the numbers it produced
/// is the fastest way to learn what any of them do.
AENUM()
enum class SkyPreset : std::uint8_t
{
    /// A temperate clear day. The baseline the defaults describe.
    Clear,
    /// Cold, dry, very clean air over snow. Deep blue overhead, almost no haze,
    /// and a bright ground that throws a great deal back up.
    Arctic,
    /// Hot and dusty. Warm-tinted haze thick enough to pale the sky toward the
    /// horizon, over dry ground.
    Savanna,
    /// Humid and hazy. Moisture scatters greyly and hardest toward the sun, so
    /// the horizon washes out early and the sky reads soft.
    Tropical,
    /// Thin air at altitude. Little of it to scatter in, so the zenith goes
    /// nearly navy and the sun stays hard and white far into the afternoon.
    Alpine,
    /// Thick, still, muggy air. The sun survives as a pale disk with no edge to
    /// it and the whole sky flattens toward grey.
    ///
    /// Deliberately NOT called overcast, and it is worth saying why. A cloudy sky
    /// is bright because light bounces many times inside the cloud; single
    /// scattering has no way to reach that, and haze thick enough to bury the sun
    /// here takes the sky down with it. This is the thickest air the model is
    /// honest about.
    Hazy,
    /// No atmosphere. A black sky, a hard white sun, and lit ground — the
    /// airless limit of the same model rather than a mode of its own.
    Airless,
    /// The knobs below, as authored. Every preset greys them out, because a
    /// preset IS the answer — an author who wants to depart from one starts here.
    Custom,
};

/// @brief An analytic atmosphere, scattering the light on the same entity.
///
/// The sun's colour and intensity are deliberately NOT here: they are the
/// DirectionalLight's, because they are one physical quantity. The same value
/// lights every surface in the world, tints the scattering, and colours the
/// disk — so a blue sun is a blue directional light and everything follows from
/// it. @ref sunDiskColor tints the disk alone, for when that coherence is not
/// what is wanted.
///
/// The scattering coefficients are the air's composition, not Earth's constants.
/// Pushing them green gives a green noon and a magenta sunset with nothing
/// further authored, because the same coefficient governs both. Setting
/// @ref airThickness to zero is a world with no atmosphere at all: a black
/// sky, a hard unattenuated sun, and a lit ground.
ACOMP()
struct Skybox
{
    /// Which sky this is. Every value but Custom answers the whole component, so
    /// the knobs below grey out — a greyed field is one nothing reads.
    ///
    /// A preset is a live answer rather than a stamp: a level saved as Arctic
    /// follows the Arctic numbers if they are ever improved, where one saved as
    /// Custom keeps exactly what was typed.
    AFIELD(radioBroadcast) SkyPreset preset = SkyPreset::Clear;

    /// @name The air
    /// @{
    /// Scattering by the air's own molecules, per channel. Earth's falls as the
    /// inverse fourth power of wavelength, which is why its sky is blue and its
    /// sunsets are red — both from this one triple.
    AFIELD(radioListen = {source = preset, value = Custom, behavior = grey}) glm::vec3 airScattering = Assisi::Render::kEarthAirScattering;

    /// How much air there is, scaling the coefficients above. Zero is airless.
    AFIELD(min = 0, radioListen = {source = preset, value = Custom, behavior = grey}) float airThickness = Assisi::Render::kDefaultAirThickness;

    /// Dust and droplets, per channel. Grey on Earth; colouring it is what a
    /// dust-dominated sky needs.
    AFIELD(radioListen = {source = preset, value = Custom, behavior = grey}) glm::vec3 hazeScattering = Assisi::Render::kEarthHazeScattering;

    /// How sharply haze throws light. Positive scatters forward (the halo around
    /// a low sun), negative scatters back toward it, zero is uniform.
    AFIELD(radioListen = {source = preset, value = Custom, behavior = grey}) float hazeForwardness = Assisi::Render::kDefaultHazeForwardness;

    /// How much light knocked out of the direct line of sight arrives anyway,
    /// having bounced again. Without it the horizon turns green, because single
    /// scattering treats that light as lost exactly where there is most of it.
    AFIELD(min = 0, radioListen = {source = preset, value = Custom, behavior = grey}) float skyBounce = Assisi::Render::kDefaultSkyBounce;
    /// @}

    /// @name Colour
    /// @{
    /// Albedo of the ground half of the sphere, lit by the same sun as the sky.
    /// Not a real surface — the sky covers every direction, and this is what the
    /// ones pointing down get.
    AFIELD(radioListen = {source = preset, value = Custom, behavior = grey}) glm::vec3 groundColor{0.11f, 0.10f, 0.09f};

    /// What is left when the sun has gone, added rather than blended so no
    /// threshold has to decide when night starts.
    AFIELD(radioListen = {source = preset, value = Custom, behavior = grey}) glm::vec3 nightColor = Assisi::Render::kDefaultNightColor;

    /// Exposure of the sky against the rest of the scene. Far from 1 because the
    /// model yields the fraction of the beam a column of air scatters toward the
    /// eye, which for a clear zenith is a few percent.
    AFIELD(min = 0, radioListen = {source = preset, value = Custom, behavior = grey}) float exposure = Assisi::Render::kDefaultSkyExposure;
    /// @}

    /// @name The disk
    /// @{
    /// Angular radius in degrees. Earth's sun is about 0.27, but a disk that
    /// small lands inside one pixel and aliases into a crawling dot.
    AFIELD(min = 0, radioListen = {source = preset, value = Custom, behavior = grey}) float sunSizeDegrees = Assisi::Render::kDefaultSunSizeDegrees;

    /// Width of the edge fade as a fraction of the radius. Never zero: the disk
    /// is the brightest thing in the frame by orders of magnitude, and a hard
    /// edge on it flickers as the camera turns.
    AFIELD(min = 0, radioListen = {source = preset, value = Custom, behavior = grey}) float sunEdgeSoftness = Assisi::Render::kDefaultSunEdgeSoftness;

    /// How much dimmer the rim is than the centre. A star is a ball of gas, not
    /// a sticker — zero is a flat disk, and a flat disk is what reads as fake.
    AFIELD(min = 0, radioListen = {source = preset, value = Custom, behavior = grey}) float sunLimbDarkening = Assisi::Render::kDefaultSunLimbDarkening;

    /// Brightness of the disk against the sky around it. Zero removes the disk
    /// and leaves the scattering that surrounds it untouched.
    AFIELD(min = 0, radioListen = {source = preset, value = Custom, behavior = grey}) float sunDiskIntensity = Assisi::Render::kDefaultSunDiskIntensity;

    /// Tint on the disk ALONE, over the light's own colour. White leaves the
    /// disk agreeing with what lights the world; pushing it is the escape hatch
    /// for a yellow sun over a neutrally-lit scene.
    AFIELD(radioListen = {source = preset, value = Custom, behavior = grey}) glm::vec3 sunDiskColor{1.0f, 0.95f, 0.82f};
    /// @}
};

/// @brief Every preset, already built, in the order @ref SkyPreset declares them.
///
/// A table rather than a function that assembles one per call: these are seven
/// fixed answers, so they are constructed once at compile time and handed out.
///
/// Each is written as what it CHANGES. The fields left out are not missing — an
/// aggregate fills them from SkySettings' own defaults — so a preset reads as
/// the handful of knobs that make it what it is, and stays a complete answer:
/// an author switching from Arctic to Savanna gets the savanna's ground as well
/// as its dust, with nothing surviving from the sky before it.
inline constexpr std::array<Assisi::Render::SkySettings, static_cast<std::size_t>(SkyPreset::Custom) + 1>
kSkyPresets{{
    // Clear: the defaults, unchanged. It is the sky every other entry is a
    // departure from.
    Assisi::Render::SkySettings{},

    // Arctic. Cold air holds almost no moisture, so there is very little to
    // scatter greyly and the blue survives all the way to the horizon. Snow
    // is the brightest ground there is, and most of what the sky sends down
    // comes straight back — which is why the light under it is so flat: the
    // fill from below rivals the sun from above, and nothing has a dark side.
    Assisi::Render::SkySettings{.airThickness = 0.22f,
                                .hazeScattering = glm::vec3(0.0010f),
                                .skyBounce = 0.50f,
                                .groundColor = glm::vec3(0.90f, 0.93f, 0.97f)},

    // Savanna. Suspended dust, which scatters red where air scatters blue,
    // so the haze itself is warm and the horizon goes straw before the sun
    // is anywhere near it. Enough of it that a shadow here is lit by warm
    // light rather than by a blue sky — which is what makes the place read
    // as hot rather than merely bright.
    Assisi::Render::SkySettings{.airThickness = 0.13f,
                                .hazeScattering = glm::vec3(0.050f, 0.032f, 0.012f),
                                .hazeForwardness = 0.72f,
                                .groundColor = glm::vec3(0.42f, 0.32f, 0.16f)},

    // Tropical. Water droplets are large enough to scatter every wavelength
    // alike and hard forward, so the sky pales toward the sun and the horizon
    // loses its colour long before the ground does. Thick enough to take real
    // bite out of the sun: the light here is mostly a bright white sky rather
    // than a beam, and the ground throws green back into everything above it.
    Assisi::Render::SkySettings{.airThickness = 0.10f,
                                .hazeScattering = glm::vec3(0.055f),
                                .hazeForwardness = 0.88f,
                                .skyBounce = 0.30f,
                                .groundColor = glm::vec3(0.09f, 0.16f, 0.07f)},

    // Alpine. Less air overhead means less of it to scatter in: the zenith
    // darkens toward navy, and the sun keeps its edge and its colour much
    // lower in the sky than it would at sea level. The hardest light of the
    // seven — a nearly white beam against almost no sky to fill the shadows,
    // over pale rock that is the only thing softening them.
    Assisi::Render::SkySettings{.airThickness = 0.035f,
                                .hazeScattering = glm::vec3(0.0004f),
                                .skyBounce = 0.10f,
                                .groundColor = glm::vec3(0.50f, 0.51f, 0.54f),
                                .sunEdgeSoftness = 0.05f},

    // Hazy. Enough suspended water to take most of the direct sun out and
    // spread it, so the disk survives as something you can look at and the
    // sky loses its blue. The wide, soft edge is what a sun seen through haze
    // has; a hard one would read as a hole punched in fog.
    //
    // The haze is at the ceiling of what single scattering is honest about,
    // which is what makes this the flattest sky here rather than the darkest:
    // sky and ground arrive at nearly the same value, so a surface's own
    // shape stops telling you which way it faces.
    Assisi::Render::SkySettings{.airThickness = 0.16f,
                                .hazeScattering = glm::vec3(0.060f),
                                .hazeForwardness = 0.55f,
                                .skyBounce = 0.35f,
                                .groundColor = glm::vec3(0.20f, 0.20f, 0.19f),
                                .exposure = 6.5f,
                                .sunEdgeSoftness = 0.40f,
                                .sunDiskIntensity = 8.0f},

    // Airless. The degenerate case, and it needs no special handling
    // anywhere: with nothing to scatter in, the scattering term is zero and
    // the beam arrives whole.
    Assisi::Render::SkySettings{.airThickness = 0.0f,
                                .hazeScattering = glm::vec3(0.0f),
                                .groundColor = glm::vec3(0.14f, 0.13f, 0.12f),
                                .nightColor = glm::vec3(0.0f),
                                .sunEdgeSoftness = Assisi::Render::kMinSunEdgeSoftness,
                                .sunDiskColor = glm::vec3(1.0f)},

    // Custom: the defaults again, which is what makes departing from a preset
    // start where the knobs already are rather than somewhere else.
    Assisi::Render::SkySettings{},
}};

// Custom is last, and this is what says so. Adding a preset anywhere shifts
// Custom's value, and the count stops matching until the table gains its row —
// which is the check the switch this replaced got from -Wswitch for free.
static_assert(kSkyPresets.size() == static_cast<std::size_t>(SkyPreset::Custom) + 1,
              "Every SkyPreset needs a row in kSkyPresets, and Custom has to stay last.");

/// @brief The settings a named sky stands for.
[[nodiscard]] inline const Assisi::Render::SkySettings &PresetSettings(SkyPreset preset)
{
    return kSkyPresets[static_cast<std::size_t>(preset)];
}

/// @brief The component's look as the render side wants it.
[[nodiscard]] inline Assisi::Render::SkySettings ToSkySettings(const Skybox &skybox)
{
    // A preset answers outright. The stored knobs are not consulted and not
    // merged into — which is what lets the inspector grey them honestly.
    if (skybox.preset != SkyPreset::Custom)
    {
        return PresetSettings(skybox.preset);
    }

    return Assisi::Render::SkySettings{.airScattering = skybox.airScattering,
                                       .airThickness = skybox.airThickness,
                                       .hazeScattering = skybox.hazeScattering,
                                       .hazeForwardness = skybox.hazeForwardness,
                                       .skyBounce = skybox.skyBounce,
                                       .groundColor = skybox.groundColor,
                                       .nightColor = skybox.nightColor,
                                       .exposure = skybox.exposure,
                                       .sunSizeDegrees = skybox.sunSizeDegrees,
                                       .sunEdgeSoftness = skybox.sunEdgeSoftness,
                                       .sunLimbDarkening = skybox.sunLimbDarkening,
                                       .sunDiskIntensity = skybox.sunDiskIntensity,
                                       .sunDiskColor = skybox.sunDiskColor};
}

} // namespace Assisi::Runtime
