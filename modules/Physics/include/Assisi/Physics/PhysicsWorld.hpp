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

#include <Jolt/Jolt.h>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace Assisi::Physics
{

/// @brief Motion type for newly created bodies.
enum class BodyMotion : std::uint8_t
{
    Static,  ///< Immovable; collides but is never moved by the simulation.
    Dynamic, ///< Fully simulated; affected by gravity and collisions.
};

/// @brief One side of a collision that began during the most recent Update().
///
/// Reported per participant rather than per pair: two bodies touching produce
/// two Contacts, one from each point of view, so a consumer never has to work
/// out which end of the pair it is looking at. A body whose entity is unknown to
/// this world (created through the raw AddBody, which takes no entity) is only
/// ever the @ref other side.
///
/// Only *new* contacts appear here. A body resting on the floor reports once, on
/// the step it lands, and then nothing — which is what keeps a contact-driven
/// response from re-firing every step into a body that is simply lying there.
struct Contact
{
    ECS::Entity entity{ECS::NullEntity}; ///< The entity this record speaks for.
    ECS::Entity other{ECS::NullEntity};  ///< What it hit; NullEntity if that body has no entity.

    /// Unit world-space normal pointing away from @ref other's surface — so a
    /// body arriving at a floor sees +Y here, whichever way the pair was ordered.
    glm::vec3 normal{0.f};

    /// @ref entity's linear velocity at the moment the contact was found, which
    /// is **before the solver ran**. This is the field that makes the log worth
    /// keeping: by the time a system sees it, the step has already absorbed the
    /// impact and the body's live velocity is whatever Jolt left it with. Zero
    /// for a static body.
    glm::vec3 velocity{0.f};
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
    /// @param position   Centre of the body in world space.
    /// @param rotation   Initial orientation as a quaternion (normalized internally).
    /// @param shape      Collider primitive and its dimensions.
    /// @param motion     Static bodies never move; dynamic bodies fall under gravity.
    RigidBody AddBody(glm::vec3 position, glm::quat rotation, const ColliderShapeDesc &shape, BodyMotion motion);

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

    /// @brief Rebuilds every body from the scene's descriptors: Clear(), then
    /// AddBodyFromDescriptor for each entity with a Transform + RigidBodyDescriptor.
    ///
    /// For use when the scene's entities were replaced wholesale (level load, a
    /// play-session restore) and every live body is stale. Entities are expected
    /// not to carry a RigidBody component yet — it is transient and never
    /// serialized, so a freshly loaded/restored scene never has one.
    ///
    /// @param parentWorld Optional; see ParentWorldFn. The world matrices it
    ///                    reads must already be propagated.
    void RebuildSceneBodies(ECS::Scene &scene, const ParentWorldFn &parentWorld = {});

    /// @brief Advances the simulation by `deltaTime` seconds.
    void Update(float deltaTime);

    // --- Contact reporting ---------------------------------------------------
    //
    // Off by default, and off costs nothing: with reporting disabled the world
    // installs no Jolt contact listener at all, so the simulation never makes the
    // call. It is per world, and a system that needs contacts switches it on for
    // itself when it runs — so a world whose level named no such system never pays
    // for it (docs/world-system-binding-design-notes.md §3).

    /// @brief Starts or stops recording new contacts during Update().
    /// Turning it off also drops whatever is currently logged.
    void SetContactReporting(bool enable);

    /// @brief Whether this world is recording contacts (see SetContactReporting).
    [[nodiscard]] bool IsContactReporting() const;

    /// @brief The contacts that began during the most recent Update(), or an
    /// empty span when reporting is off.
    ///
    /// Cleared at the top of every Update(), so the span describes exactly one
    /// fixed step and a consumer cannot process the same impact twice. Valid
    /// until the next Update() or Clear(). Entity handles in it are the ones this
    /// world's bodies were created with, so a consumer should still check the
    /// entity is alive before acting on it.
    [[nodiscard]] std::span<const Contact> Contacts() const;

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
    /// between those two. Static bodies never move and are skipped.
    void CaptureState();

    /// @brief Blends each dynamic body's previous/current snapshots into its
    /// Transform, `alpha` of the way from previous to current.
    ///
    /// Call once per render frame with the fixed-loop's interpolation alpha
    /// (`Application::GetInterpolationAlpha()`), which is the fraction of a
    /// physics step the accumulator holds. Only entities with both a Transform
    /// and a RigidBody are touched; static bodies are skipped, so their
    /// authored Transform is left intact. The written Transform is the *render*
    /// pose — the authoritative physics state is the current snapshot.
    ///
    /// @param parentWorld Optional; see ParentWorldFn. Pass it whenever a body
    ///                    might be parented — members of a blueprint instance
    ///                    routinely are (docs/blueprint-system-concept.md §12).
    void InterpolateTransforms(Assisi::ECS::Scene &scene, float alpha, const ParentWorldFn &parentWorld = {});

    /// @brief Writes each dynamic body's *last stepped* pose into its Transform,
    /// with no blend.
    ///
    /// For worlds that simulate but are not rendered (a second resident level —
    /// docs/multi-scene-design-notes.md §1). Interpolation exists to smooth
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

    /// @brief Removes and destroys all bodies, resetting the world to an empty state.
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