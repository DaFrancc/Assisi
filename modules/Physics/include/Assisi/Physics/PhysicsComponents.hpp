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

/// @brief Which shape a Collider builds: a primitive, or the collision a model
/// carries.
///
/// AENUM so it reflects as a dropdown and serializes by value. Each shape reads
/// a different subset of Collider's fields, which use AFIELD(radio) so the
/// inspector only shows the ones the chosen shape actually uses.
AENUM()
enum class ColliderShape : std::uint8_t
{
    Box,      ///< Axis-aligned box from `halfExtents`.
    Sphere,   ///< Sphere from `radius`.
    Capsule,  ///< Capsule (cylinder + hemispherical caps) from `radius` + `halfHeight`.
    Cylinder, ///< Cylinder from `radius` + `halfHeight`.

    /// The pieces `collisionAsset` authored, or the convex hull of the whole
    /// model when it authored none. Works on any body.
    Convex,

    /// The model's exact triangles. Static and kinematic bodies only: on a
    /// dynamic one it is an error, and the body takes Convex instead.
    Mesh,
    Count_,
};

/// @brief What kind of thing a body is, for deciding what it interacts with.
///
/// A body is on exactly one channel and carries a mask of the channels it
/// collides with; two bodies interact only if each one's mask includes the
/// other's channel. The same pair applies to a query, which is a participant
/// like any other — a line-of-sight ray is something on @ref Visibility, and a
/// surface opts out of being seen by dropping that channel from its own mask.
///
/// The enumerators are the engine's own channels. The slots between the last of
/// them and `Count_` belong to the game, which takes them with GameChannel and
/// names them in CollisionChannelNames.hpp.
///
/// An enumerator's value is its bit position in every mask stored in a level
/// file, so each is spelled out: renumbering one silently re-aims every mask
/// authored under the old numbering.
AENUM()
enum class CollisionChannel : std::uint8_t
{
    World = 0,      ///< Ordinary physical matter: floors, walls, props, crates.
    Character = 1,  ///< Player and NPC bodies.
    Trigger = 2,    ///< A volume that detects what enters it and blocks nothing.
    Visibility = 3, ///< Queries only: line of sight, editor picking.
    Camera = 4,     ///< Queries only: camera collision.

    /// The parts of a character a shot can hit. A convention, not a rule: the
    /// engine treats it like any other channel.
    Hitbox = 5,

    /// One past the last slot: a mask has a bit for every channel, the game's
    /// included.
    Count_ = 32,
};

static_assert(static_cast<std::uint32_t>(CollisionChannel::Count_) == Core::kBitmaskWidth<std::uint32_t>,
              "Every bit of a channel mask is a channel, and every channel has a bit.");

/// The first slot after the engine's own channels.
inline constexpr std::uint32_t kFirstGameChannel = static_cast<std::uint32_t>(CollisionChannel::Hitbox) + 1u;

/// How many channels a game may take.
inline constexpr std::uint32_t kGameChannelCount =
    static_cast<std::uint32_t>(CollisionChannel::Count_) - kFirstGameChannel;

/// Deliberately not constexpr: GameChannel calls it only for an index out of
/// range, and calling it while evaluating a consteval function is what turns that
/// index into a build error naming this function.
void GameChannelIndexIsOutOfRange();

/// @brief The game's channel @p index, from 0 to kGameChannelCount - 1.
///
/// consteval, so an index out of range is a build error rather than a channel
/// that aliases another.
consteval CollisionChannel GameChannel(std::uint32_t index)
{
    if (index >= kGameChannelCount)
    {
        GameChannelIndexIsOutOfRange();
    }
    return static_cast<CollisionChannel>(kFirstGameChannel + index);
}

/// The density of water, in kg/m³: what a collider weighs unless it says
/// otherwise.
inline constexpr float kWaterDensity = 1000.f;

/// Collider::collisionPiece naming every piece of the model.
inline constexpr std::int32_t kAllCollisionPieces = -1;

/// @brief What a Collider under a RigidBody becomes.
AENUM()
enum class ColliderAttach : std::uint8_t
{
    /// Part of the RigidBody's own shape: it turns with the body and adds to
    /// its mass.
    Piece,

