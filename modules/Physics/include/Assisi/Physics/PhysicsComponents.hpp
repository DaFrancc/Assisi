/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
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
///
/// ACOMP(transient): registered only so a Scene can store it by ComponentId.
/// It is never serialized — the bodyId is a live runtime handle, meaningless
/// across runs. RigidBodyDescriptor (below) is the serialized form; a RigidBody
/// is (re)created from it at load time.
ACOMP(transient)
struct RigidBody
{
    JPH::BodyID bodyId;
};

/// @brief Which collision primitive a RigidBodyDescriptor builds.
///
/// AENUM so it reflects as a dropdown and serializes by value. Plain `enum class`
/// (a 4-byte `int` underlying, which reflected enums use), since there are only a
/// handful of shapes. Each shape reads a different subset of RigidBodyDescriptor's
/// dimension fields (see there).
AENUM()
enum class ColliderShape
{
    Box,      ///< Axis-aligned box from `halfExtents`.
    Sphere,   ///< Sphere from `radius`.
    Capsule,  ///< Capsule (cylinder + hemispherical caps) from `radius` + `halfHeight`.
    Cylinder, ///< Cylinder from `radius` + `halfHeight`.
};

/// @brief Serializable descriptor for a rigid body's collider.
///
/// Stored in the level file; consumed at load time to create a RigidBody and the
/// underlying Jolt body. `shape` selects the primitive; only the fields that
/// shape uses matter (a Sphere ignores `halfExtents`/`halfHeight`, etc.).
ACOMP()
struct RigidBodyDescriptor
{
    AFIELD() ColliderShape shape = ColliderShape::Box; ///< Collision primitive to build.
    AFIELD() glm::vec3 halfExtents{0.5f, 0.5f, 0.5f};  ///< Box half-extents in world units.
    AFIELD(min = 0.0) float radius     = 0.5f;         ///< Sphere/Capsule/Cylinder radius.
    AFIELD(min = 0.0) float halfHeight = 0.5f;         ///< Capsule/Cylinder half-height of the cylindrical part.
    AFIELD() bool      isStatic  = false;              ///< True = immovable static body.
    AFIELD() bool      enableCCD = false;              ///< Enable continuous collision detection (dynamic only).
};

} // namespace Assisi::Physics