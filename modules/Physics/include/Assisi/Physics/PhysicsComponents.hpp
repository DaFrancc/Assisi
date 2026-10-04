/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PhysicsComponents.hpp
/// @brief ECS components for Jolt physics integration.

#include <Assisi/Prelude.hpp>
#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/Bitmask.hpp>
#include <Assisi/ECS/Entity.hpp>
#include <Assisi/Math/GLM.hpp>

#include <cstdint>

namespace Assisi::Physics
{

/// @brief Which collision primitive a Collider builds.
///
/// AENUM so it reflects as a dropdown and serializes by value. Each shape reads
/// a different subset of Collider's dimension fields, which use AFIELD(radio)
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

/// @brief Which way a RigidBody moves.
///
/// There is no static: an immovable thing is a Collider with no RigidBody.
AENUM()
enum class MotionType : std::uint8_t
{
    Dynamic,   ///< Moved by the simulation: gravity, collisions, velocity.
    Kinematic, ///< Moved only by its Transform; pushes dynamic bodies and is pushed by nothing.
    Count,
};

/// @brief One degree of freedom a RigidBody can be held in.
AENUM()
enum class LockedAxis : std::uint8_t
{
    LinearX,
    LinearY,
    LinearZ,
    AngularX,
    AngularY,
    AngularZ,
    Count,
};

/// @brief A collision shape, and what touching it is like.
///
/// On its own it is static geometry: a wall, a floor, a trigger volume that
/// never moves. Add a RigidBody to make it move. The shape is built at the
/// entity's world scale, composed through its parents; a sphere or a capsule
/// takes one scale on every axis and a cylinder one across its round axes, so
/// a non-uniform scale on those is clamped, with a warning.
///
/// `shape` is a radio source: each dimension field lists the shapes it applies
/// to and vanishes from the inspector for the others.
///
/// Replicated, and load-bearing: a client builds its own body for a mirrored
/// entity from this and its RigidBody.
ACOMP(replicable, requires = {Transform}, excludes = {Character})
struct Collider
{
    /// Turns the shape about the entity's origin.
    AFIELD() glm::quat offsetRotation{1.f, 0.f, 0.f, 0.f};

    /// A cooked collision shape. Not read by the physics world yet: the shape
    /// is always the primitive below.
    AFIELD() Core::AssetId collisionAsset;

    AFIELD(radioListen = {source = shape, value = Box, behavior = vanish})
    glm::vec3 halfExtents{0.5f, 0.5f, 0.5f}; ///< Box half-extents, before scale.

    /// Moves the shape away from the entity's origin, in the entity's own
    /// axes, so it is scaled with the shape.
    AFIELD() glm::vec3 offsetPosition{0.f};

    AFIELD(min = 0.0, radioListen = {source = shape, value = {Sphere, Capsule, Cylinder}, behavior = vanish})
    float radius = 0.5f; ///< Sphere/Capsule/Cylinder radius, before scale.
    AFIELD(min = 0.0, radioListen = {source = shape, value = {Capsule, Cylinder}, behavior = vanish})
    float halfHeight = 0.5f; ///< Capsule/Cylinder half-height of the cylindrical part, before scale.

    /// How strongly a surface sliding across this one is held back. Two
    /// touching bodies use the geometric mean of theirs.
    AFIELD(min = 0.0) float friction = 0.2f;

    /// How much speed an impact gives back: 0 stops dead, 1 loses nothing. Two
    /// touching bodies use the larger of theirs.
    AFIELD(min = 0.0, max = 1.0) float restitution = 0.f;

    /// The channels this collider collides with. Interaction needs both sides
    /// to agree, so clearing a bit here stops the pair whatever the other says.
    AFIELD() Core::Bitmask<CollisionChannel> collidesWith = AllChannels;

    /// @brief Which channel this collider is on.
    ///
    /// `Trigger` is not just a label: it makes the collider a sensor, detected
    /// by what enters it and blocking nothing. That is why there is no
    /// separate "is trigger" flag — a solid body on the trigger channel, or a
    /// pass-through body on any other, cannot be expressed and could only be a
    /// mistake.
    AFIELD() CollisionChannel channel = CollisionChannel::World;

