/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PhysicsComponents.hpp
/// @brief ECS components for Jolt physics integration.

#include <Assisi/Prelude.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>

#include <cstdint>

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
/// AENUM so it reflects as a dropdown and serializes by value. A 1-byte
/// underlying is plenty for a handful of shapes (reflectgen records the width so
/// the inspector reads/writes it correctly). Each shape reads a different subset
/// of RigidBodyDescriptor's dimension fields; the descriptor uses AFIELD(radio)
/// so the inspector only shows the dimensions the chosen shape actually uses.
AENUM()
enum class ColliderShape : std::uint8_t
{
    Box,      ///< Axis-aligned box from `halfExtents`.
    Sphere,   ///< Sphere from `radius`.
    Capsule,  ///< Capsule (cylinder + hemispherical caps) from `radius` + `halfHeight`.
    Cylinder, ///< Cylinder from `radius` + `halfHeight`.
};

/// @brief Serializable descriptor for a rigid body's collider.
///
/// Stored in the level file; consumed at load time to create a RigidBody and the
/// underlying Jolt body. `shape` is a radio source: each dimension field lists
/// the shapes it applies to and vanishes from the inspector for the others (only
/// the fields that shape uses matter — a Sphere ignores `halfExtents`/`halfHeight`).
ACOMP()
struct RigidBodyDescriptor
{
    AFIELD(radioBroadcast) ColliderShape shape = ColliderShape::Box; ///< Collision primitive to build.
    AFIELD(radioListen = {source = shape, value = Box, behavior = vanish})
    glm::vec3 halfExtents{0.5f, 0.5f, 0.5f}; ///< Box half-extents in world units.
    AFIELD(min = 0.0, radioListen = {source = shape, value = {Sphere, Capsule, Cylinder}, behavior = vanish})
    float radius = 0.5f; ///< Sphere/Capsule/Cylinder radius.
    AFIELD(min = 0.0, radioListen = {source = shape, value = {Capsule, Cylinder}, behavior = vanish})
    float      halfHeight = 0.5f;         ///< Capsule/Cylinder half-height of the cylindrical part.
    AFIELD() bool      isStatic  = false; ///< True = immovable static body.
    AFIELD() bool      enableCCD = false; ///< Enable continuous collision detection (dynamic only).
};

} // namespace Assisi::Physics