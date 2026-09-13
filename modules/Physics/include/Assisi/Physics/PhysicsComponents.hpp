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

/// @brief What kind of thing a body is, for deciding what it interacts with.
///
/// A body is on exactly one channel and carries a mask of the channels it
/// collides with; two bodies interact only if each one's mask includes the
/// other's channel. The same pair applies to a query, which is a participant
/// like any other — a line-of-sight ray is something on @ref Visibility, and a
/// surface opts out of being seen by dropping that channel from its own mask.
///
/// Append only. An enumerator's value is its bit position in every mask stored
/// in a level file, so inserting one silently re-aims every mask authored under
/// the old numbering.
AENUM()
enum class CollisionChannel : std::uint8_t
{
    World,      ///< Ordinary physical matter: floors, walls, props, crates.
    Character,  ///< Player and NPC bodies.
    Trigger,    ///< A volume that detects what enters it and blocks nothing.
    Visibility, ///< Queries only: line of sight, editor picking.
    Camera,     ///< Queries only: camera collision.
    Count,
};

/// @brief Every channel — the mask a body carries unless it narrows it.
///
/// One bit per channel, so a body defaults to interacting with everything and an
/// author subtracts rather than having to enumerate. Built from `Count` so it
/// widens with the enum instead of needing a matching edit.
inline constexpr std::uint32_t AllChannels =
    (1u << static_cast<std::uint32_t>(CollisionChannel::Count)) - 1u;

/// @brief Serializable descriptor for a rigid body's collider.
///
/// Stored in the level file; consumed at load time to create a RigidBody and the
/// underlying Jolt body. `shape` is a radio source: each dimension field lists
/// the shapes it applies to and vanishes from the inspector for the others (only
/// the fields that shape uses matter — a Sphere ignores `halfExtents`/`halfHeight`).
///
/// Replicated, and load-bearing: under local simulation a client builds a real
/// dynamic body for every mirrored entity, and this descriptor is what it builds
/// it from. It is also the discriminator between the client's two kinds of
/// mirror — an entity with one is body-corrected, an entity without one is
/// interpolated.
ACOMP(replicable)
struct RigidBodyDescriptor
{
    AFIELD(radioBroadcast) ColliderShape shape = ColliderShape::Box; ///< Collision primitive to build.
    AFIELD(radioListen = {source = shape, value = Box, behavior = vanish})
    glm::vec3 halfExtents{0.5f, 0.5f, 0.5f}; ///< Box half-extents in world units.
    AFIELD(min = 0.0, radioListen = {source = shape, value = {Sphere, Capsule, Cylinder}, behavior = vanish})
    float radius = 0.5f; ///< Sphere/Capsule/Cylinder radius.
    AFIELD(min = 0.0, radioListen = {source = shape, value = {Capsule, Cylinder}, behavior = vanish})
    float halfHeight = 0.5f;              ///< Capsule/Cylinder half-height of the cylindrical part.

    /// The channels this body collides with — one bit per CollisionChannel, at
    /// that enumerator's value. Interaction needs both sides to agree, so
    /// clearing a bit here stops the pair whatever the other body says.
    ///
    /// uint32_t rather than uint16_t because 16-bit fields are not reflected;
    /// only the low `CollisionChannel::Count` bits are ever read.
    AFIELD(bitmask = CollisionChannel) uint32_t collidesWith = AllChannels;

    /// @brief Which channel this body is on.
    ///
    /// `Trigger` is not just a label: it makes the body a sensor, detected by
    /// what enters it and blocking nothing. That is why there is no separate
    /// "is trigger" flag — a solid body on the trigger channel, or a
    /// pass-through body on any other, cannot be expressed and could only be a
    /// mistake.
    AFIELD() CollisionChannel channel = CollisionChannel::World;

    /// True = immovable static body.
    ///
    /// For a `Trigger` body this chooses how faithfully it detects rather than
    /// whether it moves. Unticked builds a sensor that never sleeps: it notices
    /// bodies already at rest, including when the volume is moved onto them, and
    /// costs a place in the simulation's active set for the level's lifetime.
    /// Ticked builds one that costs nothing while nothing awake is near it, and
    /// which learns about a resting body only when that body is woken — which
    /// creating or moving the volume does for whatever it then encloses.
    AFIELD() bool isStatic  = false;
    AFIELD() bool enableCCD = false;      ///< Enable continuous collision detection (dynamic only).
};

/// @brief Makes a rigid body ricochet off whatever it touches.
///
/// Deliberately *not* Jolt restitution, which is a solver property applied while
/// the contact is being resolved. This is the gameplay-layer version: the
/// PhysicsWorld's contact log records the impact, and a system rewrites the
/// body's linear velocity on the next fixed step — reflecting it about the
/// contact normal and scaling it by @ref rebound. Only the linear velocity is
/// touched; spin, mass, and the solver's own response are left alone.
///
/// Needs a RigidBody to act on. Acts only on the step a body arrives, never
/// while it rests, and never on a sensor — nothing passed through resisted it.
///
/// Replicated, despite the bounce itself being a local guess at what the server's
/// bounce did. Under local simulation the client runs its own physics, and a
/// mirror missing this component does not bounce at all: it falls, rests, and is
/// snapped back up by every correction — a simulation continuously wrong in a way
/// the correction stream papers over, reading on screen as a body lagging its own
/// authoritative position. The component is authored data that changes ~never;
/// only its *effect* is local.
ACOMP(replicable)
struct Bounce
{
    /// Fraction of speed carried back out of an impact: 0 stops the body dead,
    /// 0.5 halves it, 1 returns it at full speed, and above 1 it *gains* speed on
    /// every bounce (which will run away — that is the author's choice, not a
    /// bug). Negative is meaningless; the inspector floors it here and the system
    /// clamps again on use, so a hand-edited level file can't invert a bounce.
    AFIELD(min = 0.0) float rebound = 1.f;
};

} // namespace Assisi::Physics