/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Camera.hpp
/// @brief Camera utility functions derived from ECS components.
///
/// A camera is represented as an entity with both a Transform
/// (position and orientation) and a Camera (projection parameters).
///
/// Orientation convention: the camera looks along its local -Z axis.
/// The functions take the camera's world matrix — for a scene camera,
/// its ECS::WorldMatrix after PropagateTransforms has run this frame.

#include <cstdint>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Runtime/Components.hpp>

namespace Assisi::Runtime
{

/// @brief Returns the view matrix of a camera at @p world.
glm::mat4 ViewMatrix(const glm::mat4 &world);

/// @brief Returns a perspective projection matrix from the camera's parameters.
glm::mat4 ProjectionMatrix(const Camera &camera, float aspectRatio);

/// @brief Width over height, or 1 for a target with no height — a minimised
/// window reports zero, and a projection built from a division by it is NaN.
float AspectRatio(int32_t width, int32_t height);

/// @brief World-space forward direction (-Z column of the world matrix).
glm::vec3 ForwardDirection(const glm::mat4 &world);

/// @brief World-space right direction (+X column of the world matrix).
glm::vec3 RightDirection(const glm::mat4 &world);

/// @brief World-space up direction (+Y column of the world matrix).
glm::vec3 UpDirection(const glm::mat4 &world);

/// @brief The world matrix of a free camera posed by @p transform, which is
/// in world space.
glm::mat4 CameraWorldMatrix(const Transform &transform);

} // namespace Assisi::Runtime