    AFIELD(radioBroadcast) ColliderShape shape = ColliderShape::Box; ///< Collision primitive to build.
};

/// @brief Makes a Collider move.
///
/// Every field is about responding to forces, which is why a wall has none of
/// them: static geometry is a Collider with no RigidBody. Edits apply to the
/// live body; adding or removing a RigidBody rebuilds it.
///
/// No Parent: the simulation owns this entity's pose, so it cannot also be
/// relative to another entity's.
ACOMP(replicable, requires = {Transform, BodyState}, excludes = {Parent, Character})
struct RigidBody
{
    /// Kilograms. 0 takes the mass from the shape's volume at the density of
    /// water.
    AFIELD(min = 0.0) float mass = 0.f;

    /// Fraction of linear speed lost per second, as though through air.
    AFIELD(min = 0.0) float linearDamping = 0.05f;

    /// Fraction of spin lost per second.
    AFIELD(min = 0.0) float angularDamping = 0.05f;

    /// Multiplies the world's gravity for this body alone. 0 floats.
    AFIELD() float gravityScale = 1.f;

    /// The degrees of freedom the body may not move in. Locking every
    /// rotation keeps a body upright; locking a translation keeps it in a
    /// plane.
    AFIELD() Core::Bitmask<LockedAxis> lockedAxes;

    AFIELD() MotionType motion = MotionType::Dynamic;

    /// Continuous collision detection: a fast small body is swept rather than
    /// moved, so it cannot pass through a thin wall between two steps.
    AFIELD() bool ccd = false;

    /// @brief Whether the body may fall asleep when it comes to rest.
    ///
    /// A sleeping body costs nothing until something wakes it. A kinematic
    /// sensor that is never allowed to sleep notices bodies already at rest,
    /// including when the volume is moved onto them; one that may sleep learns
    /// about a resting body only when that body is woken.
    AFIELD() bool allowSleep = true;
};

/// @brief What a RigidBody is doing, after the last step.
///
/// The physics world writes it after every step for each body that moved.
/// Writing `linearVelocity` or `angularVelocity` sets the body's velocity
/// before the next step, waking it; a velocity written before the body exists
/// is the one it starts with. `asleep` is the simulation's answer and a write
/// to it does nothing. When the body is destroyed this goes back to rest.
ACOMP(transient, tracked)
struct BodyState
{
    AFIELD() glm::vec3 linearVelocity{0.f};  ///< m/s.
    AFIELD() glm::vec3 angularVelocity{0.f}; ///< rad/s.
    AFIELD() bool asleep = false;
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
/// Needs a body to act on. Acts only on the step a body arrives, never
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
/// Every Character brings one, and the physics world writes it after every
/// step. Read rather than written: a system that wants to *change* a character
/// writes its CharacterIntent instead.
ACOMP(transient)
struct CharacterState
{
    /// Velocity after the step, so an animation graph reads what the character
    /// actually did rather than what it asked for. Includes the ground's own
    /// motion when riding a platform.
    AFIELD() glm::vec3 velocity{0.f};

    /// Unit normal of the surface below, pointing up out of it. Zero when
    /// @ref ground is InAir. Meaningful even when the slope is too steep, which
    /// is what lets a slide be aimed down the fall line.
    AFIELD() glm::vec3 groundNormal{0.f};

    /// World-space velocity of whatever is underfoot, zero for static ground.
    /// Already folded into @ref velocity; exposed separately so gameplay can
    /// tell "moving because the floor moved" from "moving because I walked".
    AFIELD() glm::vec3 groundVelocity{0.f};

    /// The entity underfoot, or NullEntity when there is none or its body has
    /// no entity. Check it is still alive before acting on it — the body may
    /// have been destroyed since the step that recorded it.
    AFIELD() ECS::Entity groundEntity{ECS::NullEntity};

    /// Seconds since the character was last @ref GroundState::OnGround, zero
    /// while it still is. What an animation graph filters a one-frame stair
    /// flicker with.
    AFIELD() float timeSinceGrounded = 0.f;

