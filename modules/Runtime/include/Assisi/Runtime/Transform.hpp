#pragma once

#ifndef TRANSFORM_HPP
#define TRANSFORM_HPP

/// @file Transform.hpp
/// @brief World-space transform (position, rotation, scale) with quaternion storage.
///
/// Rotation is stored as a normalised quaternion.  Euler-angle accessors are
/// provided as a convenience interface; note that the quaternion → Euler
/// conversion is not unique and is subject to gimbal ambiguity.

#include <Assisi/Math/GLM.hpp>

namespace Assisi::Runtime
{
/// @brief Represents a world-space TRS (translate–rotate–scale) transform.
class Transform
{
  public:
    glm::vec3 WorldPosition() const { return _worldPosition; }
    glm::vec3 WorldScale() const { return _worldScale; }

    void SetWorldPosition(const glm::vec3 &worldPosition) { _worldPosition = worldPosition; }
    void SetWorldScale(const glm::vec3 &worldScale) { _worldScale = worldScale; }

    /// @brief Returns the rotation as a normalised quaternion (authoritative representation).
    glm::quat WorldRotationQuaternion() const { return _worldRotationQuaternion; }

    /// @brief Sets the rotation; the quaternion is normalised before storing.
    void SetWorldRotationQuaternion(const glm::quat &worldRotationQuaternion)
    {
        _worldRotationQuaternion = glm::normalize(worldRotationQuaternion);
    }

    /// @name Euler rotation interface
    /// Convenience wrappers that convert to/from the internal quaternion.
    /// The quaternion → Euler conversion is not unique.
    ///@{
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
    ///@}

    /// @name Incremental rotation
    ///@{

    /// @brief Post-multiplies the current rotation by `rotationDeltaQuaternion`.
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
    ///@}

    /// @brief Returns the 4×4 world matrix (T × R × S).
    glm::mat4 WorldMatrix() const
    {
        glm::mat4 result(1.0f);

        result = glm::translate(result, _worldPosition);
        result *= glm::toMat4(_worldRotationQuaternion);
        result = glm::scale(result, _worldScale);

        return result;
    }

    /// @name Local-space axis directions
    /// Derived from the current rotation quaternion.
    ///@{
    glm::vec3 ForwardDirection() const
    {
        return glm::normalize(_worldRotationQuaternion * glm::vec3(0.0f, 0.0f, -1.0f));
    }

    glm::vec3 RightDirection() const { return glm::normalize(_worldRotationQuaternion * glm::vec3(1.0f, 0.0f, 0.0f)); }

    glm::vec3 UpDirection() const { return glm::normalize(_worldRotationQuaternion * glm::vec3(0.0f, 1.0f, 0.0f)); }
    ///@}

  private:
    glm::vec3 _worldPosition{0.0f, 0.0f, 0.0f};

    /// @brief Authoritative rotation — always normalised.
    glm::quat _worldRotationQuaternion{1.0f, 0.0f, 0.0f, 0.0f};

    glm::vec3 _worldScale{1.0f, 1.0f, 1.0f};
};
} /* namespace Assisi::Runtime */

#endif /* TRANSFORM_HPP */