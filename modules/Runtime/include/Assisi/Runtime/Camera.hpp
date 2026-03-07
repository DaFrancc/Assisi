#pragma once

/// @file Camera.hpp
/// @brief Camera utility functions derived from ECS components.
///
/// A camera is represented as an entity with both a TransformComponent
/// (position and orientation) and a CameraComponent (projection parameters).
///
/// Orientation convention: the camera looks along its local -Z axis.
/// All functions read from TransformComponent::worldMatrix, so
/// PropagateTransforms() must be called before using them each frame.

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Runtime/Components.hpp>

namespace Assisi::Runtime
{

/// @brief Returns the view matrix derived from the camera's world transform.
glm::mat4 ViewMatrix(const TransformComponent &transform);

/// @brief Returns a perspective projection matrix from the camera's parameters.
glm::mat4 ProjectionMatrix(const CameraComponent &camera, float aspectRatio);

/// @brief World-space forward direction (-Z column of the world matrix).
glm::vec3 ForwardDirection(const TransformComponent &transform);

/// @brief World-space right direction (+X column of the world matrix).
glm::vec3 RightDirection(const TransformComponent &transform);

/// @brief World-space up direction (+Y column of the world matrix).
glm::vec3 UpDirection(const TransformComponent &transform);

} // namespace Assisi::Runtime