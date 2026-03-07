#pragma once

/// @file PhysicsWorld.hpp
/// @brief Jolt physics simulation wrapper.
///
/// One PhysicsWorld per scene. Call Update() each frame then SyncTransforms()
/// to push Jolt body positions/rotations back into ECS TransformComponents.

#include <Jolt/Jolt.h>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>

#include <memory>
#include <utility>

namespace Assisi::Physics
{

/// @brief Motion type for newly created bodies.
enum class BodyMotion
{
    Static,  ///< Immovable; collides but is never moved by the simulation.
    Dynamic, ///< Fully simulated; affected by gravity and collisions.
};

/// @brief Wraps a Jolt PhysicsSystem and exposes a minimal API for the game loop.
///
/// Construction initialises the Jolt library (RegisterTypes, Factory).
/// Destruction cleans up all bodies and unregisters Jolt types.
class PhysicsWorld
{
  public:
    PhysicsWorld();
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld &) = delete;
    PhysicsWorld &operator=(const PhysicsWorld &) = delete;

    /// @brief Creates an axis-aligned box body and returns its component.
    ///
    /// @param position     Centre of the box in world space.
    /// @param rotation     Initial orientation as a quaternion.
    /// @param halfExtents  Half-widths along each axis (box goes ±halfExtents).
    /// @param motion       Static bodies never move; dynamic bodies fall under gravity.
    RigidBodyComponent AddBox(glm::vec3 position, glm::quat rotation, glm::vec3 halfExtents, BodyMotion motion);

    /// @brief Advances the simulation by `deltaTime` seconds.
    void Update(float deltaTime);

    /// @brief Writes Jolt body positions/rotations back into TransformComponents.
    ///
    /// Only entities that have both a TransformComponent and a RigidBodyComponent
    /// are updated; dynamic bodies only (static bodies never move).
    void SyncTransforms(Assisi::ECS::Scene &scene);

    /// @brief Returns the current world-space position and rotation of a body.
    std::pair<glm::vec3, glm::quat> GetBodyTransform(const RigidBodyComponent &body) const;

    /// @brief Teleports a body to the given position and rotation, and reactivates it.
    void SetBodyTransform(const RigidBodyComponent &body, glm::vec3 position, glm::quat rotation);

    /// @brief Changes the motion type of an existing body at runtime.
    ///
    /// Useful for temporarily freezing a dynamic body (e.g. while editing in an
    /// inspector) and restoring it afterwards.  Switching to Dynamic also activates
    /// the body so gravity takes effect immediately.
    void SetBodyMotionType(const RigidBodyComponent &body, BodyMotion motion);

    /// @brief Removes and destroys all bodies, resetting the world to an empty state.
    void Clear();

    /// @brief Sets the gravity vector (default: {0, −9.81, 0}).
    void SetGravity(glm::vec3 gravity);

    /// @brief Returns the current gravity vector.
    glm::vec3 GetGravity() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace Assisi::Physics