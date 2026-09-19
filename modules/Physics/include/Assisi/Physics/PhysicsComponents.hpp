/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PhysicsComponents.hpp
/// @brief ECS components for Jolt physics integration.

#include <Assisi/Prelude.hpp>
#include <Assisi/Core/Bitmask.hpp>
#include <Assisi/ECS/Entity.hpp>
#include <Assisi/Math/GLM.hpp>

#include <cstdint>

namespace Assisi::Physics
{

/// @brief The value a handle carries when it names nothing.
///
/// All ones, which is also what the simulation underneath uses, so the two
/// agree without a conversion having to special-case the empty handle.
inline constexpr uint32_t InvalidPhysicsHandle = 0xFFFFFFFFu;

/// @brief Opaque handle to one body in a PhysicsWorld.
///
/// A number only the world that issued it can interpret, and only for as long as
/// that body lives. Nothing outside PhysicsWorld may take it apart — which is the
/// point: the simulation behind it can be replaced without this type or anything
/// holding one changing.
///
/// Deliberately **not** a Core::StrongId. That marker declares a type's wire
/// form, and this one has none: a body handle means nothing in another process,
/// so making it serializable could only ever produce a bug that surfaced in
/// multiplayer.
struct BodyId
{
    uint32_t value = InvalidPhysicsHandle;

    /// @brief Whether this names a body at all. A default-constructed handle,
    /// and the one a failed creation returns, do not.
    [[nodiscard]] constexpr bool IsValid() const { return value != InvalidPhysicsHandle; }

    friend constexpr bool operator==(BodyId, BodyId) = default;
};

/// @brief Opaque handle to one character controller in a PhysicsWorld.
///
/// The same contract as @ref BodyId, and not a Core::StrongId for the same
/// reason.
struct CharacterId
{
    uint32_t value = InvalidPhysicsHandle;

    [[nodiscard]] constexpr bool IsValid() const { return value != InvalidPhysicsHandle; }

    friend constexpr bool operator==(CharacterId, CharacterId) = default;
};

/// @brief Tags an entity as having a physics body.
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
    BodyId bodyId;
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

/// @brief Every channel — the mask a body carries unless it narrows it, so a
/// body defaults to interacting with everything and an author subtracts rather
/// than having to enumerate.
inline constexpr Core::Bitmask<CollisionChannel> AllChannels = Core::Bitmask<CollisionChannel>::All();

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

    /// The channels this body collides with. Interaction needs both sides to
    /// agree, so clearing a bit here stops the pair whatever the other body says.
    AFIELD() Core::Bitmask<CollisionChannel> collidesWith = AllChannels;

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

/// @brief What is under a character, and therefore what it may do next.
///
/// Mirrors the four cases the character solver distinguishes rather than
/// collapsing them to a bool, because each one wants a different response:
/// @ref OnGround may jump, @ref OnSteepGround is sliding and must not,
/// @ref NotSupported is touching something that cannot hold it, and @ref InAir
/// is touching nothing at all.
AENUM()
enum class GroundState : std::uint8_t
{
    OnGround,      ///< Standing on a surface within the walkable slope.
    OnSteepGround, ///< On a surface too steep to hold; sliding down it.
    NotSupported,  ///< Touching something, held up by none of it.
    InAir,         ///< Touching nothing.
    Count,
};

/// @brief Which of a character's two shapes is current.
///
/// Two rather than a continuous height because standing up is a question that
/// can be refused — the shape has to fit — and a refusal is only answerable
/// about a specific shape.
AENUM()
enum class Stance : std::uint8_t
{
    Standing,
    Crouching,
    Count,
};

/// @brief What a character's last simulation step left behind.
///
/// Read rather than written: everything here is produced by the step and
/// overwritten by the next one. A system that wants to *change* a character
/// writes @ref Character's intent fields instead.
///
/// Not a component of its own — it lives inside @ref Character, which is
/// transient, so none of this is serialized or replicated.
struct CharacterState
{
    /// Velocity after the step, so an animation graph reads what the character
    /// actually did rather than what it asked for. Includes the ground's own
    /// motion when riding a platform.
    glm::vec3 velocity{0.f};