    /// Height of the eye above the feet right now (m). Eases between the
    /// Character's two eye heights as the stance changes, and jumps by the
    /// difference between the two capsule heights when a stance change in the
    /// air moves the feet, so the eye itself stays where it was.
    AFIELD() float eyeHeight = 0.f;

    AFIELD() GroundState ground = GroundState::InAir;

    /// The stance the character is **actually** in, which is not always the one
    /// it was asked for: standing up under a low ceiling leaves it crouched.
    AFIELD() Stance stance = Stance::Standing;

    /// Whether a jump asked for now would fire.
    ///
    /// Deliberately not `ground == OnGround`. Within the Character's coyote
    /// time a character that has already left the ground may still jump, and one
    /// that has jumped and not yet landed may not even if it is momentarily
    /// touching something. Gameplay and animation should ask this; @ref ground
    /// stays the simulation's plain answer about what is underfoot.
    AFIELD() bool canJump = false;
};

/// @brief What happens to a character's speed when it jumps on the step it
/// lands.
///
/// Friction is a ground force and a jumping character is not on the ground, so
/// a jump timed to the landing step keeps all the speed gained in the air. This
/// says how much of it survives the take-off.
AENUM()
enum class BunnyHopPolicy : std::uint8_t
{
    Allow,    ///< Nothing is taken away; chained hops keep building speed.
    Cap,      ///< Take-off speed is limited to bunnyHopSpeedCap times walkSpeed.
    Disallow, ///< Take-off speed is limited to walkSpeed, so hopping gains nothing.

    /// Half-Life 2's rule. A jump adds speed along the way the character faces
    /// when it is asking to go forward, and takes away whatever that leaves over
    /// the limit — also along the way it faces, not the way it is moving. A
    /// character travelling backwards over the limit is therefore pushed faster
    /// by every jump, which is the accelerated back hop.
    Boost,
    Count,
};

/// @brief A character: the capsule a player or an NPC is, and how it moves.
///
/// Every field here is something a designer tunes. The solver tolerances that
/// only a physics programmer would touch are named constants inside the
/// physics module instead, so this struct stays a list of decisions about feel
/// rather than a wall of numbers.
///
/// A character is always on CollisionChannel::Character — there is no channel
/// field, because a character on any other channel would be a rigid body with
/// extra steps, and one on Trigger could not be a sensor and a solid volume at
/// once.
///
/// No Collider or RigidBody beside it: a character already owns a body (its
/// inner capsule), and a second one would collide with its own character. No
/// Parent either: the simulation owns its pose.
///
/// **The entity's Transform sits at the character's feet**, not at the middle
/// of the capsule, so an author places a character by standing it on the floor.
ACOMP(replicable, requires = {Transform, CharacterIntent, CharacterState}, excludes = {Parent, RigidBody, Collider})
struct Character
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

    /// Height of the eye above the feet while standing (m). A camera parented
    /// to the character is kept at the character's eye height, which rests here.
    AFIELD(min = 0.0) float eyeHeight = 1.5f;

    /// @ref eyeHeight while crouching. The default is lower by exactly the
    /// difference between the two capsule heights, which is what makes a crouch
    /// in the air leave the view where it was while the feet come up.
    AFIELD(min = 0.0) float crouchEyeHeight = 0.9f;

    /// How fast the eye moves between the two heights (m/s). The default covers
    /// the distance in a fifth of a second.
    AFIELD(min = 0.0) float eyeSpeed = 3.f;

    /// The speed the character asks for with a full `move` (m/s). Not a hard
    /// limit: speed gained in the air or carried off a moving platform is kept
    /// until friction takes it. The default is Half-Life 2's normal run, 190
    /// units a second at 1.905 cm to the unit — every movement default below is
    /// that game's value at that scale.
    AFIELD(min = 0.0) float walkSpeed = 3.62f;

    /// @ref walkSpeed multiplier while crouching. A scale rather than a second
    /// speed so retuning the walk carries the crouch with it.
    AFIELD(min = 0.0) float crouchSpeedScale = 0.333f;

