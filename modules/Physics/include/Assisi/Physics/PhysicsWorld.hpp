/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PhysicsWorld.hpp
/// @brief Jolt physics simulation wrapper.
///
/// One PhysicsWorld per scene, bound to it for life. The scene's components are
/// the whole interface: an entity with a Collider or a Character and a
/// Transform has a body, an edit to any of its physics components is applied to
/// it, and a write to its Transform moves it. Reconcile() is what notices, and
/// Update() runs it before every step, so nothing outside this module creates,
/// moves or destroys a body.
///
/// Transform is the simulation pose: Update() ends by writing each moving
/// body's pose into it, with its BodyState or CharacterState beside it. Call
/// Update() inside an ECS::FixedStepScope, so the ECS draws those Transforms
/// blended between steps.

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/CollisionSource.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Assisi::Physics
{

/// @brief Motion type for newly created bodies.
enum class BodyMotion : std::uint8_t
{
    Static,    ///< Immovable; collides but is never moved by the simulation.
    Dynamic,   ///< Fully simulated; affected by gravity and collisions.
    Kinematic, ///< Moved only by what sets its pose; pushes dynamic bodies and is pushed by nothing.
    Count,
};

/// @brief A position and orientation in world space.
///
/// Deliberately not ECS::Transform, which is a *local* pose — one under a parent
/// is an offset from that parent. It also carries a scale and a cached world
/// matrix that mean nothing to a body whose size comes from its collider.
struct Pose
{
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    glm::vec3 position{0.f};
};

/// @brief What a participant is, and what it interacts with.
///
/// Carried by bodies and by queries alike, because a query is a participant: a
/// cast decides what it may hit by the same two-way rule a collision does.
struct CollisionFilter
{
    Core::Bitmask<CollisionChannel> collidesWith = AllChannels;

    CollisionChannel channel = CollisionChannel::World;

    /// @brief The same filter with @p channel removed from its mask.
    ///
    /// What a movement query wants: a character sweeping its capsule forward
    /// reuses its body's own filter minus Trigger, because a sensor reports the
    /// character but never stops it, and a cast that stopped at one would have
    /// the character walk into an invisible wall.
    [[nodiscard]] CollisionFilter Without(CollisionChannel excluded) const
    {
        return CollisionFilter{collidesWith.Without(excluded), channel};
    }
};

/// @brief Where a query met a body.
///
/// One shape for every kind of query: what a caller does with a hit — place a
/// cursor, stand a character on it, decide a shot connected — reads the same
/// fields whether a ray, a swept volume or a held one found it.
struct QueryHit
{
    glm::vec3 position{0.f}; ///< World-space point on the struck surface.

    /// Unit surface normal at @ref position, pointing out of the surface and so
    /// back towards the caster. Reflecting a direction about it, or comparing it
    /// against up to decide whether a surface is walkable, both work without the
    /// caller knowing which query produced the hit.
    glm::vec3 normal{0.f};

    /// The entity whose body was struck: the owner, for a piece or a follower.
    /// The handle is the one the body was built for, so check it is still alive
    /// before acting on it.
    ECS::Entity entity{ECS::NullEntity};

    /// The entity whose Collider was struck: a piece or a follower of
    /// @ref entity, or @ref entity itself.
    ECS::Entity piece{ECS::NullEntity};

    /// For a cast, the distance from its origin to @ref position along it: zero
    /// when it began already overlapping, which is a hit, not a miss. For an
    /// overlap, how deep the shapes overlap.
    float distance = 0.f;
};

/// @brief Where a pair of bodies is in the life of their contact.
enum class ContactPhase : std::uint8_t
{
    Enter, ///< They were not touching last step and are now.
    Stay,  ///< They were touching last step and still are. Reported only when asked for.
    Exit,  ///< They were touching last step and are not now.
    Count,
};

/// @brief One side of one body pair's contact during the most recent Update().
///
/// Reported per participant rather than per pair: two bodies touching produce
/// two events, one from each point of view, so a consumer never has to work out
/// which end of the pair it is looking at.
///
/// At most one event per pair per participant per step, whatever the pair's
/// contact geometry did: several manifolds and several collision substeps
/// collapse into one.
struct ContactEvent
{
    /// Unit world-space normal pointing away from @ref other's surface — so a
    /// body arriving at a floor sees +Y here, whichever way the pair was ordered.
    glm::vec3 normal{0.f};

    /// Where the two touch, in world space: the middle of the contact the step
    /// last saw. On an Exit, where they last touched.
    glm::vec3 point{0.f};

    /// @ref entity's linear velocity when the contact was observed, which is
    /// **before the solver ran**. This is the field that makes the record worth
    /// keeping: by the time a system sees it, the step has already absorbed the
    /// impact and the body's live velocity is whatever Jolt left it with. Zero
    /// for a static body. On an Exit, and on a Stay for a pair whose bodies are
    /// both asleep, this is the last value observed rather than a fresh one —
    /// nothing measured it this step.
    glm::vec3 velocity{0.f};

    ECS::Entity entity{ECS::NullEntity}; ///< The entity this record speaks for.
    ECS::Entity other{ECS::NullEntity};  ///< What it touched; NullEntity if that body has no entity.

    /// The entities whose Colliders touched, on @ref entity's side and on
    /// @ref other's: a piece or a follower of each, or the entity itself. Of
    /// several touching parts, the most deeply overlapping.
    ECS::Entity piece{ECS::NullEntity};
    ECS::Entity otherPiece{ECS::NullEntity};

    ContactPhase phase = ContactPhase::Enter;

    /// True when either body is a sensor, which is to say on the Trigger channel.
    /// A response that pushes back — a bounce, a damage impulse — wants only the
    /// contacts that actually resisted something, and a sensor resisted nothing.
    bool sensor = false;
};

/// @brief Which joint component a joint is.
enum class JointKind : std::uint8_t
{
    Fixed,      ///< FixedJoint
    Point,      ///< PointJoint
    Hinge,      ///< HingeJoint
    Slider,     ///< SliderJoint
    Distance,   ///< DistanceJoint
    SwingTwist, ///< SwingTwistJoint
    Count,
};

/// @brief A joint that broke during the most recent Update().
///
/// By the time a system reads this the joint's component, and its motor's, are
/// already gone from @ref owner, and the two bodies are free of each other.
struct JointBroke
{
    ECS::Entity owner{ECS::NullEntity}; ///< The entity that carried the joint.
    ECS::Entity other{ECS::NullEntity}; ///< The body it was attached to; NullEntity for the world.
    float force = 0.f;                  ///< The pull it bore in the step it broke (N).
    float torque = 0.f;                 ///< The twist it bore in the step it broke (N·m).
    JointKind kind = JointKind::Fixed;
};

/// @brief The most bodies a PhysicsWorld holds unless told otherwise. Jolt
/// reserves room for all of them up front, a pointer apiece.
inline constexpr uint32_t kDefaultMaxBodies = 65536;

/// @brief Wraps a Jolt PhysicsSystem and keeps it in step with one scene.
///
/// Construction brings the shared Jolt runtime up if this is the first world.
/// Destruction destroys every body and character this world built.
class PhysicsWorld
{
public:
    /// @param scene      The scene whose bodies this world simulates. Must
    ///                   outlive the world.
    /// @param collision  Where a Convex or Mesh collider's model is read from.
    ///                   Must outlive the world.
    /// @param maxBodies  The most bodies this world can hold, characters'
    ///                   included. A body past it is not built, and the entity
    ///                   is named in the log.
    PhysicsWorld(ECS::Scene &scene, const CollisionSource &collision, uint32_t maxBodies = kDefaultMaxBodies);
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld &) = delete;
    PhysicsWorld &operator=(const PhysicsWorld &) = delete;

    /// @brief Brings the simulation into line with the scene.
    ///
    /// Builds a body or character for every entity that gained a Collider or a
    /// Character, applies edits to Collider, RigidBody and Character in place,
    /// destroys what lost them or its Transform or was destroyed, pushes every
    /// BodyState velocity written by anything but this world to the body, and
    /// pushes every Transform written by anything but this world:
    ///
    ///   - a static body is placed;
    ///   - a kinematic body is swept there over the step Update() is about to
    ///     run, so it pushes what it meets; called on its own it is placed;
    ///   - a dynamic body is placed and keeps its velocity;
    ///   - a character is placed when its position changed. Its rotation is the
    ///     way it faces, which the controller does not use.
    ///
    /// A changed scale rebuilds the shape at the new size, and a character whose
    /// CharacterIntent asks for another stance takes it, if it fits.
    ///
    /// Update() calls this first. Call it yourself when the scene changed and no
    /// step is coming: a loader that wants bodies in place before the world is
    /// handed over, or an editor that is not simulating but still casts rays.
    void Reconcile();

    /// @brief Drops every body and character and builds them again from the scene.
    ///
    /// For a scene that was replaced wholesale — a level load, or the editor
    /// putting back the scene it had before play — where nothing simulated
    /// should carry over: every body starts at its Transform, at rest, and every
    /// character with no momentum and its default stance.
    void Rebuild();

    /// @brief Whether @p entity has a body or a character of its own in this
    /// world. False for a piece or a follower, whose body is its owner's.
    ///
    /// Every other call that takes an entity — the requests, the reads, the
    /// contact queries, a query's `ignore` — answers for or acts on the owner
    /// when given a piece or a follower.
    [[nodiscard]] bool HasBody(ECS::Entity entity) const;

    /// @brief Moves @p entity's body or character to a world-space @p pose and
    /// stops it, writing its Transform to match.
    ///
    /// Unlike a Transform write, which keeps a dynamic body's velocity, this
    /// zeroes it, and the render blend does not slide the body across the gap.
    void Teleport(ECS::Entity entity, const Pose &pose);

    // --- Requests --------------------------------------------------------------
    //
    // Pushes and sleep changes, queued in the order they are made and applied
    // at the start of the next Update(), after its reconcile: an entity spawned
    // this frame has its body by then, and a Transform written before the
    // request has already moved it. Reconcile() on its own applies none of them.
    // An entity with no body by then is skipped.
    //
    // A force acts over the one step it is applied to, so a steady push is one
    // AddForce every fixed step; an impulse is a single kick. Both change a
    // dynamic body's velocity by their amount over its mass, and wake it. A
    // character takes forces and impulses too, as a change to its velocity that
    // its movement rules then carry on from — a knockback, a wind pushing it.
    // Nothing else moves: a static body, a kinematic body, and everything a
    // character cannot do — spin, sleep, be pushed off its centre — ignore them.

    /// @brief Pushes @p entity through its centre of mass by @p force (N) for
    /// the next step.
    void AddForce(ECS::Entity entity, glm::vec3 force);

    /// @brief Pushes @p entity by @p force (N) at a world-space @p point for the
    /// next step, which spins it as well when the point is off its centre.
    void AddForceAt(ECS::Entity entity, glm::vec3 force, glm::vec3 point);

    /// @brief Kicks @p entity through its centre of mass by @p impulse (N·s).
    void AddImpulse(ECS::Entity entity, glm::vec3 impulse);

    /// @brief Kicks @p entity by @p impulse (N·s) at a world-space @p point.
    void AddImpulseAt(ECS::Entity entity, glm::vec3 impulse, glm::vec3 point);

    /// @brief Twists @p entity by @p torque (N·m), about world axes, for the
    /// next step.
    void AddTorque(ECS::Entity entity, glm::vec3 torque);

    /// @brief Spins @p entity by @p angularImpulse (N·m·s) about world axes.
    void AddAngularImpulse(ECS::Entity entity, glm::vec3 angularImpulse);

    /// @brief Wakes @p entity's body, so it is simulated from the next step.
    void Wake(ECS::Entity entity);

    /// @brief Puts @p entity's body to sleep where it is, at rest. It wakes
    /// again as soon as something touches it or pushes it.
    void Sleep(ECS::Entity entity);

    /// @brief Reconciles, advances the simulation by `deltaTime` seconds, and
    /// writes the result into the Transforms of the bodies that moved.
    ///
    /// Each character reads its CharacterIntent, then the characters are swept,
    /// then the bodies are solved. That order is
    /// what lets a character shove a crate and the crate move in the same step,
    /// and what lets it ride a platform whose velocity was set before this call.
    ///
    /// A body that fell asleep is written once more, at rest, and then costs
    /// nothing until something wakes it. A character's rotation is never
    /// written: the capsule is symmetric about its up axis, so the way it faces
    /// belongs to whatever aims it.
    void Update(float deltaTime);

    // --- Contact events ------------------------------------------------------
    //
    // Enter and Exit are always on. Trigger volumes are authored data, and a
    // switch a level had to remember to flip would make one silently do nothing.
    // Stay is off unless asked for: it repeats every step for every resting
    // pair, and what it would say is a question IsTouching answers.
    //
    // Jolt's own contact callbacks cannot be the source of these. Its
    // "contact removed" callback fires when a body falls *asleep*, and forbids
    // touching either body because one may already have been destroyed — so a
    // design that trusted it would report a motionless character as having left
    // the volume it is standing in. Instead the callbacks only record which pairs
    // touched, and the phases are derived after the step by comparing that
    // against the previous step's pairs.

    /// @brief Every contact event produced by the most recent Update().
    ///
    /// Cleared at the top of every Update(), so the span describes exactly one
    /// fixed step and a consumer cannot process the same event twice. Valid until
    /// the next Update() or Clear().
    ///
    /// Ordered by entity, then by what it touched, then by phase — the order Jolt
    /// discovers contacts in depends on how its jobs were scheduled, and a
    /// consumer that accumulated in that order would produce a different answer
    /// from one run to the next.
    ///
    /// A pair whose bodies are *both* asleep counts as still touching: a
    /// sleeping body has not moved, the simulation merely stopped testing it.
    /// With Stay reported it keeps producing Stay. A static body moved out from
    /// under a sleeping one wakes it, and the pair ends on the step after.
    ///
    /// A settled scene produces nothing here and costs nothing to resolve: a
    /// pair is looked at only when something about it may have changed.
    ///
    /// Characters appear here too, including against static floors — which their
    /// inner bodies alone could never report, a kinematic body resting on a
    /// static one generating no contact at all. The character's own sweep is
    /// what sees those, so a character's contacts are found a different way from
    /// a body's and arrive looking identical.
    ///
    /// A **frozen** character reports nothing while it is frozen: it is not
    /// being swept, so nothing is measuring what it touches.
    [[nodiscard]] std::span<const ContactEvent> ContactEvents() const;

    /// @brief Whether ContactEvents() reports Stay: every touching pair, every
    /// step, from both sides. Off by default; per world.
    void SetStayEventsReported(bool reported);
    [[nodiscard]] bool StayEventsReported() const;

    /// @brief Whether the bodies of @p a and @p b are touching as of the most
    /// recent Update(), with the same meaning ContactEvents() gives touching.
    [[nodiscard]] bool IsTouching(ECS::Entity a, ECS::Entity b) const;

    /// @brief Every entity @p entity's body is touching as of the most recent
    /// Update(), into @p out, which is cleared first. In no particular order.
    void Touching(ECS::Entity entity, std::vector<ECS::Entity> &out) const;

    /// @brief Stops the bodies of @p a and @p b colliding with or reporting
    /// each other, or with @p ignore false, lets them again.
    ///
    /// The exception channels and masks cannot express: one projectile and the
    /// one character that fired it. A piece or a follower names its owner, so
    /// the exception covers every part of both. Kept until it is undone or
    /// either entity is destroyed, through any rebuild of their bodies.
    void IgnoreCollision(ECS::Entity a, ECS::Entity b, bool ignore = true);

    // --- Joints ----------------------------------------------------------------

    /// @brief Every joint the most recent Update() broke, in owner order.
    ///
    /// A joint breaks when the force or the torque holding it together in a
    /// step exceeds its breakForce or breakTorque; its component is removed
    /// from the owner in the same step. Cleared at the top of every Update(),
    /// like ContactEvents(). With several collision steps a joint is measured
    /// by the last of them.
    [[nodiscard]] std::span<const JointBroke> BrokenJoints() const;

    /// @brief Whether this world breaks joints itself. On by default.
    ///
    /// Off for a world that mirrors another's simulation, which loses a Joint
    /// when the authority removes it rather than deciding for itself.
    void SetBreaksJoints(bool breaks);
    [[nodiscard]] bool BreaksJoints() const;

    // --- World queries -------------------------------------------------------
    //
    // Each is filtered by the same two-way rule as a collision: it finds a body
    // only if its own mask includes that body's channel and the body's mask
    // includes its channel. A ray takes the filter of the thing asking; a shape
    // query takes a Collider and is that collider asking, so it finds what the
    // collider would touch there — its channel, its mask, its offset.
    //
    // The single-hit form gives the nearest hit; the All form fills a caller's
    // vector, cleared first, with one hit per piece struck, nearest first (an
    // overlap's deepest first), so a
    // query a frame allocates nothing once the vector has grown.
    //
    // A sweep is given as a displacement rather than a direction and a length,
    // which is the one spelling that cannot disagree with itself. Distances in
    // the result are along it.
    //
    // None of these may be called from inside a contact callback — Jolt asserts,
    // because the bodies are already locked.

    /// @brief The first body a ray meets, or nothing if it meets none.
    ///
    /// A ray starting inside a body reports that body at distance 0. That is
    /// what @p ignore is for: a character casting from its own centre passes
    /// itself and gets the first thing that is not itself.
    ///
    /// @param origin  Where the ray starts, in world space.
    /// @param sweep   Direction and length together; a zero sweep finds nothing.
    /// @param filter  What is asking, and what it may find.
    /// @param ignore  An entity to skip, or NullEntity to skip nothing.
    [[nodiscard]] std::optional<QueryHit> CastRay(glm::vec3 origin, glm::vec3 sweep, CollisionFilter filter,
                                                  ECS::Entity ignore) const;

    /// @brief Every body a ray meets. See CastRay and the All form above.
    void CastRayAll(glm::vec3 origin, glm::vec3 sweep, CollisionFilter filter, ECS::Entity ignore,
                    std::vector<QueryHit> &out) const;

    /// @brief The first body a swept collider meets, or nothing if it meets none.
    ///
    /// The character-controller query: "if this capsule moved by @p sweep, what
    /// would stop it?" A cast that begins already overlapping something reports
    /// it at distance 0 rather than missing.
    ///
    /// @param collider  The shape to sweep, at its own offset and unscaled, and
    ///                  the filter it asks with. A collision asset is not
    ///                  supported here yet and sweeps the primitive.
    /// @param start     Where the collider's entity would begin, in world space.
    /// @param sweep     Direction and length together; a zero sweep finds nothing.
    /// @param ignore    An entity to skip, or NullEntity to skip nothing.
    [[nodiscard]] std::optional<QueryHit> CastShape(const Collider &collider, const Pose &start, glm::vec3 sweep,
                                                    ECS::Entity ignore) const;

    /// @brief Every body a swept collider meets. See CastShape and the All
    /// form above.
    void CastShapeAll(const Collider &collider, const Pose &start, glm::vec3 sweep, ECS::Entity ignore,
                      std::vector<QueryHit> &out) const;

    /// @brief The body a collider held still at @p at overlaps most deeply, or
    /// nothing if it overlaps none.
    ///
    /// Answers the question a sensor cannot: what is inside this volume right
    /// now, static scenery included. A trigger reports bodies that move, because
    /// that is what a contact is; walls and floors never generate one, so asking
    /// once is the only way to hear about them.
    ///
    /// A hit's `distance` is how deep it overlaps, and its `normal` points out of
    /// the body struck.
    [[nodiscard]] std::optional<QueryHit> Overlap(const Collider &collider, const Pose &at, ECS::Entity ignore) const;

    /// @brief Every body a collider held still overlaps, deepest first. See
    /// Overlap and the All form above.
    void OverlapAll(const Collider &collider, const Pose &at, ECS::Entity ignore, std::vector<QueryHit> &out) const;

    /// @brief Number of collision substeps Jolt runs per Update() call.
    ///
    /// Splitting a step into more substeps shrinks how far a fast body can move
    /// before the solver sees a contact, which reduces — and at a high enough
    /// count effectively eliminates — the impact penetration that reads as a
    /// body sinking into a surface and popping back out. Cost is roughly linear:
    /// N substeps ≈ N× the collision work per step. Clamped to [1, 16].
    void SetCollisionSteps(int32_t steps);

    /// @brief Current collision-substep count (see SetCollisionSteps).
    int32_t GetCollisionSteps() const;

    // --- Facts ------------------------------------------------------------------

    /// @brief Every entity whose own body or character the simulation has
    /// awake. A follower's body is not listed; its owner is.
    ///
    /// The order Jolt returns active bodies in is unspecified, which is fine:
    /// every consumer of this re-sorts by its own identity.
    void ActiveBodies(std::vector<ECS::Entity> &out) const;

    /// @brief Whether the simulation currently considers @p entity's body awake.
    [[nodiscard]] bool IsBodyActive(ECS::Entity entity) const;

    /// @brief The entity whose body @p piece's collider answers for: @p piece
    /// itself when it has a body, its owner when it is a piece or a follower,
    /// NullEntity when there is none.
    [[nodiscard]] ECS::Entity BodyOf(ECS::Entity piece) const;

    /// @brief The mass the simulation gives @p entity's body (kg):
    /// RigidBody::mass when set, every piece's volume at its density otherwise,
    /// Character::mass for a character. Zero for a static or kinematic body,
    /// which nothing can push, and for an entity with no body.
    [[nodiscard]] float Mass(ECS::Entity entity) const;

    /// @brief Sets pose, velocities and sleep in one call — what a replication
    /// correction is.
    ///
    /// Deliberately not composed from a Transform write and a BodyState write: a
    /// correction must be able to leave a body asleep, and land in this step
    /// rather than the next reconcile. @p state's `asleep` is obeyed here, unlike
    /// a write to the component.
    ///
    /// For a character, only the position and the linear velocity are taken:
    /// it faces wherever gameplay turns it, and it never sleeps.
    ///
    /// Writes the Transform and snaps it, so the drawn pose jumps with the
    /// correction. The replication view offset relies on that: it hides the
    /// jump, and a blend sliding after it would show twice.
    ///
    /// No-op for an entity with no body. A static body is placed and nothing more.
    void ApplyCorrection(ECS::Entity entity, const Pose &pose, const BodyState &state);

    /// @brief The current world-space pose of @p entity's body, or the identity
    /// pose when it has none.
    [[nodiscard]] Pose GetBodyPose(ECS::Entity entity) const;

    /// @brief @p entity's velocities and sleep as the simulation holds them now.
    ///
    /// Read from the body rather than from the BodyState component, which is
    /// written only after a step. At rest for a static body or an entity with
    /// no body, so callers can display the result unconditionally; a character
    /// reports its velocity and is never asleep.
    [[nodiscard]] BodyState GetBodyState(ECS::Entity entity) const;

    /// @brief Whether a body's motion quality is currently LinearCast (CCD on).
    /// False for Discrete bodies, static bodies, or an entity with no body.
    [[nodiscard]] bool IsBodyCCDEnabled(ECS::Entity entity) const;

    /// @brief The scale @p entity's collider was built at, in its own axes.
    ///
    /// The Transform's world scale where the shape can take it. A sphere or a
    /// capsule has one radius, so it is built at one scale on every axis, and a
    /// cylinder at one across its round axes. One on every axis for an unscaled
    /// collider, a character, or an entity with no body.
    [[nodiscard]] glm::vec3 GetColliderScale(ECS::Entity entity) const;

    /// @brief What @p entity's body is, and what it interacts with.
    [[nodiscard]] CollisionFilter GetBodyCollisionFilter(ECS::Entity entity) const;

    // --- Collision models -----------------------------------------------------
    //
    // A Convex or Mesh collider's shape is built from its model the first time
    // any collider asks for it, and shared by every collider that asks for the
    // same one afterwards, whatever its scale. A model is read once per world.

    /// @brief The edges of the shape @p collider builds, as pairs of points in
    /// the collider's own space, before its scale and offset. Empty for a
    /// primitive, or a model that could not be built. For drawing it.
    void CollisionAssetEdges(const Collider &collider, std::vector<glm::vec3> &out) const;

    /// @brief How many shapes this world has built from models.
    [[nodiscard]] uint32_t CookedShapeCount() const;

    /// @brief Forgets every model and shape built from one, and builds every
    /// body again from the scene, reading the models afresh. For the editor,
    /// after a model's file changed; not for use while simulating, since it is
    /// a Rebuild().
    void InvalidateCollisionAssets();

    // --- Characters -----------------------------------------------------------
    //
    // A character is not a rigid body being pushed around. It is swept through
    // the world under its own rules — climbing steps, sliding off slopes,
    // stopping dead at walls — because a body given a walking speed stumbles on
    // stairs, skates down ramps, and turns a jump into a guess about impulses.
    //
    // Every character carries an ordinary kinematic body inside it, on
    // CollisionChannel::Character. That inner body is what makes a character
    // *exist* to the rest of the simulation: queries, contact events and
    // collision filtering all reach a character through it.
    //
    // A character is driven through its CharacterIntent and reports through its
    // CharacterState. Beyond those, only AddForce and AddImpulse reach it.

    /// @brief Sets the gravity vector (default: {0, −9.81, 0}).
    void SetGravity(glm::vec3 gravity);

    /// @brief Returns the current gravity vector.
    glm::vec3 GetGravity() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

/// @brief Running totals of what Jolt has allocated since the runtime came up.
///
/// **Churn, not residency.** JPH::FreeFunction takes no size, so tracking live
/// bytes would mean putting a header on every block, which breaks the aligned
/// allocation Jolt relies on. Churn is the more useful signal anyway: a physics
/// frame that allocates is a physics frame that will pay for the free later.
/// Sample once a frame and difference it to get a per-frame rate.
struct JoltAllocationStats
{
    uint64_t count = 0;
    uint64_t bytes = 0;
};

[[nodiscard]] JoltAllocationStats GetJoltAllocationStats();

} // namespace Assisi::Physics