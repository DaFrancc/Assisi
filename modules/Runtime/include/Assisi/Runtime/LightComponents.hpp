/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file LightComponents.hpp
/// @brief ECS component types for the three basic dynamic light types.
///
/// All three types are trivially copyable (plain data, no pointers).
///
/// Placement:
///   - DirectionalLightComponent  — direction is stored directly on the component;
///     no TransformComponent is needed (it has no world position).
///   - PointLightComponent        — requires a TransformComponent for world position.
///   - SpotLightComponent         — requires a TransformComponent for world position;
///     direction is stored on the component.

#include <Assisi/Prelude.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::Runtime
{

/// @brief Infinite-distance directional light (sun / moon).
///
/// No position, no falloff.  Multiple directional lights are supported.
ACOMP()
struct DirectionalLightComponent
{
    AFIELD() glm::vec3 direction{0.f, -1.f, 0.f}; ///< World-space direction toward the light (unit vector).
    AFIELD() glm::vec3 color{1.f, 1.f, 1.f};      ///< Linear-RGB colour.
    AFIELD() float     intensity = 1.f;
};

/// @brief Omnidirectional point light with distance falloff.
///
/// Requires TransformComponent for world position.
/// Uses windowed inverse-square attenuation: zero contribution beyond `radius`.
ACOMP()
struct PointLightComponent
{
    AFIELD() glm::vec3 color{1.f, 1.f, 1.f}; ///< Linear-RGB colour.
    AFIELD() float     intensity = 1.f;
    AFIELD() float     radius    = 10.f;      ///< Maximum influence range in world units.
};

/// @brief Cone-restricted point light (flashlight / stage spotlight).
///
/// Requires TransformComponent for world position.
/// Intensity falls off with distance (same attenuation as PointLight) and
/// is smoothly masked outside the cone between innerAngle and outerAngle.
ACOMP()
struct SpotLightComponent
{
    AFIELD() glm::vec3 direction{0.f, -1.f, 0.f}; ///< World-space direction (unit vector).
    AFIELD() glm::vec3 color{1.f, 1.f, 1.f};      ///< Linear-RGB colour.
    AFIELD() float     intensity   = 1.f;
    AFIELD() float     radius      = 10.f;         ///< Maximum influence range in world units.
    AFIELD() float     innerAngle  = 15.f;         ///< Half-angle of the full-brightness cone (degrees).
    AFIELD() float     outerAngle  = 30.f;         ///< Half-angle of the cutoff cone (degrees).
};

} // namespace Assisi::Runtime