    /// Upward speed a jump starts with (m/s). With the default
    /// @ref gravityScale it rises about 0.4 m.
    AFIELD(min = 0.0) float jumpSpeed = 3.05f;

    /// How strongly the ground slows the character, as the fraction of its
    /// speed lost per second. Applied every step it stands on walkable ground,
    /// before acceleration, whether or not it is asking to move — the short
    /// slide when changing direction is this losing to the old velocity.
    AFIELD(min = 0.0) float friction = 4.f;

    /// Below this speed friction acts as though the character were moving this
    /// fast (m/s). Without it the loss shrinks with the speed and a stop creeps
    /// toward zero forever; with it the last of the speed goes at a constant
    /// rate and the stop is firm.
    AFIELD(min = 0.0) float stopSpeed = 1.905f;

    /// How fast speed is gained along the requested direction on the ground, as
    /// a fraction of the requested speed per second: at 10, a tenth of a second
    /// of gain reaches it. Only the speed *along* that direction is limited to
    /// the requested speed; whatever the character has sideways is left to
    /// friction.
    AFIELD(min = 0.0) float groundAcceleration = 10.f;

    /// The same while airborne. Zero keeps whatever horizontal velocity the
    /// jump launched with and ignores steering entirely.
    AFIELD(min = 0.0) float airAcceleration = 10.f;

    /// In the air, the requested speed is cut down to this before deciding how
    /// much may be added (m/s), while the gain per second still uses the full
    /// requested speed. Holding a direction therefore adds almost nothing, and
    /// turning the request away from the current velocity keeps adding — which
    /// is what lets a strafing jump curve and gain speed.
    AFIELD(min = 0.0) float airWishSpeedCap = 0.5715f;

    /// With BunnyHopPolicy::Cap, the most horizontal speed a jump may leave the
    /// ground with, as a multiple of @ref walkSpeed.
    AFIELD(min = 0.0) float bunnyHopSpeedCap = 1.7f;

    /// Multiplies the world's gravity for this character alone. Above 1 falls
    /// heavier and jumps shorter; the arc is what most of a character's weight
    /// reads as.
    AFIELD(min = 0.0) float gravityScale = 1.165f;

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

    /// What a jump on the landing step does with speed gained in the air.
    AFIELD() BunnyHopPolicy bunnyHop = BunnyHopPolicy::Cap;

    /// Whether this character can shove other bodies at all. Distinct from
    /// @ref pushStrength being zero only in intent; both are honoured.
    AFIELD() bool canPushBodies = true;

    /// Whether other bodies can shove this character. False makes it immovable
    /// by anything but its own movement — what an NPC that must hold its mark
    /// wants.
    AFIELD() bool canBePushed = true;
};

/// @brief What a character is being asked to do.
///
/// Every Character brings one. The fields are the whole input surface:
/// whatever fills them — a keyboard, an AI, a replicated command — the
/// character behaves the same, which is what lets one controller serve players
/// and NPCs. The physics world reads them every fixed step, and clears @ref jump
/// once it is consumed.
ACOMP(transient)
struct CharacterIntent
{
    /// Where the character is trying to go, in world space. A unit direction
    /// asks for the Character's walk speed (scaled while crouching), and a
    /// shorter one for proportionally less. The vertical component is ignored:
    /// going up is @ref jump's business and going down is gravity's.
    AFIELD() glm::vec3 move{0.f};

    /// The stance being asked for, held for as long as it is wanted rather than
    /// pulsed. Asking to stand under something too low fails silently and is
    /// retried every step, so a character stands back up by itself once it walks
    /// clear; CharacterState::stance is what it actually managed.
    AFIELD() Stance stance = Stance::Standing;

    /// A request, not a state: set it to ask for a jump, and the step clears it
    /// once it has been taken.
    ///
    /// Remembered briefly rather than only on the step it arrives, so a jump
    /// asked for a few milliseconds before landing still fires on the landing
    /// step instead of being swallowed. Set again every step, the character
    /// jumps again as soon as it may.
    AFIELD() bool jump = false;
};

} // namespace Assisi::Physics