    /// A body of its own that rides along at its entity's pose, adding no mass.
    Body,
    Count_,
};

/// @brief Every channel — the mask a body carries unless it narrows it, so a
/// body defaults to interacting with everything and an author subtracts rather
/// than having to enumerate.
inline constexpr Core::Bitmask<CollisionChannel, std::uint32_t> AllChannels = Core::Bitmask<CollisionChannel, std::uint32_t>::All();

/// @brief Which way a RigidBody moves.
///
/// There is no static: an immovable thing is a Collider with no RigidBody.
AENUM()
enum class MotionType : std::uint8_t
{
    Dynamic,   ///< Moved by the simulation: gravity, collisions, velocity.
    Kinematic, ///< Moved only by its Transform; pushes dynamic bodies and is pushed by nothing.
    Count_,
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
    Count_,
};

/// @brief A collision shape, and what touching it is like.
///
/// On its own it is static geometry: a wall, a floor, a trigger volume that
/// never moves. Add a RigidBody to make it move. What one on a child entity is
/// depends on what is above it; see ColliderRole. The shape is built at the
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

    /// The model whose collision a Convex or Mesh shape is. One cooked shape is
    /// shared by every collider that names the same model.
    AFIELD(radioListen = {source = shape, value = {Convex, Mesh}, behavior = vanish})
    Core::AssetId collisionAsset;

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

    /// How much speed an impact gives back: 0 stops dead, 1 loses nothing, and
    /// above 1 the body gains speed on every bounce. Two touching bodies use the
    /// larger of theirs. An impact slower than 1 m/s does not bounce at all, so
    /// a body at rest stays at rest whatever this is.
    AFIELD(min = 0.0) float restitution = 0.f;

    /// kg/m³. With its volume, how much this collider adds to its body's mass,
    /// and where the body's centre of mass falls.
    AFIELD(min = 0.0) float density = kWaterDensity;

    /// One piece of a Convex model, by its place in the model's list of pieces,
    /// or -1 for all of them. One piece is built at the collider's own origin
    /// rather than where the model put it, so a child entity standing where the
    /// piece was is that piece, its shape still shared.
    AFIELD(min = -1, radioListen = {source = shape, value = Convex, behavior = vanish})
    int32_t collisionPiece = kAllCollisionPieces;

    /// The channels this collider collides with. Interaction needs both sides
    /// to agree, so clearing a bit here stops the pair whatever the other says.
    AFIELD() Core::Bitmask<CollisionChannel, std::uint32_t> collidesWith = AllChannels;

    /// @brief Which channel this collider is on.
    ///
    /// `Trigger` is not just a label: it makes the collider a sensor, detected
    /// by what enters it and blocking nothing. That is why there is no
    /// separate "is trigger" flag — a solid body on the trigger channel, or a
    /// pass-through body on any other, cannot be expressed and could only be a
    /// mistake.
    AFIELD() CollisionChannel channel = CollisionChannel::World;

    AFIELD(radioBroadcast) ColliderShape shape = ColliderShape::Box; ///< Collision primitive to build.

    /// Under a RigidBody, whether this is part of the body or rides along as a
    /// body of its own. A collider on the Trigger channel always rides along:
    /// Jolt makes a whole body a sensor or none of it.
    AFIELD() ColliderAttach attach = ColliderAttach::Piece;

    /// False takes the collider out of the simulation without removing it: no
    /// body, no piece, never hit. For a hitbox live only during an attack.
    AFIELD() bool enabled = true;
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
    /// Kilograms. 0 adds up every collider of the body, each by its volume and
    /// density; anything else scales that total to this.
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
    AFIELD() Core::Bitmask<LockedAxis, std::uint8_t> lockedAxes;

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

