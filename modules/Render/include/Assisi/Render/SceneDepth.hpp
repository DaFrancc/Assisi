/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SceneDepth.hpp
/// @brief How the scene's depth buffer is laid out: reversed Z, the near plane
/// at one and the far plane at zero, in 32-bit float.
///
/// Perspective crowds distant surfaces into a sliver of depth, and float is
/// finest near zero. Storing the far plane at zero lays the two against each
/// other, so two surfaces a ten-thousandth of their distance apart stay in
/// order from a hand's width away to 20 km; with the far plane at one, they
/// fight from a few hundred metres out. Every pass that projects, clears, tests
/// or reads the scene's depth takes it from here. Shadow maps keep their own
/// conventional depth.

#include <nvrhi/nvrhi.h>

#include <Assisi/Math/GLM.hpp>

namespace Assisi::Render
{

/// The depth stored at the camera's near plane.
inline constexpr float kSceneDepthNear = 1.f;

/// The depth stored at the far plane, what the depth buffer is cleared to, and
/// where the sky is drawn.
inline constexpr float kSceneDepthFar = 0.f;

/// The test that keeps the nearer of two surfaces.
inline constexpr nvrhi::ComparisonFunc kSceneDepthNearer = nvrhi::ComparisonFunc::Greater;

/// The same, also passing a surface at exactly the stored depth: what a pass
/// drawing over a depth prepass, or lines over the surfaces they outline, uses.
inline constexpr nvrhi::ComparisonFunc kSceneDepthNearerOrSame = nvrhi::ComparisonFunc::GreaterOrEqual;

/// @brief The camera's projection, with the scene's depth layout.
[[nodiscard]] inline glm::mat4 ScenePerspective(float fovYRadians, float aspectRatio, float nearZ, float farZ)
{
    // GLM's zero-to-one projection stores its near argument at zero and its far
    // argument at one, so handing it the planes swapped stores them reversed.
    return glm::perspective(fovYRadians, aspectRatio, farZ, nearZ);
}

} // namespace Assisi::Render
