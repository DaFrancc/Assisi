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

/// @brief Where a cast met the first body in its path.
///
/// One shape for both kinds of cast: what a caller does with a hit — place a
/// cursor, stand a character on it, decide a shot connected — reads the same
/// four fields whether a ray or a swept volume found it.
struct QueryHit
{
    glm::vec3 position{0.f}; ///< World-space point on the struck surface.

    /// Unit surface normal at @ref position, pointing out of the surface and so
    /// back towards the caster. Reflecting a direction about it, or comparing it
    /// against up to decide whether a surface is walkable, both work without the
    /// caller knowing which cast produced the hit.
    glm::vec3 normal{0.f};

    /// The entity whose body was struck. The handle is the one the body was
    /// built for, so check it is still alive before acting on it.
    ECS::Entity entity{ECS::NullEntity};

    /// Distance from the cast's origin to @ref position along the cast. Zero when
    /// the cast began already overlapping, which is a hit, not a miss.
    float distance = 0.f;
};

/// @brief Where a pair of bodies is in the life of their contact.
enum class ContactPhase : std::uint8_t
{
    Enter, ///< They were not touching last step and are now.
    Stay,  ///< They were touching last step and still are.
    Exit,  ///< They were touching last step and are not now.
    Count,
};

/// @brief One side of one body pair's contact during the most recent Update().
///
/// Reported per participant rather than per pair: two bodies touching produce
/// two events, one from each point of view, so a consumer never has to work out
/// which end of the pair it is looking at.
///
/// Exactly one event per pair per participant per step, whatever the pair's
/// contact geometry did: several manifolds and several collision substeps
/// collapse into one.
struct ContactEvent
{
    /// Unit world-space normal pointing away from @ref other's surface — so a
    /// body arriving at a floor sees +Y here, whichever way the pair was ordered.
    glm::vec3 normal{0.f};

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

    ContactPhase phase = ContactPhase::Enter;