// ---------------------------------------------------------------------------
// Joints
// ---------------------------------------------------------------------------
//
// A joint attaches the body of the entity carrying it to another body, or to
// the world, and comes in one component per kind. Whichever of the two bodies
// carries it, it holds both ways. The two need not touch: each is held where
// it was relative to the other when the joint was built, and bodies joined
// directly do not collide with each other unless `collideConnected` says so.
//
// The entity must have a body of its own: a Collider and a Transform, with or
// without a RigidBody, and no Parent. Anchors and axes are in its own axes,
// scaled with it as a Collider's offset is. A joint is built once both bodies
// exist, and is gone when either body or the component goes.
//
// `breakForce` and `breakTorque` break the joint when one step pulls or twists
// it harder; 0 never breaks. A broken joint is reported by
// PhysicsWorld::BrokenJoints and its component removed.

/// @brief Holds two bodies in the pose they were built in, as if welded.
ACOMP(replicable, requires = {Transform}, excludes = {Character})
struct FixedJoint
{
    AFIELD() glm::vec3 anchor{0.f}; ///< Where the weld is drawn. It holds the two as they stand wherever it is.
    AFIELD() ECS::Entity other{ECS::NullEntity}; ///< The other body, or nothing for the world.
    AFIELD(min = 0.0) float breakForce = 0.f; ///< N.
    AFIELD(min = 0.0) float breakTorque = 0.f; ///< N·m.
    AFIELD() bool collideConnected = false;
};

/// @brief Lets two bodies turn freely about a shared point: a ball and socket,
/// or a link of a chain.
ACOMP(replicable, requires = {Transform}, excludes = {Character})
struct PointJoint
{
    AFIELD() glm::vec3 anchor{0.f}; ///< The pivot.
    AFIELD() ECS::Entity other{ECS::NullEntity}; ///< The other body, or nothing for the world.
    AFIELD(min = 0.0) float breakForce = 0.f; ///< N.
    AFIELD() bool collideConnected = false;
};

/// @brief Lets two bodies turn about one axis through a shared point: a door,
/// a wheel, a lever.
ACOMP(replicable, requires = {Transform}, excludes = {Character})
struct HingeJoint
{
    AFIELD() glm::vec3 anchor{0.f}; ///< A point on the axle.
    AFIELD() glm::vec3 axis{0.f, 1.f, 0.f}; ///< The axle's direction.
    AFIELD() ECS::Entity other{ECS::NullEntity}; ///< The other body, or nothing for the world.

    /// Degrees either way from the angle it was built at. -180 to 180 is free.
    AFIELD(min = -180.0, max = 0.0) float minAngle = -180.f;
    AFIELD(min = 0.0, max = 180.0) float maxAngle = 180.f;

    /// Hz. 0 stops dead at a limit; above 0 a limit gives like a spring of
    /// this frequency and pulls back. Higher is stiffer.
    AFIELD(min = 0.0) float limitSpringFrequency = 0.f;

    /// How quickly a soft limit stops bouncing: 0 never, 1 without overshooting.
    AFIELD(min = 0.0) float limitSpringDamping = 1.f;

    /// N·m resisting the turn while no HingeMotor drives it. A door with
    /// friction stays where it is pushed.
    AFIELD(min = 0.0) float friction = 0.f;

    AFIELD(min = 0.0) float breakForce = 0.f; ///< N.
    AFIELD(min = 0.0) float breakTorque = 0.f; ///< N·m.
    AFIELD() bool collideConnected = false;
};

/// @brief Lets two bodies slide along one axis without turning: a drawer, a
/// piston, a lift on a rail.
ACOMP(replicable, requires = {Transform}, excludes = {Character})
struct SliderJoint
{
    AFIELD() glm::vec3 anchor{0.f};
    AFIELD() glm::vec3 axis{0.f, 1.f, 0.f}; ///< The rail's direction.
    AFIELD() ECS::Entity other{ECS::NullEntity}; ///< The other body, or nothing for the world.

    /// Metres along the axis from where it was built.
    AFIELD() float minDistance = -1.f;
    AFIELD() float maxDistance = 1.f;

    /// Hz. 0 stops dead at a limit; above 0 a limit gives like a spring.
    AFIELD(min = 0.0) float limitSpringFrequency = 0.f;

    /// How quickly a soft limit stops bouncing: 0 never, 1 without overshooting.
    AFIELD(min = 0.0) float limitSpringDamping = 1.f;

