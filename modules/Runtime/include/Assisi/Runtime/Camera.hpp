#pragma once

#ifndef CAMERA_HPP
#define CAMERA_HPP

/// @file Camera.hpp
/// @brief A look-at camera that produces a view matrix from world-space state.

#include <Assisi/Math/GLM.hpp>

namespace Assisi::Game
{
/// @brief Represents a view camera defined by a position and a look-at target.
///
/// The view matrix is computed on-demand from the stored world-space state.
/// Roll is resolved using the configurable up direction.
class Camera
{
  public:
    Camera() = default;

    /// @param worldPosition  Camera position in world space.
    /// @param lookAtTarget   World-space point the camera looks at.
    explicit Camera(const glm::vec3 &worldPosition, const glm::vec3 &lookAtTarget)
        : _worldPosition(worldPosition), _lookAtTarget(lookAtTarget)
    {
    }

    /* World-space state accessors */
    glm::vec3 WorldPosition() const { return _worldPosition; }
    glm::vec3 LookAtTarget() const { return _lookAtTarget; }
    glm::vec3 WorldUpDirection() const { return _worldUpDirection; }

    /* World-space mutators */
    void SetWorldPosition(const glm::vec3 &worldPosition) { _worldPosition = worldPosition; }

    void SetLookAtTarget(const glm::vec3 &lookAtTarget) { _lookAtTarget = lookAtTarget; }

    /// @brief Sets the reference up vector used to resolve camera roll.
    void SetWorldUpDirection(const glm::vec3 &worldUpDirection) { _worldUpDirection = worldUpDirection; }

    /// @brief Returns the view matrix computed from current position, target, and up direction.
    glm::mat4 ViewMatrix() const { return glm::lookAt(_worldPosition, _lookAtTarget, _worldUpDirection); }

    /// @name Derived camera-space directions
    /// These are recomputed each call; cache if used frequently.
    ///@{
    glm::vec3 ForwardDirection() const { return glm::normalize(_lookAtTarget - _worldPosition); }

    /// @brief Right axis, orthogonal to forward and the world up reference.
    glm::vec3 RightDirection() const { return glm::normalize(glm::cross(ForwardDirection(), _worldUpDirection)); }

    /// @brief True camera up, orthogonal to both right and forward.
    glm::vec3 UpDirection() const { return glm::normalize(glm::cross(RightDirection(), ForwardDirection())); }
    ///@}

  private:
    glm::vec3 _worldPosition{0.0f, 0.0f, 3.0f};
    glm::vec3 _lookAtTarget{0.0f, 0.0f, 0.0f};

    /// @brief Reference direction used to resolve camera roll.
    glm::vec3 _worldUpDirection{0.0f, 1.0f, 0.0f};
};
} // namespace Assisi::Game

#endif /* CAMERA_HPP */