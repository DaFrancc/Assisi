#pragma once

#ifndef CAMERA_HPP
#define CAMERA_HPP

#include <Assisi/Math/GLM.hpp>

namespace Assisi::Scene
{
class Camera
{
  public:
    Camera() = default;

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

    void SetWorldUpDirection(const glm::vec3 &worldUpDirection) { _worldUpDirection = worldUpDirection; }

    /* View matrix */
    glm::mat4 ViewMatrix() const { return glm::lookAt(_worldPosition, _lookAtTarget, _worldUpDirection); }

    /* Derived camera-space directions */
    glm::vec3 ForwardDirection() const { return glm::normalize(_lookAtTarget - _worldPosition); }

    glm::vec3 RightDirection() const { return glm::normalize(glm::cross(ForwardDirection(), _worldUpDirection)); }

    glm::vec3 UpDirection() const { return glm::normalize(glm::cross(RightDirection(), ForwardDirection())); }

  private:
    /* Persistent world-space state */
    glm::vec3 _worldPosition{0.0f, 0.0f, 3.0f};
    glm::vec3 _lookAtTarget{0.0f, 0.0f, 0.0f};

    /* Reference direction used to resolve camera roll */
    glm::vec3 _worldUpDirection{0.0f, 1.0f, 0.0f};
};
} // namespace Assisi::Scene

#endif /* CAMERA_HPP */