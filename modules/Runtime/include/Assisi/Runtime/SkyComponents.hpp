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
/// @ref zenithOpticalDepth to zero is a world with no atmosphere at all: a black
/// sky, a hard unattenuated sun, and a lit ground.
ACOMP()
struct Skybox
{
    /// @name The air
    /// @{
    /// Scattering by the air's own molecules, per channel. Earth's falls as the
    /// inverse fourth power of wavelength, which is why its sky is blue and its
    /// sunsets are red — both from this one triple.
    AFIELD() glm::vec3 rayleighCoefficients = Assisi::Render::kEarthRayleighCoefficients;

    /// How much air there is, scaling the coefficients above. Zero is airless.
    AFIELD(min = 0) float zenithOpticalDepth = Assisi::Render::kDefaultZenithOpticalDepth;

    /// Dust and droplets, per channel. Grey on Earth; colouring it is what a
    /// dust-dominated sky needs.
    AFIELD() glm::vec3 mieCoefficients = Assisi::Render::kEarthMieCoefficients;

    /// How sharply haze throws light. Positive scatters forward (the halo around
    /// a low sun), negative scatters back toward it, zero is uniform.
    AFIELD() float mieAsymmetry = Assisi::Render::kDefaultMieAsymmetry;

    /// How much light knocked out of the direct line of sight arrives anyway,
    /// having bounced again. Without it the horizon turns green, because single
    /// scattering treats that light as lost exactly where there is most of it.
    AFIELD(min = 0) float multipleScattering = Assisi::Render::kDefaultMultipleScattering;
    /// @}

    /// @name Colour
    /// @{
    /// Albedo of the ground half of the sphere, lit by the same sun as the sky.
    /// Not a real surface — the sky covers every direction, and this is what the
    /// ones pointing down get.
    AFIELD() glm::vec3 groundColor{0.11f, 0.10f, 0.09f};

    /// What is left when the sun has gone, added rather than blended so no
    /// threshold has to decide when night starts.
    AFIELD() glm::vec3 nightColor = Assisi::Render::kDefaultNightColor;

    /// Exposure of the sky against the rest of the scene. Far from 1 because the
    /// model yields the fraction of the beam a column of air scatters toward the
    /// eye, which for a clear zenith is a few percent.
    AFIELD(min = 0) float intensity = Assisi::Render::kDefaultSkyIntensity;
    /// @}

    /// @name The disk
    /// @{
    /// Angular radius in degrees. Earth's sun is about 0.27, but a disk that
    /// small lands inside one pixel and aliases into a crawling dot.
    AFIELD(min = 0) float sunAngularRadiusDegrees = Assisi::Render::kDefaultSunAngularRadiusDegrees;

    /// Width of the edge fade as a fraction of the radius. Never zero: the disk
    /// is the brightest thing in the frame by orders of magnitude, and a hard
    /// edge on it flickers as the camera turns.
    AFIELD(min = 0) float sunEdgeSoftness = Assisi::Render::kDefaultSunEdgeSoftness;

    /// How much dimmer the rim is than the centre. A star is a ball of gas, not
    /// a sticker — zero is a flat disk, and a flat disk is what reads as fake.
    AFIELD(min = 0) float sunLimbDarkening = Assisi::Render::kDefaultSunLimbDarkening;

    /// Brightness of the disk against the sky around it. Zero removes the disk
    /// and leaves the scattering that surrounds it untouched.
    AFIELD(min = 0) float sunDiskIntensity = Assisi::Render::kDefaultSunDiskIntensity;

    /// Tint on the disk ALONE, over the light's own colour. White leaves the
    /// disk agreeing with what lights the world; pushing it is the escape hatch
    /// for a yellow sun over a neutrally-lit scene.
    AFIELD() glm::vec3 sunDiskColor{1.0f, 0.95f, 0.82f};
    /// @}
};

/// @brief The component's look as the render side wants it.
[[nodiscard]] inline Assisi::Render::SkySettings ToSkySettings(const Skybox &skybox)
{
    return Assisi::Render::SkySettings{.rayleighCoefficients = skybox.rayleighCoefficients,
                                       .zenithOpticalDepth = skybox.zenithOpticalDepth,
                                       .mieCoefficients = skybox.mieCoefficients,
                                       .mieAsymmetry = skybox.mieAsymmetry,
                                       .multipleScattering = skybox.multipleScattering,
                                       .groundColor = skybox.groundColor,
                                       .nightColor = skybox.nightColor,
                                       .intensity = skybox.intensity,
                                       .sunAngularRadiusDegrees = skybox.sunAngularRadiusDegrees,
                                       .sunEdgeSoftness = skybox.sunEdgeSoftness,
                                       .sunLimbDarkening = skybox.sunLimbDarkening,
                                       .sunDiskIntensity = skybox.sunDiskIntensity,
                                       .sunDiskColor = skybox.sunDiskColor};
}

} // namespace Assisi::Runtime
