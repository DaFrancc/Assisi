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

/// @brief The settings a named sky stands for.
///
/// Every field, so a preset is a complete answer rather than a partial one — an
/// author switching from Arctic to Savanna gets the savanna's ground as well as
/// its dust, and nothing survives from the sky before it.
[[nodiscard]] inline Assisi::Render::SkySettings PresetSettings(SkyPreset preset)
{
    Assisi::Render::SkySettings settings; // Clear: what the defaults already describe.
    switch (preset)
    {
    case SkyPreset::Clear:
    case SkyPreset::Custom:
        break;

    case SkyPreset::Arctic:
        // Cold air holds almost no moisture, so there is very little to scatter
        // greyly and the blue survives all the way to the horizon. Snow is the
        // brightest ground there is, and most of what the sky sends down comes
        // straight back — which is why the light under it is so flat.
        settings.airThickness = 0.16f;
        settings.hazeScattering = glm::vec3(0.0015f);
        settings.groundColor = glm::vec3(0.80f, 0.83f, 0.88f);
        settings.skyBounce = 0.35f;
        break;

    case SkyPreset::Savanna:
        // Suspended dust, which absorbs blue rather than scattering it evenly,
        // so the haze itself is warm and the horizon goes straw before the sun
        // is anywhere near it.
        settings.airThickness = 0.13f;
        settings.hazeScattering = glm::vec3(0.030f, 0.022f, 0.012f);
        settings.hazeForwardness = 0.70f;
        settings.groundColor = glm::vec3(0.34f, 0.27f, 0.15f);
        break;

    case SkyPreset::Tropical:
        // Water droplets are large enough to scatter every wavelength alike and
        // hard forward, so the sky pales toward the sun and the horizon loses
        // its colour long before the ground does.
        settings.airThickness = 0.12f;
        settings.hazeScattering = glm::vec3(0.040f);
        settings.hazeForwardness = 0.85f;
        settings.groundColor = glm::vec3(0.12f, 0.16f, 0.09f);
        break;

    case SkyPreset::Alpine:
        // Less air overhead means less of it to scatter in: the zenith darkens
        // toward navy, and the sun keeps its edge and its colour much lower in
        // the sky than it would at sea level.
        settings.airThickness = 0.055f;
        settings.hazeScattering = glm::vec3(0.0008f);
        settings.groundColor = glm::vec3(0.42f, 0.44f, 0.47f);
        settings.sunEdgeSoftness = 0.05f;
        break;

    case SkyPreset::Hazy:
        // Enough suspended water to take a third of the direct sun out and
        // spread it, so the disk survives as something you can look at and the
        // sky loses most of its blue. The wide, soft edge is what a sun seen
        // through haze has; a hard one would read as a hole punched in fog.
        settings.airThickness = 0.11f;
        settings.hazeScattering = glm::vec3(0.05f);
        settings.hazeForwardness = 0.60f;
        settings.groundColor = glm::vec3(0.17f, 0.17f, 0.16f);
        settings.sunDiskIntensity = 8.0f;
        settings.sunEdgeSoftness = 0.40f;
        settings.exposure = 6.5f;
        break;

    case SkyPreset::Airless:
        // The degenerate case, and it needs no special handling anywhere: with
        // nothing to scatter in, the scattering term is zero and the beam
        // arrives whole.
        settings.airThickness = 0.0f;
        settings.hazeScattering = glm::vec3(0.0f);
        settings.nightColor = glm::vec3(0.0f);
        settings.groundColor = glm::vec3(0.14f, 0.13f, 0.12f);
        settings.sunEdgeSoftness = Assisi::Render::kMinSunEdgeSoftness;
        settings.sunDiskColor = glm::vec3(1.0f);
        break;
    }
    return settings;
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
