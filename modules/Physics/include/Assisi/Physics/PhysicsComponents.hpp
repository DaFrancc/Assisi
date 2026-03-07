#pragma once

/// @file PhysicsComponents.hpp
/// @brief ECS components for Jolt physics integration.

#include <Assisi/Prelude.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>

namespace Assisi::Physics
{

/// @brief Tags an entity as having a Jolt physics body.
///
/// Trivially copyable — safe to store in SparseSet<T>.
/// The actual body is owned by the PhysicsWorld; this is just a handle.
struct RigidBodyComponent
{
    JPH::BodyID bodyId;
};

/// @brief Serializable descriptor for a box-shaped rigid body.
///
/// Stored in the level file; consumed at load time to create a
/// RigidBodyComponent and the underlying Jolt body.  Not used at runtime.
ACOMP()
struct RigidBodyDescriptor
{
    AFIELD() glm::vec3 halfExtents{0.5f, 0.5f, 0.5f}; ///< Box half-extents in world units.
    AFIELD() bool      isStatic = false;               ///< True = immovable static body.
};

} // namespace Assisi::Physics