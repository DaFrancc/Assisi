#pragma once

/// @file PhysicsComponents.hpp
/// @brief ECS component that links an entity to a Jolt rigid body.

#include <Assisi/Prelude.hpp>
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

} // namespace Assisi::Physics