    /// Unit normal of the surface below, pointing up out of it. Zero when
    /// @ref ground is InAir. Meaningful even when the slope is too steep, which
    /// is what lets a slide be aimed down the fall line.
    glm::vec3 groundNormal{0.f};

    /// World-space velocity of whatever is underfoot, zero for static ground.
    /// Already folded into @ref velocity; exposed separately so gameplay can
    /// tell "moving because the floor moved" from "moving because I walked".
    glm::vec3 groundVelocity{0.f};

    /// The entity underfoot, or NullEntity when there is none or its body has
    /// no entity. Check it is still alive before acting on it — the body may
    /// have been destroyed since the step that recorded it.
    ECS::Entity groundEntity{ECS::NullEntity};

    /// Seconds since the character was last @ref GroundState::OnGround, zero
    /// while it still is. What an animation graph filters a one-frame stair
    /// flicker with.
    float timeSinceGrounded = 0.f;

    GroundState ground = GroundState::InAir;

    /// The stance the character is **actually** in, which is not always the one
    /// it was asked for: standing up under a low ceiling leaves it crouched.
    Stance stance = Stance::Standing;

    /// Whether a jump asked for now would fire.
    ///
    /// Deliberately not `ground == OnGround`. Within the descriptor's coyote
    /// time a character that has already left the ground may still jump, and one
    /// that has jumped and not yet landed may not even if it is momentarily
    /// touching something. Gameplay and animation should ask this; @ref ground
    /// stays the simulation's plain answer about what is underfoot.
    bool canJump = false;
};

/// @brief Serializable description of a character's shape and how it moves.
///
/// Every field here is something a designer tunes. The solver tolerances that
/// only a physics programmer would touch are named constants inside
/// PhysicsWorld.cpp instead, so this struct stays a list of decisions about feel
/// rather than a wall of numbers.
///
/// A character is always on CollisionChannel::Character — there is no channel
/// field, because a character on any other channel would be a rigid body with
/// extra steps, and one on Trigger could not be a sensor and a solid volume at
/// once.
///
/// An entity carries this **or** a RigidBodyDescriptor, never both: a character
/// already owns a rigid body (its inner body), and a second one would collide
/// with its own character.
ACOMP(replicable)
struct CharacterDescriptor
{
    /// The channels this character collides with. Interaction needs both sides to
    /// agree, so clearing a bit here stops the pair whatever the other body says.
    AFIELD() Core::Bitmask<CollisionChannel> collidesWith = AllChannels;

    AFIELD(min = 0.0) float radius = 0.3f; ///< Capsule radius; half the character's width.

    /// Half the height of the capsule's cylindrical part, so the standing
    /// character is `2 * (halfHeight + radius)` tall. The default is a 1.8 m
    /// figure.
    AFIELD(min = 0.0) float halfHeight = 0.6f;

    /// @ref halfHeight while crouching. Clamped to @ref halfHeight on use: a
    /// crouch taller than standing is not a crouch.
    AFIELD(min = 0.0) float crouchHalfHeight = 0.3f;

    /// Steepest surface that still counts as ground. Above it the character is
    /// OnSteepGround and slides. Capped below 90 because a vertical surface is a
    /// wall — at exactly 90 every wall becomes a floor and the character walks
    /// up it.
    AFIELD(min = 0.0, max = 89.0) float maxSlopeDegrees = 50.f;

    /// Tallest step the character climbs without jumping, and equally the
    /// furthest it is held down to the floor while descending one — a stair
    /// walked up must be walkable back down. Clamped to the capsule's standing
    /// half-height on use: a step taller than that is a wall, whatever the
    /// author typed.
    AFIELD(min = 0.0) float maxStepHeight = 0.4f;

    AFIELD(min = 0.0) float walkSpeed = 5.f; ///< Top speed on the ground (m/s).

    /// @ref walkSpeed multiplier while crouching. A scale rather than a second
    /// speed so retuning the walk carries the crouch with it.
    AFIELD(min = 0.0) float crouchSpeedScale = 0.4f;

    /// Upward speed a jump starts with (m/s). At the default gravity 6 m/s
    /// clears about 1.8 m.
    AFIELD(min = 0.0) float jumpSpeed = 6.f;

