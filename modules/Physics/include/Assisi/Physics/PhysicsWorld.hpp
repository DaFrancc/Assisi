/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PhysicsWorld.hpp
/// @brief Jolt physics simulation wrapper.
///
/// One PhysicsWorld per scene. Per fixed step call Update() then CaptureState()
/// to snapshot the new body poses; once per render frame call
/// InterpolateTransforms() to blend the last two snapshots into ECS Transforms.
/// Because physics steps at a fixed rate but rendering does not, that blend is
/// what keeps physics-driven motion smooth on high-refresh displays instead of
/// beating against the step rate.

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
    RigidBody AddBox(glm::vec3 position, glm::quat rotation, glm::vec3 halfExtents, BodyMotion motion);

    /// @brief Advances the simulation by `deltaTime` seconds.
    void Update(float deltaTime);

    /// @brief Number of collision substeps Jolt runs per Update() call.
    ///
    /// Splitting a step into more substeps shrinks how far a fast body can move
    /// before the solver sees a contact, which reduces — and at a high enough
    /// count effectively eliminates — the impact penetration that reads as a
    /// body sinking into a surface and popping back out. Cost is roughly linear:
    /// N substeps ≈ N× the collision work per step. Clamped to [1, 16].
    void SetCollisionSteps(int steps);

    /// @brief Current collision-substep count (see SetCollisionSteps).
    int GetCollisionSteps() const;

    /// @brief Snapshots each dynamic body's pose for render interpolation.
    ///
    /// Call once per fixed step, immediately after Update(): it shifts the
    /// previous snapshot to the last-captured one and records the freshly
    /// stepped pose as the new current. InterpolateTransforms() then blends
    /// between those two. Static bodies never move and are skipped.
    void CaptureState();

    /// @brief Blends each dynamic body's previous/current snapshots into its
    /// Transform, `alpha` of the way from previous to current.
    ///
    /// Call once per render frame with the fixed-loop's interpolation alpha
    /// (`Application::GetInterpolationAlpha()`), which is the fraction of a
    /// physics step the accumulator holds. Only entities with both a Transform
    /// and a RigidBody are touched; static bodies are skipped, so their
    /// authored Transform is left intact. The written Transform is the *render*
    /// pose — the authoritative physics state is the current snapshot.
    void InterpolateTransforms(Assisi::ECS::Scene &scene, float alpha);

    /// @brief Returns the current world-space position and rotation of a body.
    std::pair<glm::vec3, glm::quat> GetBodyTransform(const RigidBody &body) const;

    /// @brief Returns a body's current linear (m/s) and angular (rad/s) velocity.
    ///
    /// Both are zero for a static body or one that isn't in the simulation, so
    /// callers can display the result unconditionally.
    std::pair<glm::vec3, glm::vec3> GetBodyVelocity(const RigidBody &body) const;

    /// @brief Whether a body's motion quality is currently LinearCast (CCD on).
    /// False for Discrete bodies, static bodies, or handles not in the simulation.
    bool IsBodyCCDEnabled(const RigidBody &body) const;

    /// @brief Teleports a body to the given position and rotation, and reactivates it.
    void SetBodyTransform(const RigidBody &body, glm::vec3 position, glm::quat rotation);

    /// @brief Replaces the collision shape of an existing box body with new half-extents.
    ///
    /// Use this to apply inspector edits to `halfExtents` at runtime without
    /// recreating the body.
    void ReshapeBox(const RigidBody &body, glm::vec3 halfExtents);

    /// @brief Enables or disables continuous collision detection (CCD) on a body.
    ///
    /// Only meaningful for dynamic bodies; no-op on static bodies.
    /// Uses Jolt's LinearCast motion quality for CCD, Discrete otherwise.
    void SetBodyCCD(const RigidBody &body, bool enable);

    /// @brief Changes the motion type of an existing body at runtime.
    ///
    /// Useful for temporarily freezing a dynamic body (e.g. while editing in an
    /// inspector) and restoring it afterwards.  Switching to Dynamic also activates
    /// the body so gravity takes effect immediately.
    void SetBodyMotionType(const RigidBody &body, BodyMotion motion);

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