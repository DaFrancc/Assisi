#pragma once

/// @file Camera.hpp
/// @brief Camera utility functions derived from ECS components.
///
/// A camera is represented as an entity with both a TransformComponent
/// (position and orientation) and a CameraComponent (projection parameters).
///
/// Orientation convention: the camera looks along its local -Z axis.
/// Set TransformComponent::rotation so that `rotation * (0,0,-1)` points in
/// the desired view direction, or use glm::quat_cast on a basis matrix built
/// from (right, up, -forward).

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Runtime/Components.hpp>

namespace Assisi::Runtime
{

/// @brief Returns the view matrix derived from the transform's position and rotation.
///
/// The camera looks along its local -Z axis (OpenGL convention).
glm::mat4 ViewMatrix(const TransformComponent &transform);

/// @brief Returns a perspective projection matrix from the camera's parameters.
///
/// @param camera      Camera projection parameters (fovDegrees, nearZ, farZ).
/// @param aspectRatio Viewport width / height.
glm::mat4 ProjectionMatrix(const CameraComponent &camera, float aspectRatio);

/// @brief World-space forward direction (-Z axis rotated by the transform's quaternion).
glm::vec3 ForwardDirection(const TransformComponent &transform);

/// @brief World-space right direction (+X axis rotated by the transform's quaternion).
glm::vec3 RightDirection(const TransformComponent &transform);

/// @brief World-space up direction (+Y axis rotated by the transform's quaternion).
glm::vec3 UpDirection(const TransformComponent &transform);

} // namespace Assisi::Runtime