    /// True when either body is a sensor, which is to say on the Trigger channel.
    /// A response that pushes back — a bounce, a damage impulse — wants only the
    /// contacts that actually resisted something, and a sensor resisted nothing.
    bool sensor = false;
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
    /// @param maxBodies  The most bodies this world can hold, characters'
    ///                   included. A body past it is not built, and the entity
    ///                   is named in the log.
    explicit PhysicsWorld(ECS::Scene &scene, uint32_t maxBodies = kDefaultMaxBodies);
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld &) = delete;
    PhysicsWorld &operator=(const PhysicsWorld &) = delete;

    /// @brief The collider primitive + its dimensions, gathered into one struct so
    /// the shape queries don't take a growing pile of shape parameters. Only the
    /// fields the chosen `shape` uses are read.
    struct ColliderShapeDesc
    {
        ColliderShape shape = ColliderShape::Box;
        glm::vec3 halfExtents{0.5f, 0.5f, 0.5f}; ///< Box.
        float radius = 0.5f;                     ///< Sphere/Capsule/Cylinder.
        float halfHeight = 0.5f;                 ///< Capsule/Cylinder cylindrical half-height.

        friend bool operator==(const ColliderShapeDesc &, const ColliderShapeDesc &) = default;
    };

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

    /// @brief Whether @p entity has a body or a character in this world.
    [[nodiscard]] bool HasBody(ECS::Entity entity) const;

    /// @brief Moves @p entity's body or character to a world-space @p pose and
    /// stops it, writing its Transform to match.
    ///
    /// Unlike a Transform write, which keeps a dynamic body's velocity, this
    /// zeroes it, and the render blend does not slide the body across the gap.
    void Teleport(ECS::Entity entity, const Pose &pose);

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
    // Always on. Trigger volumes are authored data, and a switch a level had to
    // remember to flip would make one silently do nothing.
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
    /// A pair that stops being reported while *both* its bodies are asleep counts
    /// as still touching, and keeps producing Stay. A sleeping body has not moved;
    /// the simulation merely stopped testing it.
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

    // --- World queries -------------------------------------------------------
    //
    // Each takes the filter of the thing doing the asking, so a query is
    // filtered by the same two-way rule as a collision: it finds a body only if
    // its own mask includes that body's channel and the body's mask includes
    // its channel.
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

    /// @brief The first body a swept shape meets, or nothing if it meets none.
    ///
    /// The character-controller query: "if this capsule moved by @p sweep, what
    /// would stop it?" A cast that begins already overlapping something reports
    /// it at distance 0 rather than missing.
    ///
    /// @param shape   Collider primitive and its dimensions to sweep.
    /// @param start   Where the shape begins, in world space.
    /// @param sweep   Direction and length together; a zero sweep finds nothing.
    /// @param filter  What is asking, and what it may find.
    /// @param ignore  An entity to skip, or NullEntity to skip nothing.
    [[nodiscard]] std::optional<QueryHit> CastShape(const ColliderShapeDesc &shape, const Pose &start, glm::vec3 sweep,
                                                    CollisionFilter filter, ECS::Entity ignore) const;

    /// @brief Every entity whose body intersects a shape held still.
    ///
    /// Answers the question a sensor cannot: what is inside this volume right
    /// now, static scenery included. A trigger reports bodies that move, because
    /// that is what a contact is; walls and floors never generate one, so asking
    /// once is the only way to hear about them.
    ///
    /// Bodies with no entity are left out — a list of NullEntity says nothing.
    /// Each entity appears once however many of its shapes overlap.
    [[nodiscard]] std::vector<ECS::Entity> Overlap(const ColliderShapeDesc &shape, const Pose &at,
                                                   CollisionFilter filter, ECS::Entity ignore) const;

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

    // --- Authoritative body state (replication) -------------------------------

    /// @brief Every entity whose body or character the simulation has awake.
    ///
    /// The order Jolt returns active bodies in is unspecified, which is fine:
    /// every consumer of this re-sorts by its own identity.
    void ActiveBodies(std::vector<ECS::Entity> &out) const;

    /// @brief Whether the simulation currently considers @p entity's body awake.
    [[nodiscard]] bool IsBodyActive(ECS::Entity entity) const;

    /// @brief Put @p entity's body to sleep without moving it.
    ///
    /// A kinematic sensor put to sleep stops detecting anything, since a sleeping
    /// body generates no contacts.
    void DeactivateBody(ECS::Entity entity);

    /// @brief Set pose, both velocities, and activation in one call — what a
    /// replication correction is.
    ///
    /// Deliberately not composed from a Transform write and a velocity setter:
    /// a correction must be able to leave a body asleep, set its angular
    /// velocity, and land in this step rather than the next reconcile.
    ///
    /// For a character, only the position and the linear velocity are taken:
    /// it faces wherever gameplay turns it, and it never sleeps.
    ///
    /// Writes the Transform and snaps it, so the drawn pose jumps with the
    /// correction. The replication view offset relies on that: it hides the
    /// jump, and a blend sliding after it would show twice.
    ///
    /// No-op for an entity with no body. A static body is placed and nothing more.
    void ApplyBodyState(ECS::Entity entity, const Pose &pose, glm::vec3 linearVelocity, glm::vec3 angularVelocity,
                        bool activate);

    /// @brief The current world-space pose of @p entity's body, or the identity
    /// pose when it has none.
    [[nodiscard]] Pose GetBodyPose(ECS::Entity entity) const;

    /// @brief Returns a body's current linear (m/s) and angular (rad/s) velocity.
    ///
    /// Both are zero for a static body or an entity with no body, so callers can
    /// display the result unconditionally.
    [[nodiscard]] std::pair<glm::vec3, glm::vec3> GetBodyVelocity(ECS::Entity entity) const;

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
    // CharacterState; there is no character call here.

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