    /// N resisting the slide while no SliderMotor drives it.
    AFIELD(min = 0.0) float friction = 0.f;

    AFIELD(min = 0.0) float breakForce = 0.f; ///< N.
    AFIELD(min = 0.0) float breakTorque = 0.f; ///< N·m.
    AFIELD() bool collideConnected = false;
};

/// @brief Keeps a point on each body within a range of each other: a rope
/// when the range is wide, a rod when its ends are equal.
ACOMP(replicable, requires = {Transform}, excludes = {Character})
struct DistanceJoint
{
    AFIELD() glm::vec3 anchor{0.f}; ///< This body's end.

    /// The other end, in this entity's space when the joint is built: a point
    /// on the other body, or in the world when there is none.
    AFIELD() glm::vec3 otherAnchor{0.f};

    AFIELD() ECS::Entity other{ECS::NullEntity}; ///< The other body, or nothing for the world.

    /// Metres between the two ends.
    AFIELD(min = 0.0) float minDistance = 0.f;
    AFIELD(min = 0.0) float maxDistance = 1.f;

    /// Hz. 0 stops dead at a limit; above 0 a limit stretches like a spring.
    AFIELD(min = 0.0) float limitSpringFrequency = 0.f;

    /// How quickly a soft limit stops bouncing: 0 never, 1 without overshooting.
    AFIELD(min = 0.0) float limitSpringDamping = 1.f;

    AFIELD(min = 0.0) float breakForce = 0.f; ///< N.
    AFIELD() bool collideConnected = false;
};

/// @brief Lets two bodies tilt their axis within a cone and twist about it: a
/// shoulder, a hip, a neck.
ACOMP(replicable, requires = {Transform}, excludes = {Character})
struct SwingTwistJoint
{
    AFIELD() glm::vec3 anchor{0.f}; ///< The pivot.
    AFIELD() glm::vec3 axis{0.f, 1.f, 0.f}; ///< The axis that tilts and twists, like an upper arm.
    AFIELD() ECS::Entity other{ECS::NullEntity}; ///< The other body, or nothing for the world.

    /// Degrees the axis may tilt from where it was built, any way.
    AFIELD(min = 0.0, max = 180.0) float swingAngle = 45.f;

    /// Degrees of twist about the axis either way from where it was built.
    AFIELD(min = -180.0, max = 0.0) float minTwist = -45.f;
    AFIELD(min = 0.0, max = 180.0) float maxTwist = 45.f;

    /// N·m resisting any turn. What makes a ragdoll's limbs stiff rather
    /// than floppy.
    AFIELD(min = 0.0) float friction = 0.f;

    AFIELD(min = 0.0) float breakForce = 0.f; ///< N.
    AFIELD(min = 0.0) float breakTorque = 0.f; ///< N·m.
    AFIELD() bool collideConnected = false;
};

/// @brief How a motor drives its joint.
AENUM()
enum class MotorMode : std::uint8_t
{
    Velocity, ///< At `target` per second, for as long as it has the strength.
    Position, ///< To `target`, springing there and holding.
    Count_,
};

/// @brief Turns a HingeJoint by itself: a powered door, a drawbridge, a fan.
ACOMP(replicable, requires = {HingeJoint})
struct HingeMotor
{
    /// Degrees per second, or degrees from the angle the hinge was built at.
    AFIELD() float target = 0.f;

    /// The most torque it turns with (N·m). 0 is no limit.
    AFIELD(min = 0.0) float maxTorque = 0.f;

    AFIELD() MotorMode mode = MotorMode::Velocity;
};

/// @brief Slides a SliderJoint by itself: a lift, a piston, a sliding door.
ACOMP(replicable, requires = {SliderJoint})
struct SliderMotor
{
    /// Metres per second, or metres from where the slider was built.
    AFIELD() float target = 0.f;

    /// The most force it pushes with (N). 0 is no limit.
    AFIELD(min = 0.0) float maxForce = 0.f;

    AFIELD() MotorMode mode = MotorMode::Velocity;
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
    Count_,
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
    Count_,
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
    Count_,
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
    AFIELD() Core::Bitmask<CollisionChannel, std::uint32_t> collidesWith = AllChannels;

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