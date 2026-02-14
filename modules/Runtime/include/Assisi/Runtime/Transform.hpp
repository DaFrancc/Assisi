#pragma once

#ifndef TRANSFORM_HPP
#define TRANSFORM_HPP

#include <Assisi/Math/GLM.hpp>

namespace Assisi::Game
{
class Transform
{
  public:
    glm::vec3 WorldPosition() const { return _worldPosition; }
    glm::vec3 WorldScale() const { return _worldScale; }

    void SetWorldPosition(const glm::vec3 &worldPosition) { _worldPosition = worldPosition; }

    void SetWorldScale(const glm::vec3 &worldScale) { _worldScale = worldScale; }

    /* Quaternion rotation (authoritative internal representation). */
    glm::quat WorldRotationQuaternion() const { return _worldRotationQuaternion; }

    void SetWorldRotationQuaternion(const glm::quat &worldRotationQuaternion)
    {
        _worldRotationQuaternion = glm::normalize(worldRotationQuaternion);
    }

    /* Euler rotation interface (convenience; conversion is not unique). */
    glm::vec3 WorldRotationEulerRadians() const
    {
        /* GLM returns Euler angles in radians. */
        return glm::eulerAngles(_worldRotationQuaternion);
    }

    void SetWorldRotationEulerRadians(const glm::vec3 &eulerAnglesRadians)
    {
        _worldRotationQuaternion = glm::normalize(glm::quat(eulerAnglesRadians));
    }

    glm::vec3 WorldRotationEulerDegrees() const { return glm::degrees(WorldRotationEulerRadians()); }

    void SetWorldRotationEulerDegrees(const glm::vec3 &eulerAnglesDegrees)
    {
        SetWorldRotationEulerRadians(glm::radians(eulerAnglesDegrees));
    }

    void RotateByQuaternion(const glm::quat &rotationDeltaQuaternion)
    {
        _worldRotationQuaternion = glm::normalize(rotationDeltaQuaternion * _worldRotationQuaternion);
    }

    void RotateByEulerRadians(const glm::vec3 &rotationDeltaEulerRadians)
    {
        RotateByQuaternion(glm::quat(rotationDeltaEulerRadians));
    }

    void RotateByEulerDegrees(const glm::vec3 &rotationDeltaEulerDegrees)
    {
        RotateByEulerRadians(glm::radians(rotationDeltaEulerDegrees));
    }

    glm::mat4 WorldMatrix() const
    {
        glm::mat4 result(1.0f);

        result = glm::translate(result, _worldPosition);
        result *= glm::toMat4(_worldRotationQuaternion);
        result = glm::scale(result, _worldScale);

        return result;
    }

    glm::vec3 ForwardDirection() const
    {
        return glm::normalize(_worldRotationQuaternion * glm::vec3(0.0f, 0.0f, -1.0f));
    }

    glm::vec3 RightDirection() const { return glm::normalize(_worldRotationQuaternion * glm::vec3(1.0f, 0.0f, 0.0f)); }

    glm::vec3 UpDirection() const { return glm::normalize(_worldRotationQuaternion * glm::vec3(0.0f, 1.0f, 0.0f)); }

  private:
    glm::vec3 _worldPosition{0.0f, 0.0f, 0.0f};

    /* Authoritative rotation storage. */
    glm::quat _worldRotationQuaternion{1.0f, 0.0f, 0.0f, 0.0f};

    glm::vec3 _worldScale{1.0f, 1.0f, 1.0f};
};
} /* namespace Assisi::Game */

#endif /* TRANSFORM_HPP */