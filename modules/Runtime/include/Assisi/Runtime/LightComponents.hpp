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

/// @brief Infinite-distance directional light (sun / moon).
///
/// No position, no falloff.  Multiple directional lights are supported.
ACOMP()
struct DirectionalLight
{
    AFIELD() glm::vec3 direction{0.f, -1.f, 0.f}; ///< World-space direction the light shines (unit vector).
    AFIELD() glm::vec3 color{1.f, 1.f, 1.f};      ///< Linear-RGB colour.
    AFIELD() float intensity = 1.f;
    AFIELD() bool castsShadows = true; ///< Whether this light renders a shadow map.
};

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