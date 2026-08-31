/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file LightComponents.hpp
/// @brief ECS component types for the three basic dynamic light types.
///
/// All three types are trivially copyable (plain data, no pointers).
///
/// Placement:
///   - DirectionalLight  — direction is stored directly on the component;
///     no Transform is needed (it has no world position).
///   - PointLight        — requires a Transform for world position.
///   - SpotLight         — requires a Transform for world position; `direction`
///     is stored on the component and is LOCAL, rotated into world by that
///     Transform (see LightingSystem::WorldSpotDirection).

#include <Assisi/Prelude.hpp>
#include <Assisi/Math/Color.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::Runtime
{

// None of the three light types is ACOMP(replicable), and that is a decision
// rather than an omission. Lighting is authored data both machines already hold
// from the level file, so replicating it would spend bandwidth restating what
// nobody is changing — and a mirrored light whose intensity the host animates is
// a feature no game here has asked for yet. Granting the capability later is one
// word plus a regen, with one caveat worth knowing: the flag is a protocol-hash
// input, so builds either side of that change refuse to pair.

// `castsShadows` defaults ON on all three types, and the default is the whole
// point: an unshadowed light passes through walls, so leaving it off by default
// would make every newly placed light wrong until someone noticed. Turning it
// off is a cost decision the author makes deliberately.

/// @brief How a directional light's own colour is written down.
///
/// Two spellings of one thing. Kelvin is how light sources are actually
/// specified — 3000 K reads as afternoon, 6500 K as an overcast noon — and RGB is
/// there for a sun that is not a blackbody at all.
AENUM()
enum class SunColorExpression : std::uint8_t
{
    Rgb,         ///< Whatever `color` says.
    Temperature, ///< A blackbody at `temperatureKelvin`.
};

/// @brief Infinite-distance directional light (sun / moon).
///
/// No position, no falloff.  Multiple directional lights are supported.
ACOMP()
struct DirectionalLight
{
    AFIELD() glm::vec3 direction{0.f, -1.f, 0.f}; ///< World-space direction the light shines (unit vector).

    /// @brief Take the light's colour from the sky on this same entity.
    ///
    /// On, the world is lit by what is left of the sun after that atmosphere: at
    /// sunset it goes orange and dimmer because that is what reaches the ground
    /// by then, and it matches the sky it sits under with no system keeping the
    /// two in step. Off is for lighting that is art-directed rather than
    /// physical.
    ///
    /// Does nothing without a Skybox on this entity — there is no atmosphere to
    /// take a colour from, and @ref color is used as authored.
    AFIELD(radioBroadcast) bool tintedBySky = true;

    /// Which of the two below says what colour this sun is. Both vanish while the
    /// sky is supplying one, because then neither is read.
    AFIELD(radioBroadcast, radioListen = {source = tintedBySky, value = false, behavior = vanish})
    SunColorExpression colorExpression = SunColorExpression::Rgb;

    /// Linear-RGB colour, under SunColorExpression::Rgb.
    AFIELD(radioListen = {source = colorExpression, value = Rgb, behavior = grey})
    Assisi::Math::Color3 color{1.f, 1.f, 1.f};

    /// Colour temperature, under SunColorExpression::Temperature. Low is warm:
    /// about 2800 K for tungsten, 5800 for daylight, 6500 for an overcast sky,
    /// past 10000 for a blue north sky. Morning and afternoon light differ here.
    AFIELD(min = 1667.0, max = 25000.0, radioListen = {source = colorExpression, value = Temperature, behavior = grey})
    float temperatureKelvin = 5778.f;

    AFIELD() float intensity = 1.f;
    AFIELD() bool castsShadows = true; ///< Whether this light renders a shadow map.
};

/// @brief The sun's own colour, however it was written down.
///
/// One place resolves the two spellings, so nothing downstream has to know there
/// were two. Not what lights the world when @ref DirectionalLight::tintedBySky is
/// on — see LightingSystem::SunlightColor for that.
[[nodiscard]] inline glm::vec3 AuthoredSunColor(const DirectionalLight &light)
{
    return light.colorExpression == SunColorExpression::Temperature
               ? Assisi::Math::BlackbodyColor(light.temperatureKelvin)
               : glm::vec3(light.color);
}

/// @brief Omnidirectional point light with distance falloff.
///
/// Requires Transform for world position.
/// Uses windowed inverse-square attenuation: zero contribution beyond `radius`.
ACOMP()
struct PointLight
{
    AFIELD() Assisi::Math::Color3 color{1.f, 1.f, 1.f};
    AFIELD() float intensity = 1.f;           ///< May be negative (light subtraction).
    AFIELD(min = 0) float radius = 10.f;      ///< Maximum influence range in world units; never negative.
    AFIELD() bool castsShadows = true;        ///< Whether this light renders shadow maps (six faces).
};

/// @brief Cone-restricted point light (flashlight / stage spotlight).
///
/// Requires Transform for world position.
/// Intensity falls off with distance (same attenuation as PointLight) and
/// is smoothly masked outside the cone between innerAngle and outerAngle.
ACOMP()
struct SpotLight
{
    AFIELD() glm::vec3 direction{0.f, -1.f, 0.f}; ///< Aim in LOCAL space; the Transform rotates it into world.
    AFIELD() glm::vec3 color{1.f, 1.f, 1.f};      ///< Linear-RGB colour.
    AFIELD() float intensity   = 1.f;              ///< May be negative (light subtraction).
    AFIELD(min = 0) float radius   = 10.f;         ///< Maximum influence range in world units; never negative.
    AFIELD() float innerAngle  = 15.f;             ///< Half-angle of the full-brightness cone (degrees).
    AFIELD() float outerAngle  = 30.f;             ///< Half-angle of the cutoff cone (degrees).
    AFIELD() bool castsShadows = true;             ///< Whether this light renders a shadow map.
};

} // namespace Assisi::Runtime