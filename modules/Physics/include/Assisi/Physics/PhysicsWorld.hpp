/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PhysicsWorld.hpp
/// @brief Jolt physics simulation wrapper.
///
/// One PhysicsWorld per scene. Per fixed step call Update() then CaptureState()
/// to snapshot the new body poses; once per render frame call
/// InterpolateTransforms() to blend the last two snapshots into ECS Transforms.
/// Because physics steps at a fixed rate but rendering does not, that blend is
/// what keeps physics-driven motion smooth on high-refresh displays instead of
/// beating against the step rate.

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
/// is an offset from that parent, the mismatch ParentWorldFn exists to undo. It
/// also carries a scale and a cached world matrix that mean nothing to a body
/// whose size comes from its collider.
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

    /// The entity whose body was struck, or NullEntity when that body has none —
    /// only AddBodyFromDescriptor knows an entity. The handle is the one the body
    /// was created with, so check it is still alive before acting on it.
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
/// which end of the pair it is looking at. A body whose entity is unknown to
/// this world (created through the raw AddBody, which takes no entity) is only
/// ever the @ref other side.
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

/// @brief Wraps a Jolt PhysicsSystem and exposes a minimal API for the game loop.
///
/// Construction initialises the Jolt library (RegisterTypes, Factory).
/// Destruction cleans up all bodies and unregisters Jolt types.
class PhysicsWorld
{
public:
    PhysicsWorld();
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld &) = delete;
    PhysicsWorld &operator=(const PhysicsWorld &) = delete;

    /// @brief The collider primitive + its dimensions, gathered into one struct so
    /// the body-creation calls don't take a growing pile of shape parameters. Only
    /// the fields the chosen `shape` uses are read.
    struct ColliderShapeDesc
    {
        ColliderShape shape = ColliderShape::Box;
        glm::vec3 halfExtents{0.5f, 0.5f, 0.5f};     ///< Box.
        float radius     = 0.5f;                     ///< Sphere/Capsule/Cylinder.
        float halfHeight = 0.5f;                     ///< Capsule/Cylinder cylindrical half-height.
    };

    /// @brief Creates a rigid body with the given collider and returns its component.
    ///
    /// A body on the Trigger channel is created as a sensor: detected by whatever
    /// enters it, blocking nothing. Its motion chooses how faithfully it detects
    /// rather than whether it moves — Static sees only bodies that are awake,
    /// while Dynamic and Kinematic both build an activated kinematic sensor that
    /// never sleeps and so also sees bodies at rest. Dynamic is folded into
    /// Kinematic because a sensor falling under gravity is never what was meant.
    ///
    /// @param pose    Centre and orientation in world space (rotation is normalized internally).
    /// @param shape   Collider primitive and its dimensions.
    /// @param motion  Static bodies never move; dynamic bodies fall under gravity;
    ///                kinematic bodies move only when something sets their pose.
    /// @param filter  What this body is, and what it interacts with.
    RigidBody AddBody(const Pose &pose, const ColliderShapeDesc &shape, BodyMotion motion,
                      CollisionFilter filter);

    /// @brief Answers "what world matrix is this entity's Transform relative to?"
    /// — its parent's, or null if it has none.
    ///
    /// Physics reasons in world space; a Transform under a parent is an offset
    /// *from* that parent. Nothing here knows that on its own, and the two
    /// disagree silently: a body spawns at its local pose, and the world pose
    /// written back is multiplied by the parent again by whatever propagates
    /// transforms. A parented body therefore both starts in the wrong place and
    /// drifts by its parent's transform every frame.
    ///
    /// Supplied by the caller rather than read here because the parent link lives
    /// a layer up (Runtime::Parent) while Physics sits below it — Physics links
    /// Core + ECS + Jolt and deliberately not Runtime, which links Render and
    /// would poison the headless server's link. App provides one via
    /// App::ParentWorldResolver. An empty function means "nothing in this scene
    /// is parented", the common case, and costs a single branch.
    ///
    /// The parent's world matrix must be current, so propagate transforms before
    /// building bodies from a freshly loaded scene.
    using ParentWorldFn = std::function<const glm::mat4 *(Assisi::ECS::Entity entity)>;

    /// @brief Creates a Jolt body for @p entity from its authored descriptor at
    /// @p transform's pose, and attaches the transient RigidBody component.
    ///
    /// The durable RigidBodyDescriptor is what a level stores; this is the one
    /// place that turns it into live simulation state (motion type from
    /// `isStatic`, collider from the shape fields, CCD flag). Used by level
    /// load, play/stop scene restores, and live component-add in the editor.
    ///
    /// @param parentWorld Optional; see ParentWorldFn. Pass it whenever the
    ///                    entity might be parented.
    RigidBody AddBodyFromDescriptor(ECS::Scene &scene, ECS::Entity entity, const ECS::Transform &transform,
                                    const RigidBodyDescriptor &descriptor, const ParentWorldFn &parentWorld = {});

    /// @brief Rebuilds every body and character from the scene's descriptors:
    /// Clear(), then RebuildEntityPhysics for each entity carrying either
    /// descriptor alongside a Transform.
    ///
    /// For use when the scene's entities were replaced wholesale (level load, a
    /// play-session restore) and every live body is stale. Entities are expected
    /// not to carry a RigidBody or Character component yet — both are transient
    /// and never serialized, so a freshly loaded/restored scene never has one.
    ///
    /// An entity that cannot be built is logged and skipped rather than
    /// abandoning the rest of the scene: one bad descriptor should cost one
    /// object, not a level with no physics in it.
    ///
    /// @param parentWorld Optional; see ParentWorldFn. The world matrices it
    ///                    reads must already be propagated.
    void RebuildSceneBodies(ECS::Scene &scene, const ParentWorldFn &parentWorld = {});

    /// @brief Advances the simulation by `deltaTime` seconds.
    ///
    /// Characters are swept first, then the bodies are solved. That order is
    /// what lets a character shove a crate and the crate move in the same step,
    /// and what lets it ride a platform whose velocity was set before this call.
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
    [[nodiscard]] std::optional<QueryHit> CastShape(const ColliderShapeDesc &shape, const Pose &start,
                                                    glm::vec3 sweep, CollisionFilter filter,
                                                    ECS::Entity ignore) const;

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

    /// @brief Snapshots each dynamic body's pose for render interpolation.
    ///
    /// Call once per fixed step, immediately after Update(): it shifts the
    /// previous snapshot to the last-captured one and records the freshly
    /// stepped pose as the new current. InterpolateTransforms() then blends
    /// between those two.
    ///
    /// Everything that can move is snapshotted — dynamic bodies, kinematic ones
    /// (something can drive those through the simulation), and characters. Only
    /// static bodies are skipped, because their Transform is what placed them.
    void CaptureState();

    /// @brief Blends each dynamic body's previous/current snapshots into its
    /// Transform, `alpha` of the way from previous to current.
    ///
    /// Call once per render frame with the fixed-loop's interpolation alpha
    /// (`Application::GetInterpolationAlpha()`), which is the fraction of a
    /// physics step the accumulator holds. Only entities with a Transform and
    /// either a RigidBody or a Character are touched; static bodies are skipped,
    /// so their authored Transform is left intact. The written Transform is the
    /// *render* pose — the authoritative physics state is the current snapshot.
    ///
    /// A character's **rotation is left alone**. The capsule is symmetric about
    /// its up axis, so the simulation has no opinion about which way a character
    /// faces; that belongs to whatever is aiming it, and overwriting it here
    /// would snap a turning character back to forward every frame.
    ///
    /// @param parentWorld Optional; see ParentWorldFn. Pass it whenever a body
    ///                    might be parented — members of a blueprint instance
    ///                    routinely are.
    void InterpolateTransforms(Assisi::ECS::Scene &scene, float alpha, const ParentWorldFn &parentWorld = {});

    /// @brief Writes each dynamic body's *last stepped* pose into its Transform,
    /// with no blend.
    ///
    /// For worlds that simulate but are not rendered (a second resident level).
    /// Interpolation exists to smooth
    /// physics against a faster display; with nothing being displayed there is
    /// nothing to smooth against, and the render path that would normally call
    /// InterpolateTransforms never runs for these worlds — so without this their
    /// Transforms would sit at spawn pose forever no matter how much the bodies
    /// move. Call once per frame after the fixed-step loop, before propagating.
    void SyncTransforms(Assisi::ECS::Scene &scene, const ParentWorldFn &parentWorld = {})
    {
        InterpolateTransforms(scene, 1.f, parentWorld);
    }

    // --- Authoritative body state (replication) -------------------------------
    //
    // Replication reads the simulation directly, not the render-side Transform:
    // the render pose is not the physics truth. A headless host never runs the
    // writeback at all (its physics-driven entities would replicate their load
    // pose forever), and the writeback covers every body every frame including
    // sleeping ones (a settled world would never stop costing bandwidth).

    /// @brief One active body's authoritative motion state.
    struct ActiveBodyState
    {
        ECS::Entity entity;
        glm::vec3 position;
        glm::quat rotation;
        glm::vec3 linearVelocity;
        glm::vec3 angularVelocity;
    };

    /// @brief Every currently-awake dynamic body, with its pose and velocities.
    ///
    /// Only bodies created through AddBodyFromDescriptor appear — it is the one
    /// entry point that knows an entity, and a body with no entity is nothing a
    /// replication layer could name. The order Jolt returns active bodies in is
    /// unspecified, which is fine: every consumer of this re-sorts by its own
    /// identity.
    void GetActiveBodyStates(std::vector<ActiveBodyState> &out) const;

    /// @brief Whether the simulation currently considers @p body awake.
    [[nodiscard]] bool IsBodyActive(const RigidBody &body) const;

    /// @brief Put @p body to sleep without moving it.
    ///
    /// A kinematic sensor put to sleep stops detecting anything, since a sleeping
    /// body generates no contacts. Nothing prevents it; a trigger that has to be
    /// switched off is better expressed by removing the body.
    void DeactivateBody(const RigidBody &body);

    /// @brief Set pose, both velocities, and activation in one call.
    ///
    /// Deliberately not composed from the pieces above, because those have the
    /// wrong semantics for a correction three times over: SetBodyTransform
    /// reactivates every non-static body (so an "asleep" correction applied through
    /// it would wake the body), it zeroes the velocities, and there is no
    /// angular-velocity setter at all.
    ///
    /// Like SetBodyTransform it collapses both render-interpolation snapshots
    /// onto the target. That is load-bearing for the smoothing above this: the
    /// visual offset assumes the rendered pose is *unchanged* at the instant of
    /// a correction, and if the writeback also smeared the jump across a frame
    /// the two would double-count into a wobble at every correction.
    ///
    /// No-op for a static body or a handle not in the simulation.
    void ApplyBodyState(const RigidBody &body, glm::vec3 position, glm::quat rotation, glm::vec3 linearVelocity,
                        glm::vec3 angularVelocity, bool activate);

    /// @brief Returns the current world-space position and rotation of a body.
    std::pair<glm::vec3, glm::quat> GetBodyTransform(const RigidBody &body) const;

    /// @brief Returns a body's current linear (m/s) and angular (rad/s) velocity.
    ///
    /// Both are zero for a static body or one that isn't in the simulation, so
    /// callers can display the result unconditionally.
    std::pair<glm::vec3, glm::vec3> GetBodyVelocity(const RigidBody &body) const;

    /// @brief Whether a body's motion quality is currently LinearCast (CCD on).
    /// False for Discrete bodies, static bodies, or handles not in the simulation.
    bool IsBodyCCDEnabled(const RigidBody &body) const;

    /// @brief Teleports a body to the given position and rotation, and reactivates it.
    ///
    /// Moving a static sensor also wakes whatever lies in the space it left and
    /// the space it now occupies. Neither set would otherwise be re-tested — a
    /// sleeping body generates no contacts — so the volume would keep reporting
    /// what it no longer contains and stay silent about what it now does.
    ///
    /// This is a teleport, not a swept move: a small, fast-moving volume can pass
    /// through a thin body between steps without ever overlapping it on one.
    void SetBodyTransform(const RigidBody &body, glm::vec3 position, glm::quat rotation);

    /// @brief Replaces a body's linear velocity (m/s), waking it.
    ///
    /// Unlike SetBodyTransform this does not touch the pose or the interpolation
    /// snapshots — the change shows up through the simulation, over the following
    /// steps, exactly as if the solver had produced it. The wake is deliberate: a
    /// body that had gone to sleep on a surface would otherwise keep the new
    /// velocity on paper and never move. No-op on a static body, which has no
    /// velocity to set.
    void SetBodyLinearVelocity(const RigidBody &body, glm::vec3 velocity);

    /// @brief Changes what an existing body is and what it interacts with.
    ///
    /// Use this to apply inspector edits to the channel or the collides-with mask
    /// at runtime. Without it the descriptor and the live body disagree, and the
    /// edit appears to do nothing until something rebuilds the body from the
    /// descriptor — which reads as the change taking effect one play session late.
    ///
    /// Moving a body onto or off the Trigger channel makes it a sensor or stops
    /// it being one. A body that becomes a sensor while dynamic is made kinematic,
    /// the same rule AddBody applies; one that stops being a sensor keeps whatever
    /// motion it had, so restore that from the descriptor if it matters.
    void SetBodyCollisionFilter(const RigidBody &body, CollisionFilter filter);

    /// @brief What @p body is, and what it interacts with.
    [[nodiscard]] CollisionFilter GetBodyCollisionFilter(const RigidBody &body) const;

    /// @brief Replaces the collision shape of an existing body.
    ///
    /// Use this to apply inspector edits to the collider (shape type or its
    /// dimensions) at runtime without recreating the body.
    void ReshapeBody(const RigidBody &body, const ColliderShapeDesc &shape);

    /// @brief Removes and destroys a single body, dropping it from the simulation.
    ///
    /// Use when an entity's collider is deleted at runtime (the inspector's remove
    /// button). No-op for an invalid handle; the RigidBody component should be
    /// removed from the entity alongside this call.
    void RemoveBody(const RigidBody &body);

    /// @brief Enables or disables continuous collision detection (CCD) on a body.
    ///
    /// Only meaningful for dynamic bodies; no-op on static bodies.
    /// Uses Jolt's LinearCast motion quality for CCD, Discrete otherwise.
    void SetBodyCCD(const RigidBody &body, bool enable);

    /// @brief Changes the motion type of an existing body at runtime.
    ///
    /// Useful for temporarily freezing a dynamic body (e.g. while editing in an
    /// inspector) and restoring it afterwards.  Switching to Dynamic also activates
    /// the body so gravity takes effect immediately.
    void SetBodyMotionType(const RigidBody &body, BodyMotion motion);

    // --- Characters -----------------------------------------------------------
    //
    // A character is not a rigid body being pushed around. It is swept through
    // the world under its own rules — climbing steps, sliding off slopes,
    // stopping dead at walls — because a body given a walking speed stumbles on
    // stairs, skates down ramps, and turns a jump into a guess about impulses.
    //
    // Every character carries an ordinary kinematic body inside it, on
    // CollisionChannel::Character. That inner body is what makes a character
    // *exist* to the rest of the simulation: without one, a cast would pass
    // through it, a trigger would never report it, and two characters would walk
    // through each other. Queries, contact events and collision filtering all
    // reach a character through it, which is why they need no character-shaped
    // API of their own.
    //
    // Characters are stepped by Update(), before the bodies, so a character
    // reacts to where the world was at the start of the step and whatever it
    // pushed is solved in the same step rather than the next one.

    /// @brief Why a physics object could not be created for an entity.
    enum class PhysicsError : std::uint8_t
    {
        /// The simulation is full. The entity gets no body and no character
        /// rather than a handle that silently does nothing.
        BodyLimit,

        /// The entity carries both a RigidBodyDescriptor and a
        /// CharacterDescriptor. A character already owns a rigid body, so
        /// building both would have it collide with itself; neither is built.
        ConflictingDescriptors,

        /// The entity has no Transform, so there is nowhere to put it.
        NoTransform,

        Count,
    };

    /// @brief Creates a character for @p entity from its authored descriptor at
    /// @p transform's pose, and attaches the transient Character component.
    ///
    /// @p transform positions the character's **feet**, matching how the
    /// descriptor is authored — the capsule is built standing on that point.
    ///
    /// @param parentWorld Optional; see ParentWorldFn. Pass it whenever the
    ///                    entity might be parented.
    std::expected<Character, PhysicsError> AddCharacterFromDescriptor(
        ECS::Scene &scene, ECS::Entity entity, const ECS::Transform &transform,
        const CharacterDescriptor &descriptor, const ParentWorldFn &parentWorld = {});

    /// @brief Removes and destroys a character, inner body and all.
    ///
    /// Emits the same contact Exits a destroyed body does, so whatever the
    /// character was standing in hears that it left. The Character component
    /// should be removed from the entity alongside this call.
    void RemoveCharacter(const Character &character);

    /// @brief Sets what @p character is trying to do on the next step.
    ///
    /// @p desiredVelocity is a world-space velocity, already scaled to a real
    /// speed — the controller accelerates toward it rather than snapping to it,
    /// at the descriptor's ground or air rate depending on where the character
    /// is. Its vertical component is ignored; gravity and @p jump own that axis.
    ///
    /// @p jump is a request that survives a short time: asked for just before
    /// landing it fires on the landing step, and asked for just after walking
    /// off a ledge it still counts as a ground jump. Asked for while genuinely
    /// airborne it is dropped rather than queued, so a held button cannot bank
    /// jumps.
    void MoveCharacter(const Character &character, glm::vec3 desiredVelocity, bool jump);

    /// @brief Asks @p character to stand or crouch.
    ///
    /// @return false when the change could not be made, which in practice means
    /// standing up under something too low. The stance is unchanged in that
    /// case, and asking again once the character has moved clear succeeds — so
    /// a caller holding a crouch simply asks every step rather than tracking
    /// whether it is stuck.
    bool SetCharacterStance(const Character &character, Stance stance);

    /// @brief What @p character's last step left behind.
    [[nodiscard]] CharacterState GetCharacterState(const Character &character) const;

    /// @brief Teleports a character to a pose, feet first like the spawn.
    ///
    /// Moves the inner body with it rather than leaving that to the next step:
    /// a cast made between this call and the next Update() would otherwise find
    /// the character where it used to be. Collapses both render-interpolation
    /// snapshots onto the target, so the jump is not smeared across a frame, and
    /// re-finds what the character is touching at the destination.
    void SetCharacterTransform(const Character &character, glm::vec3 position, glm::quat rotation);

    /// @brief Freezes a character in place, or releases it.
    ///
    /// A frozen character keeps its pose, does not fall, and ignores whatever it
    /// was asked to do — the character equivalent of pinning a body to Static
    /// while an author drags it. **Gravity included**: a character that kept
    /// falling would drop away from under the cursor. Its velocity is zeroed
    /// rather than resumed on release, since the pose it was dragged to says
    /// nothing about how fast it was going.
    ///
    /// While frozen it reports no contacts, because it is not being swept and so
    /// nothing is measuring what it touches. Its inner body stays where it is,
    /// so casts and sensors still find it.
    void SetCharacterFrozen(const Character &character, bool frozen);

    // --- Physics by entity ----------------------------------------------------
    //
    // The same operations addressed by entity rather than by handle, dispatching
    // on whichever descriptor the entity carries. What a caller outside this
    // module almost always wants: the editor, a level load and a blueprint spawn
    // all know an entity and none of them should have to ask which of two kinds
    // of physics it has, fetch the matching handle component, and get the pair
    // wrong for the kind they forgot about.

    /// @brief Destroys whatever physics @p entity has and rebuilds it from its
    /// descriptor, dropping the old handle component and attaching a new one.
    ///
    /// The one call that turns authored data into simulation for a single
    /// entity, whichever kind it is. Rebuilding an entity that has no descriptor
    /// simply leaves it with none.
    ///
    /// @param parentWorld Optional; see ParentWorldFn. Its world matrices must
    ///                    already be propagated.
    std::expected<void, PhysicsError> RebuildEntityPhysics(ECS::Scene &scene, ECS::Entity entity,
                                                           const ParentWorldFn &parentWorld = {});

    /// @brief Destroys whatever physics @p entity has and removes its handle
    /// component. Does nothing to an entity that has none.
    void RemoveEntityPhysics(ECS::Scene &scene, ECS::Entity entity);

    /// @brief Makes an existing object match its descriptor again, after the
    /// descriptor was edited.
    ///
    /// A body is retuned in place — shape, CCD and collision filter — because it
    /// can be. A character is destroyed and rebuilt, because its shape, slope
    /// and step height are baked into the solver at creation; it keeps its
    /// position and stance across the rebuild but **loses its velocity and its
    /// jump timers**, which is invisible in an editor and a small jolt if
    /// something edits a descriptor mid-play.
    ///
    /// Does nothing to an entity with no descriptor, or with a descriptor but no
    /// live object — use RebuildEntityPhysics to create one.
    void ReconfigureEntityPhysics(ECS::Scene &scene, ECS::Entity entity,
                                  const ParentWorldFn &parentWorld = {});

    /// @brief Freezes @p entity's physics in place, or releases it.
    ///
    /// A body goes Static and comes back to whatever its descriptor authored; a
    /// character is frozen (see SetCharacterFrozen). For an author dragging an
    /// object in an inspector, where the thing being dragged must not fall away
    /// under the cursor.
    void SetEntityPhysicsFrozen(ECS::Scene &scene, ECS::Entity entity, bool frozen);

    /// @brief Moves @p entity's physics to a pose, whichever kind it has.
    void SetEntityTransform(ECS::Scene &scene, ECS::Entity entity, glm::vec3 position,
                            glm::quat rotation);

    /// @brief Moves a kinematic body to a pose *and gives it the velocity that
    /// move implies*, over a step of @p deltaTime.
    ///
    /// The call a moving platform needs. SetBodyTransform teleports: it zeroes
    /// the body's velocity, so anything standing on the result is standing on
    /// something the simulation believes is stationary, and slides off the back
    /// of a platform that is visibly moving. This sets the velocity that carries
    /// the body there instead, which is both what pushes resting bodies along
    /// and what a character reads to ride it.
    ///
    /// No-op for a static body, a non-positive @p deltaTime, or a handle not in
    /// the simulation.
    void MoveBodyKinematic(const RigidBody &body, glm::vec3 position, glm::quat rotation,
                           float deltaTime);

    /// @brief Removes and destroys all bodies and characters, resetting the
    /// world to an empty state.
    void Clear();

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