    /// How hard the character accelerates toward its requested velocity while
    /// on the ground (m/s²). An acceleration rather than a blend fraction so
    /// the feel does not change with the fixed-step rate. The default reaches
    /// @ref walkSpeed in under a tenth of a second, which reads as instant;
    /// lower it for weight, and far lower for ice.
    AFIELD(min = 0.0) float groundAcceleration = 60.f;

    /// The same while airborne (m/s²). Zero keeps whatever horizontal velocity
    /// the jump launched with and ignores steering entirely; the default
    /// reverses a full-speed jump in about half a second.
    AFIELD(min = 0.0) float airAcceleration = 10.f;

    /// Multiplies the world's gravity for this character alone. Above 1 falls
    /// heavier and jumps shorter; the arc is what most of a character's weight
    /// reads as.
    AFIELD(min = 0.0) float gravityScale = 1.f;

    /// Mass used when pushing down on what it stands on, and when deciding how
    /// far a push moves it (kg).
    AFIELD(min = 0.0) float mass = 70.f;

    /// Greatest force the character can push another body with (N). Zero makes
    /// it unable to shift anything: bodies become immovable walls to it.
    AFIELD(min = 0.0) float pushStrength = 100.f;

    /// How long after walking off a ledge a jump still counts as a ground jump
    /// (seconds). Zero is literal ground-only, which reads as a jump that
    /// sometimes ignores the button — the player pressed it a frame after the
    /// edge and nothing happened.
    AFIELD(min = 0.0) float coyoteTime = 0.1f;

    /// How long before landing a jump request is remembered (seconds). Covers
    /// the opposite mistake: the button pressed a frame before touching down.
    AFIELD(min = 0.0) float jumpBufferTime = 0.15f;

    /// Whether this character can shove other bodies at all. Distinct from
    /// @ref pushStrength being zero only in intent; both are honoured.
    AFIELD() bool canPushBodies = true;

    /// Whether other bodies can shove this character. False makes it immovable
    /// by anything but its own movement — what an NPC that must hold its mark
    /// wants.
    AFIELD() bool canBePushed = true;
};

/// @brief Tags an entity as being driven by a character controller, and carries
/// what it is being asked to do.
///
/// Trivially copyable — safe to store in SparseSet<T>. The controller itself is
/// owned by the PhysicsWorld; @ref id is a handle into it.
///
/// The intent fields are the whole input surface. Whatever fills them — a
/// keyboard, an AI, a replicated command — the character behaves the same, which
/// is what lets one controller serve players and NPCs. They are read once per
/// fixed step and @ref jump is cleared when it is consumed.
///
/// **The entity's Transform sits at the character's feet**, not at the middle of
/// the capsule, so an author places a character by standing it on the floor.
///
/// ACOMP(transient): registered only so a Scene can store it by ComponentId. It
/// is never serialized — @ref id is a live runtime handle, meaningless across
/// runs, and @ref state describes a step that has already happened.
/// CharacterDescriptor is the serialized form; a Character is (re)created from
/// it at load time.
ACOMP(transient)
struct Character
{
    /// What the last step left behind. Refreshed by CharacterMoveSystem before
    /// it reads intent, so a system ordered after that one sees this step's
    /// answer rather than the previous one's.
    CharacterState state;

    /// Where the character is trying to go, in world space, as a direction of
    /// magnitude at most 1 — the scale to a real speed is the descriptor's, so
    /// the same input drives a walk and a crouch. The vertical component is
    /// ignored: going up is @ref jump's business and going down is gravity's.
    glm::vec3 move{0.f};

    /// Handle into the PhysicsWorld's characters.
    CharacterId id;

    /// The stance being asked for, held for as long as it is wanted rather than
    /// pulsed. Asking to stand under something too low fails silently and is
    /// retried every step, so a character stands back up by itself once it walks
    /// clear; CharacterState::stance is what it actually managed.
    Stance stance = Stance::Standing;

    /// A request, not a state: set it to ask for a jump, and the controller
    /// clears it once it has either jumped or let the request expire.
    ///
    /// Remembered briefly rather than only on the step it arrives, so a jump
    /// asked for a few milliseconds before landing still fires on the landing
    /// step instead of being swallowed. Held down, it re-arms every step and the
    /// character jumps again as soon as it may.
    bool jump = false;
};

} // namespace Assisi::Physics