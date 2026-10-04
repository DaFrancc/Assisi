/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PhysicsInternal.hpp
/// @brief What the pieces of PhysicsWorld share with each other, and with
///        nothing else.
///
/// PhysicsWorld is one class split across several translation units — bodies,
/// contacts, queries, characters, writeback — because one file holding all of it
/// was long enough that finding anything meant scrolling past everything. They
/// share a pimpl and a set of Jolt filter implementations, and this is where
/// those live.
///
/// **Not an installed header.** It sits in src/ and names Jolt types the public
/// headers deliberately hide; anything outside this module that included it
/// would be reaching past the pimpl.

#include <Assisi/Physics/PhysicsWorld.hpp>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>

#include <Jolt/Jolt.h>

// A sanitized build steps physics on one thread — see JoltRuntime.cpp for why.
#if defined(__SANITIZE_THREAD__)
#define ASSISI_PHYSICS_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define ASSISI_PHYSICS_TSAN 1
#endif
#endif

#include <Jolt/Core/JobSystem.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Assisi::Physics
{

// ---------------------------------------------------------------------------
// Bodies and entities
// ---------------------------------------------------------------------------
//
// Every body this world builds carries its entity in Jolt's per-body user data,
// a character's inner body included (Jolt copies the character's onto it). The
// packed handle is stored inverted so that NullEntity, all ones, stores as 0 —
// which is also what Jolt reports for a body it cannot lock — and so that
// Entity{0, 0}, the first entity of every scene, does not.

inline JPH::uint64 UserDataOf(ECS::Entity entity)
{
    return ~((static_cast<JPH::uint64>(entity.generation) << 32) | static_cast<JPH::uint64>(entity.index));
}

inline ECS::Entity EntityOfUserData(JPH::uint64 userData)
{
    const JPH::uint64 packed = ~userData;
    return ECS::Entity{static_cast<std::uint32_t>(packed & 0xFFFFFFFFull), static_cast<std::uint32_t>(packed >> 32)};
}

// ---------------------------------------------------------------------------
// Object / broad-phase layers
// ---------------------------------------------------------------------------

/* An ObjectLayer carries everything a filter decision needs, packed by hand
   rather than through Jolt's ObjectLayerPairFilterMask. That class splits the
   layer evenly into group and mask bits and derives the broad-phase layer from
   the group alone, which leaves nowhere to record whether a body moves — so
   static and dynamic bodies would share one broad-phase tree and every static
   body would be re-tested against every other one.

   Layout, low bits first: the channel's index, the mask of channels it collides
   with, and the motion type. 22 bits of the 32 the build configures.

   Jolt's ObjectLayer is uint16 unless JPH_OBJECT_LAYER_BITS says otherwise. The
   root CMakeLists sets it to 32, and Jolt makes that a PUBLIC definition on its
   own target so this file and Jolt agree; the assert is what catches the day
   they stop agreeing, because a narrower layer would silently truncate the mask
   and every body would collide with the wrong things. */
static_assert(sizeof(JPH::ObjectLayer) == sizeof(std::uint32_t),
              "Assisi packs 22 bits into a Jolt ObjectLayer; build Jolt with OBJECT_LAYER_BITS=32.");

static constexpr std::uint32_t ChannelBits = 4;
static constexpr std::uint32_t MaskBits = 16;
static constexpr std::uint32_t MotionBits = 2;

static constexpr std::uint32_t ChannelShift = 0;
static constexpr std::uint32_t MaskShift = ChannelShift + ChannelBits;
static constexpr std::uint32_t MotionShift = MaskShift + MaskBits;

static constexpr std::uint32_t ChannelMask = (1u << ChannelBits) - 1u;
static constexpr std::uint32_t MaskMask = (1u << MaskBits) - 1u;
static constexpr std::uint32_t MotionMask = (1u << MotionBits) - 1u;

static_assert(static_cast<std::uint32_t>(CollisionChannel::Count) <= MaskBits,
              "Every channel needs a bit in the collides-with mask.");
static_assert(static_cast<std::uint32_t>(CollisionChannel::Count) <= ChannelMask + 1u,
              "Every channel needs to be nameable by the channel index.");
static_assert(static_cast<std::uint32_t>(BodyMotion::Count) <= MotionMask + 1u,
              "Every motion type needs to fit the motion field.");

inline JPH::ObjectLayer PackLayer(CollisionFilter filter, BodyMotion motion)
{
    const std::uint32_t channel = static_cast<std::uint32_t>(filter.channel) & ChannelMask;
    const std::uint32_t mask = filter.collidesWith.bits & MaskMask;
    const std::uint32_t packed = (channel << ChannelShift) | (mask << MaskShift) |
                                 ((static_cast<std::uint32_t>(motion) & MotionMask) << MotionShift);
    return static_cast<JPH::ObjectLayer>(packed);
}

inline std::uint32_t ChannelOf(JPH::ObjectLayer layer)
{
    return (static_cast<std::uint32_t>(layer) >> ChannelShift) & ChannelMask;
}

/// The channel as a single set bit, ready to test against a collides-with mask.
inline std::uint32_t ChannelBitOf(JPH::ObjectLayer layer)
{
    return 1u << ChannelOf(layer);
}

inline std::uint32_t MaskOf(JPH::ObjectLayer layer)
{
    return (static_cast<std::uint32_t>(layer) >> MaskShift) & MaskMask;
}

inline BodyMotion MotionOf(JPH::ObjectLayer layer)
{
    return static_cast<BodyMotion>((static_cast<std::uint32_t>(layer) >> MotionShift) & MotionMask);
}

inline bool IsTriggerLayer(JPH::ObjectLayer layer)
{
    return ChannelOf(layer) == static_cast<std::uint32_t>(CollisionChannel::Trigger);
}

/// The rule both bodies and queries are filtered by: each side's mask must admit
/// the other's channel. One-sided agreement is not enough, so either participant
/// can refuse the pair on its own.
inline bool ChannelsAgree(std::uint32_t maskA, std::uint32_t channelBitA, std::uint32_t maskB,
                          std::uint32_t channelBitB)
{
    return (maskA & channelBitB) != 0u && (maskB & channelBitA) != 0u;
}

/// @brief Identifies one pair of bodies, whichever order they are named in.
///
/// Two BodyIDs' index+sequence numbers packed into one value, the lower first.
/// A type of its own rather than a bare integer because nothing about it is a
/// number: adding to it, comparing it for order, or handing it to anything
/// expecting a count are all meaningless, and a bare uint64_t invites all three.
///
/// Deliberately **not** a Core::StrongId. That marker declares a type's wire
/// form, and this one has none — it is built from live BodyIDs, which mean
/// nothing outside the process that issued them.
struct PairKey
{
    std::uint64_t value = 0;

    friend constexpr bool operator==(PairKey, PairKey) = default;
};

/// Supplied to the map rather than specializing std::hash, which would mean
/// opening namespace std for a type that never leaves this module.
struct PairKeyHash
{
    std::size_t operator()(PairKey key) const noexcept { return std::hash<std::uint64_t>{}(key.value); }
};

namespace BPLayers
{
static constexpr JPH::BroadPhaseLayer Static(0);
static constexpr JPH::BroadPhaseLayer Moving(1);
static constexpr unsigned int Count = 2;
} // namespace BPLayers

// Maps object layers → broad-phase layers. Bodies that never move get their own
// tree, which is the one Jolt does not rebuild as the simulation runs.
class BPLayerInterface final : public JPH::BroadPhaseLayerInterface
{
public:
    unsigned int GetNumBroadPhaseLayers() const override { return BPLayers::Count; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
    {
        return MotionOf(layer) == BodyMotion::Static ? BPLayers::Static : BPLayers::Moving;
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
    {
        return layer == BPLayers::Static ? "Static" : "Moving";
    }
#endif
};

// Decides whether an object layer should be tested against a broad-phase layer.
class ObjVsBPFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bpLayer) const override
    {
        if (bpLayer != BPLayers::Static)
            return true;

        // Two bodies that both never move can never begin to touch, so walking
        // the static tree for one of them only ever confirms that.
        if (MotionOf(layer) == BodyMotion::Static)
            return false;

        // A sensor is kinematic, and Jolt generates no kinematic-vs-static
        // contact unless a body asks for one, which none here does. The walk
        // would find candidates and the narrow phase would discard every one.
        return !IsTriggerLayer(layer);
    }
};

// Decides whether two object layers should collide at all.
class ObjLayerFilter final : public JPH::ObjectLayerPairFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer layerA, JPH::ObjectLayer layerB) const override
    {
        if (MotionOf(layerA) == BodyMotion::Static && MotionOf(layerB) == BodyMotion::Static)
            return false;

        // One sensor inside another says nothing about the world: neither is
        // solid, so neither entered anything. Reachable because the default
        // sensor is kinematic, and Jolt does pair a kinematic body with a sensor.
        if (IsTriggerLayer(layerA) && IsTriggerLayer(layerB))
            return false;

        return ChannelsAgree(MaskOf(layerA), ChannelBitOf(layerA), MaskOf(layerB), ChannelBitOf(layerB));
    }
};

/// Applies the two-way channel rule between one participant and each body a
/// query reaches. A query is filtered exactly as a body is: it finds something
/// only if it admits that thing's channel and that thing admits its channel.
///
/// Unlike ObjLayerFilter this ignores motion entirely. A query is not a body: it
/// has no motion type, and the reasons two static bodies are never paired — they
/// cannot begin to touch — say nothing about whether a ray may hit a wall.
class FilterLayerFilter final : public JPH::ObjectLayerFilter
{
public:
    explicit FilterLayerFilter(CollisionFilter filter)
        : _mask(filter.collidesWith.bits), _channelBit(Core::Bitmask<CollisionChannel>::Of(filter.channel).bits)
    {
    }

    bool ShouldCollide(JPH::ObjectLayer layer) const override
    {
        return ChannelsAgree(_mask, _channelBit, MaskOf(layer), ChannelBitOf(layer));
    }

private:
    std::uint32_t _mask;
    std::uint32_t _channelBit;
};

// ---------------------------------------------------------------------------
// Shared Jolt runtime
// ---------------------------------------------------------------------------

/// @brief RAII handle to the process-wide Jolt runtime. The first one
/// constructed brings Jolt up; the last one destroyed tears it down.
///
/// See JoltRuntime.cpp for what "the runtime" is and why it is shared across
/// every PhysicsWorld rather than built per world.
class JoltRuntimeRef
{
public:
    JoltRuntimeRef();
    ~JoltRuntimeRef();

    JoltRuntimeRef(const JoltRuntimeRef &) = delete;
    JoltRuntimeRef &operator=(const JoltRuntimeRef &) = delete;

    // The base type, so the tsan build's single-threaded job system substitutes
    // without every caller caring which one it got.
    [[nodiscard]] JPH::JobSystem &JobSystem() const;
};

// ---------------------------------------------------------------------------
// Character tuning
// ---------------------------------------------------------------------------
//
// Solver tolerances, not game feel. Everything an author tunes is a field on
// Character; these are the numbers only someone debugging the sweep
// itself would touch, so each says what it prevents.

/// The world axis a character stands along. Characters do not model a rotating
/// gravity: slope angles, step heights and jump direction are all measured
/// against this one vector, and a world whose gravity pointed elsewhere would
/// have its characters walking on walls.
inline const JPH::Vec3 kCharacterUp = JPH::Vec3::sAxisY();

/// How far beyond the capsule contacts are predicted (meters). Too small and a
/// wall is discovered only once the capsule is inside it, with no surface left
/// to slide along; too large and the character slides along walls it has not
/// reached.
constexpr float kCharacterPredictiveContactDistance = 0.1f;

/// Fraction of an overlap pushed out per step. At 1 a character that spawns
/// inside geometry is ejected in one step rather than seeping out over several.
constexpr float kCharacterPenetrationRecoverySpeed = 1.f;

/// How far the sweep stays off geometry (meters). Keeps the capsule from coming
/// to rest exactly on a surface, where a contact flickers in and out between
/// steps and the character buzzes.
constexpr float kCharacterPadding = 0.02f;

/// A capsule sliding across two boxes laid edge to edge otherwise catches on the
/// seam: the shared edge is interior to neither box, so it reports a normal
/// facing into the direction of travel. Costs extra work per contact.
constexpr bool kCharacterEnhancedInternalEdgeRemoval = true;

/// Above this rising speed relative to its ground, a character reported as
/// standing has in fact just jumped and not yet left (m/s). Adopting the
/// ground's velocity there would cancel the jump on the very next step.
constexpr float kMaxRisingSpeedWhileGrounded = 0.1f;

/// How much overlap a stance change tolerates, as a multiple of the solver's
/// resting slop. Exactly zero would refuse to stand up while the head is within
/// the tolerance the solver itself allows, so standing would fail in the open.
constexpr float kStanceChangePenetrationSlopFactor = 1.5f;

/// A step taller than this fraction of a character's standing height is a wall,
/// whatever a Character says. Left unclamped, the stair sweep vaults the
/// character up surfaces shorter than itself — over railings, onto tables —
/// which reads as the collision simply not working.
constexpr float kMaxStepHeightFraction = 0.5f;

/// Builds a character's capsule with the **bottom of the shape at the origin**,
/// which is what puts an entity's Transform at its feet: an author stands a
/// character on the floor rather than guessing where its middle is. Jolt's
/// capsule is centred on its origin, so it is lifted by its own half-height.
///
/// Dimensions are clamped like a collider's, so a zeroed field from an inspector
/// drag cannot build a degenerate shape that asserts.
JPH::RefConst<JPH::Shape> MakeCharacterShape(float radius, float halfHeight);

/// Distance from a character's feet to the middle of the capsule
/// @ref MakeCharacterShape builds.
float CharacterHalfHeight(float radius, float halfHeight);

/// Below this horizontal speed friction stops the character outright (m/s).
/// Dividing by a speed this small to keep the direction would amplify rounding
/// into a velocity that wanders.
constexpr float kFrictionMinSpeed = 0.002f;

/// Below this requested speed there is no direction to accelerate along (m/s):
/// normalizing it would divide by nearly zero.
constexpr float kMinWishSpeed = 1.0e-4f;

/// With BunnyHopPolicy::Boost, the share of its forward request a standing
/// character's jump adds, and equally how far over walkSpeed the jump may leave
/// it. Half-Life 2's value for a player who is not sprinting.
constexpr float kJumpBoostStanding = 0.5f;

/// The same for a crouched character. The smaller margin is why back hopping
/// gains more while crouched: more of the speed counts as over the limit.
constexpr float kJumpBoostCrouching = 0.1f;

/// What ground friction leaves of a horizontal @p velocity after @p deltaTime:
/// the speed drops by `max(speed, stopSpeed) * friction * deltaTime` and never
/// below zero, so the direction is kept and the character cannot be pushed
/// backwards by its own friction.
JPH::Vec3 ApplyFriction(JPH::Vec3Arg velocity, float friction, float stopSpeed, float deltaTime);

/// Adds speed along the unit @p wishDirection until the velocity's component
/// along it reaches @p targetSpeed, by at most @p maxGain.
///
/// Only that one component is measured. A velocity already faster than
/// @p targetSpeed in another direction still gains along this one, which is
/// where both the slide of a ground turn and the speed of an air strafe come
/// from; one already at @p targetSpeed along it is returned unchanged, never
/// slowed.
JPH::Vec3 Accelerate(JPH::Vec3Arg velocity, JPH::Vec3Arg wishDirection, float targetSpeed, float maxGain);

/// Scales a horizontal @p velocity down to @p maxSpeed if it is faster, keeping
/// its direction.
JPH::Vec3 LimitSpeed(JPH::Vec3Arg velocity, float maxSpeed);

/// The world matrix @p entity's Transform is relative to, composed from the
/// local poses up its Parent chain: the simulation pose, not the drawn one the
/// propagated world matrices hold. Null for a root.
std::optional<glm::mat4> SimulationParentMatrix(const ECS::Scene &scene, ECS::Entity entity);

/// Builds the Jolt collision shape for a primitive. Radii and half-heights are
/// clamped to the convex radius, so a zeroed dimension field — reachable from an
/// inspector drag — cannot create a degenerate, asserting shape.
JPH::ShapeRefC MakeShape(const PhysicsWorld::ColliderShapeDesc &shape);

/// The scale a primitive is built at for a requested @p scale: as given for a
/// box, one scale on every axis for a sphere or a capsule, and one across the
/// round axes for a cylinder.
glm::vec3 ClampedShapeScale(ColliderShape shape, glm::vec3 scale);

/// How far a clamped scale may differ from the one asked for before the clamp
/// is worth a warning: rounding in a composed parent scale is not a mistake.
constexpr float kScaleClampTolerance = 1e-4f;

/// The same primitive, at ClampedShapeScale(@p scale).
JPH::ShapeRefC MakeScaledShape(const PhysicsWorld::ColliderShapeDesc &shape, glm::vec3 scale);

/// The primitive a Collider names.
PhysicsWorld::ColliderShapeDesc ShapeOf(const Collider &collider);

/// The whole shape a Collider builds at @p scale: its primitive at the
/// clamped scale, moved and turned by its offset, the offset scaled with it.
JPH::ShapeRefC MakeColliderShape(const Collider &collider, glm::vec3 scale);

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct PhysicsWorld::Impl
{
    /* First member: brings the shared Jolt runtime up before any other member's
       constructor allocates through Jolt, and releases it after they are gone. */
    JoltRuntimeRef jolt;

    explicit Impl(ECS::Scene &owner) : scene(owner) {}

    static constexpr uint32_t kMaxBodyPairs = 65536;
    static constexpr uint32_t kMaxContactConstraints = 10240;

    // Collision substeps per Update(); runtime-adjustable via SetCollisionSteps.
    // Defaults to 1 (a single solve, like Unity/Unreal at their fixed rate);
    // raise it to trade CPU for shallower impact penetration.
    static constexpr int32_t kDefaultCollisionSteps = 1;
    static constexpr int32_t kMaxCollisionSteps = 16;

    BPLayerInterface bpLayerInterface;
    ObjVsBPFilter objVsBPFilter;
    ObjLayerFilter objLayerFilter;

    // Per-world scratch for this world's Update() (see JoltRuntime for why it is
    // not shared). Constructed after `jolt`, so the Jolt allocator is installed.
    JPH::TempAllocatorImpl tempAlloc{10u * 1024u * 1024u}; // 10 MiB

    JPH::PhysicsSystem physicsSystem;

    ECS::Scene &scene;

    /// The Transform values this world last wrote to an entity, or found there
    /// when it last pushed them to the body, and the change tick they carried.
    ///
    /// A Transform whose tick differs was written by something else since.
    /// Which fields differ from these says what that write changed: a changed
    /// scale rebuilds the shape, and a character is only placed when its
    /// position changed, since the look systems turn it every frame.
    struct TransformStamp
    {
        glm::quat rotation{1.f, 0.f, 0.f, 0.f};
        glm::vec3 position{0.f};
        glm::vec3 scale{1.f};
        uint64_t tick = 0;
    };

    /// What an entity's slot holds.
    enum class SlotKind : std::uint8_t
    {
        Empty,
        Body,
        Character,
        Count,
    };

    /// Everything this world keeps about one entity, at that entity's index.
    ///
    /// Indexed rather than hashed: the reconcile and the writeback visit slots by
    /// entity many times a frame, and entity indices are dense. The generation
    /// beside it says which life of the index the slot belongs to.
    struct BodySlot
    {
        TransformStamp stamp;

        /// The Collider and RigidBody values the body was built or last
        /// retuned from, so an edit can be told apart from a write that changed
        /// nothing physical. The RigidBody is default for a static body.
        Collider collider;
        RigidBody rigidBody;

        /// The BodyState change tick this world last wrote or pushed. A
        /// different one was written by something else since.
        uint64_t stateTick = 0;

        /// The world-space scale the shape was built at.
        glm::vec3 worldScale{1.f};

        /// The body, or a character's inner body.
        JPH::BodyID body;

        CollisionFilter filter;
        uint32_t generation = 0;
        BodyMotion motion = BodyMotion::Static;
        SlotKind kind = SlotKind::Empty;

        /// Followed by the writeback: woken in some step and not yet written at
        /// rest. See `awake`.
        bool followed = false;
    };

    std::vector<BodySlot> slots;

    /// Entity indices of the bodies the writeback still follows. A body joins
    /// when Jolt reports it awake after a step and leaves once its Transform
    /// holds its resting pose, so a settled world costs the writeback nothing.
    std::vector<std::uint32_t> awake;

    /// Entity indices of the kinematic bodies swept to a Transform write in the
    /// step about to run, and in the one before. A sweep's velocity carries the
    /// body to its target in one step; one not swept again is stopped there.
    std::vector<std::uint32_t> sweptThisStep;
    std::vector<std::uint32_t> sweptLastStep;

    /// Change tick the next reconcile reads the scene's logs from.
    uint64_t changeCursor = 0;

    /// The scene's clear epoch when this world last reconciled. A different one
    /// means every entity handle in `slots` may name an unrelated entity.
    uint32_t clearEpoch = 0;

    uint32_t maxBodies = 0;
    int32_t collisionSteps = kDefaultCollisionSteps;

    /// True while Jolt is stepping. Nothing may call into the world then: the
    /// bodies are locked by the step, and the contact callbacks are reading them.
    bool stepping = false;

    /// Scratch for the reconcile, kept so a frame that changes nothing allocates
    /// nothing.
    std::vector<ECS::Entity> scratchChanged;
    std::vector<ECS::Entity> scratchRemoved;

    /// The slot @p entity's index maps to, if it holds that entity.
    BodySlot *SlotFor(ECS::Entity entity);
    const BodySlot *SlotFor(ECS::Entity entity) const;

    /// The slot at @p entity's index, created if the table is short.
    BodySlot &SlotAt(ECS::Entity entity);

    /// The entity a body was built for, or NullEntity for a body this world
    /// does not know.
    ECS::Entity EntityFor(const JPH::BodyID &id) const;

    /// The body @p entity owns — a character's inner body for a character — or
    /// an invalid id if it has none.
    JPH::BodyID BodyFor(ECS::Entity entity) const;

    // --- Reconcile (PhysicsReconcile.cpp) --------------------------------------

    /// See PhysicsWorld::Reconcile. @p stepTime is the step about to run, or 0
    /// when none is, which places kinematic bodies rather than sweeping them.
    void ReconcileScene(float stepTime);

    /// Builds, retunes or destroys @p entity's body or character to match its
    /// components.
    void SyncEntity(ECS::Entity entity);

    /// Pushes a Transform written by something else to @p entity's body.
    void PushTransform(ECS::Entity entity, float stepTime);

    /// Pushes a BodyState velocity written by something else to @p entity's
    /// body, waking it.
    void PushBodyState(ECS::Entity entity);

    /// Builds @p entity's body from its Collider and, unless it is static, its
    /// @p rigidBody.
    void CreateBody(ECS::Entity entity, const Collider &collider, const RigidBody *rigidBody);

    /// Applies edits to @p entity's Collider and RigidBody to its live body.
    /// Gaining or losing the RigidBody rebuilds it, since a static body has no
    /// motion to change.
    void EditBody(ECS::Entity entity, const Collider &collider, const RigidBody *rigidBody);

    /// Sets a moving body's mass from @p rigidBody: overridden when it names
    /// one, from the shape's volume otherwise.
    void ApplyMass(const JPH::BodyID &body, const RigidBody &rigidBody);

    /// Logs that @p entity's @p shape cannot take @p scale and is built at the
    /// nearest scale it can. Called where a scale is applied, not every step.
    void WarnOnClampedScale(ECS::Entity entity, ColliderShape shape, glm::vec3 scale) const;

    void CreateCharacter(ECS::Entity entity, const Character &character);

    /// Retunes @p entity's character from an edited Character, keeping its
    /// velocity, timers and stance. The capsule is rebuilt only when its size
    /// changed, since the solver bakes that in.
    void EditCharacter(ECS::Entity entity, const Character &character);

    /// Destroys whatever the slot at @p index holds, queuing the contact Exits
    /// its departure causes.
    void DestroySlot(std::uint32_t index);

    /// Puts @p entity's BodyState back at rest, so a body built for it later
    /// starts from a standstill rather than from the motion of the one destroyed.
    void ForgetBodyState(ECS::Entity entity);

    /// Destroys every body and character without reporting Exits: what was
    /// touching no longer exists to hear it.
    void DestroyAll();

    /// Records @p entity's current Transform as this world's own: what it last
    /// wrote, or what it has just pushed to the body.
    void StampTransform(ECS::Entity entity);

    /// Starts following the slot at @p index in the writeback.
    void Follow(std::uint32_t index);

    // --- Contact events ------------------------------------------------------

    /// One body pair seen touching during the step Jolt is running.
    ///
    /// Deliberately not an event: the callbacks cannot tell Enter from Stay
    /// without the previous step's pairs, and cannot see Exit at all. They record
    /// what touched; `pairs` turns that into phases once the step is over.
    struct TouchRecord
    {
        glm::vec3 normal{0.f};    ///< Away from body1's surface, as Jolt reports it.
        glm::vec3 velocity1{0.f}; ///< Pre-solve, so an impact's speed survives the solver.
        glm::vec3 velocity2{0.f};
        JPH::BodyID id1;
        JPH::BodyID id2;
        bool sensor = false;
    };

    /// What is known about a pair that was touching as of some step.
    ///
    /// `stamp` is the step it was last seen in; the sweep after each step treats
    /// anything older as gone. The velocities are the last ones observed, which is
    /// all an Exit can carry — nothing measured the pair on the step it ended.
    /// The entities are captured when the pair is seen, so an Exit for a body
    /// destroyed since still names who it was.
    struct PairState
    {
        std::uint64_t stamp = 0;
        glm::vec3 normal{0.f};
        glm::vec3 velocity1{0.f};
        glm::vec3 velocity2{0.f};
        ECS::Entity entity1{ECS::NullEntity};
        ECS::Entity entity2{ECS::NullEntity};
        JPH::BodyID id1;
        JPH::BodyID id2;
        bool sensor = false;
    };

    /// Written from Jolt's worker jobs during Update(), drained on the main thread
    /// after it. The mutex only guards the append: contacts are rare relative to
    /// the collision work that produced them, so this never becomes the
    /// bottleneck, and per-thread buffers would cost more to merge than they save.
    std::mutex touchMutex;
    std::vector<TouchRecord> touchedThisStep;

    /// Every pair currently touching, keyed by both bodies. Survives across steps
    /// — it is the memory that makes Enter, Stay and Exit distinguishable.
    std::unordered_map<PairKey, PairState, PairKeyHash> pairs;

    /// Exits for pairs whose body was destroyed before the next step could notice.
    std::vector<ContactEvent> pendingExits;

    std::vector<ContactEvent> events;

    /// Counts Update() calls, so a pair's `stamp` says which step last saw it.
    std::uint64_t step = 0;

    /// Orders the two ids so a pair has one key whichever way Jolt reports it.
    static PairKey KeyFor(const JPH::BodyID &a, const JPH::BodyID &b)
    {
        const std::uint32_t ka = a.GetIndexAndSequenceNumber();
        const std::uint32_t kb = b.GetIndexAndSequenceNumber();
        const std::uint32_t lo = ka < kb ? ka : kb;
        const std::uint32_t hi = ka < kb ? kb : ka;
        return PairKey{(static_cast<std::uint64_t>(hi) << 32) | static_cast<std::uint64_t>(lo)};
    }

    /// Records that a pair touched. Called from Jolt's narrow phase — i.e.
    /// *before* the solver runs, which is the whole reason the velocities are
    /// captured here rather than read back afterwards.
    void RecordTouch(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold);

    /// Turns the step's touches into events, and ages out the pairs that ended.
    void ResolveContactEvents();

    /// Wakes every body overlapping @p bounds that @p filter would interact with.
    ///
    /// A sleeping body reports no contacts, so a static sensor cannot discover one
    /// that settled before the sensor arrived — there is no step on which the pair
    /// is ever tested — and a body resting on a wall that moved would float
    /// where the wall was. Waking the bodies is what creates that step. Costs one
    /// broad-phase query and a brief loss of rest for whatever was inside.
    void WakeInside(const JPH::AABox &bounds, CollisionFilter filter);

    /// The same, for a body already in the simulation: wakes what its own bounds
    /// enclose, under its own filter.
    void WakeInside(const JPH::BodyID &id);

    /// The world-space bounds of a body, or an empty box if it cannot be read.
    JPH::AABox BoundsOf(const JPH::BodyID &id) const;

    /// The filter a body was created with, read back out of its packed layer.
    CollisionFilter FilterOf(const JPH::BodyID &id) const;

    /// Appends to @p out one event per side of a pair that has an entity.
    static void EmitPair(const PairState &state, ContactPhase phase, std::vector<ContactEvent> &out);

    /// Emits an Exit for every pair naming @p id and forgets those pairs.
    void EmitExitsFor(const JPH::BodyID &id, std::vector<ContactEvent> &out);

    /// Installed for the world's whole life. A trigger volume is authored data,
    /// and a switch that had to be flipped to make one work is a switch somebody
    /// forgets, leaving a volume that silently does nothing.
    class ContactCollector final : public JPH::ContactListener
    {
public:
        explicit ContactCollector(Impl &owner) : _owner(owner) {}

        void OnContactAdded(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold,
                            JPH::ContactSettings &settings) override
        {
            (void)settings; // we observe contacts, we don't retune them
            _owner.RecordTouch(body1, body2, manifold);
        }

        // Persisted contacts are recorded exactly like new ones. Which of the two
        // Jolt called says only whether it saw the pair last step, and the pair
        // table already knows that — and knows it in the cases Jolt gets wrong,
        // where a body woke and Jolt reports a contact it never stopped having.
        void OnContactPersisted(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold,
                                JPH::ContactSettings &settings) override
        {
            (void)settings;
            _owner.RecordTouch(body1, body2, manifold);
        }

        // OnContactRemoved is deliberately not overridden. Jolt calls it when a
        // body falls asleep, which is not the pair ending, and forbids reading
        // either body because one may already be destroyed. The post-step sweep
        // decides what ended instead, and treats a pair whose bodies are both
        // asleep as still touching.

private:
        Impl &_owner;
    };

    ContactCollector collector{*this};

    // --- Characters ----------------------------------------------------------

    /// One character: the solver object, the two shapes it switches between,
    /// and the authored numbers its step consults.
    ///
    /// The Character's values are copied in rather than read back from the
    /// scene each step; the reconcile copies them again when it is edited.
    struct CharacterRecord
    {
        JPH::Ref<JPH::CharacterVirtual> character;
        JPH::RefConst<JPH::Shape> standingShape;
        JPH::RefConst<JPH::Shape> crouchingShape;

        /// The way it is looking, from its Transform at the start of the step.
        /// The capsule itself is never turned; only BunnyHopPolicy::Boost reads
        /// this.
        glm::vec3 facing{0.f, 0.f, -1.f};

        ECS::Entity entity{ECS::NullEntity};

        /// What the character's sweep may be stopped by. The Character's mask
        /// minus Trigger: a sensor reports a character but must not block it, and
        /// a sweep that stopped at one would be an invisible wall. The inner body
        /// keeps the full mask, which is what lets the sensor see it.
        CollisionFilter queryFilter;

        float jumpSpeed = 0.f;
        float walkSpeed = 0.f;
        float crouchSpeedScale = 0.f;
        float friction = 0.f;
        float stopSpeed = 0.f;
        float groundAcceleration = 0.f;
        float airAcceleration = 0.f;
        float airWishSpeedCap = 0.f;
        float bunnyHopSpeedCap = 0.f;
        float standingEyeHeight = 0.f;
        float crouchEyeHeight = 0.f;
        float eyeSpeed = 0.f;
        float gravityScale = 1.f;
        float coyoteTime = 0.f;
        float jumpBufferTime = 0.f;
        float maxStepHeight = 0.f;
        float radius = 0.f;
        float standingHalfHeight = 0.f;
        float crouchHalfHeight = 0.f;

        /// How much taller the standing capsule is than the crouching one (m),
        /// which is how far the feet move when the stance changes in the air.
        float stanceLift = 0.f;

        /// Where the eye is above the feet right now; see
        /// CharacterState::eyeHeight.
        float eyeHeight = 0.f;

        /// Seconds since last standing on walkable ground; what coyote time is
        /// measured against.
        float timeSinceGrounded = 0.f;

        /// Seconds a pending jump request has left before it is forgotten.
        float jumpBufferRemaining = 0.f;

        Stance stance = Stance::Standing;

        BunnyHopPolicy bunnyHop = BunnyHopPolicy::Cap;

        /// Set when a jump fires, cleared on landing. Without it the jump buffer
        /// would fire a second jump in the same flight the moment the first one
        /// left the coyote window open.
        bool jumpedSinceGrounded = false;

        bool canPushBodies = true;
        bool canBePushed = true;

        /// Copies the tuning a step reads from @p character, leaving the
        /// character's motion, timers and stance as they are.
        void Retune(const Character &character);

        /// The horizontal velocity, relative to the ground, that one step of
        /// intent turns @p velocity into. @p wish is the horizontal request;
        /// @p grounded is standing on walkable ground and not jumping this step.
        JPH::Vec3 Steer(JPH::Vec3Arg velocity, JPH::Vec3Arg wish, bool grounded, float deltaTime) const;

        /// The most horizontal speed a jump may leave the ground with.
        float TakeOffSpeedLimit() const;

        /// What a jump under BunnyHopPolicy::Boost turns the horizontal
        /// @p velocity into, given the horizontal request @p wish.
        JPH::Vec3 BoostTakeOff(JPH::Vec3Arg velocity, JPH::Vec3Arg wish) const;
    };

    /// Keyed by entity index, and ordered rather than hashed: characters are
    /// stepped in this order, and one that depended on the iteration order of an
    /// unordered_map would simulate differently between two runs of the same
    /// level.
    std::map<std::uint32_t, CharacterRecord> characters;

    /// What every character collides against every other one through. A
    /// character is not in the broad phase, so without this registry two of them
    /// would each sweep through a world the other is not part of and walk
    /// straight through each other.
    JPH::CharacterVsCharacterCollisionSimple characterVsCharacter;

    CharacterRecord *FindCharacter(ECS::Entity entity);
    const CharacterRecord *FindCharacter(ECS::Entity entity) const;

    /// Builds the CharacterVirtual for @p record from @p character at a
    /// world-space @p pose. False when the simulation is full.
    bool BuildCharacterVirtual(CharacterRecord &record, const Character &character, const Pose &pose);

    /// Reads every character's CharacterIntent and sweeps it by one step,
    /// before the bodies are solved. See PhysicsWorld::Update.
    void StepCharacters(float deltaTime);

    /// Changes @p record's capsule to @p stance's. False when it does not fit,
    /// which in practice means standing up under something too low; nothing
    /// changes then. On the ground the feet stay put and the head moves; in the
    /// air the head stays put and the feet move, and the Transform follows.
    bool ApplyStance(CharacterRecord &record, Stance stance);

    /// Stops every kinematic body swept last step and not this one, so it rests
    /// where its Transform put it rather than carrying on at the sweep's speed.
    void StopFinishedSweeps();

    /// Changes every character whose CharacterIntent asks for another stance,
    /// where it fits. See ApplyStance.
    void ApplyAskedStances();

    /// What @p record's last step left behind.
    CharacterState BuildCharacterState(const CharacterRecord &record) const;

    /// Writes each moving body's and every character's pose from the step
    /// that just ran into its Transform, and its velocities into its BodyState
    /// or CharacterState. A body that fell asleep gets one last write, at rest,
    /// and is then left alone.
    void WriteBack();

    /// Writes @p state into @p entity's BodyState, skipping a write that would
    /// change nothing, and stamps it as this world's own.
    void WriteBodyState(ECS::Entity entity, const BodyState &state);

    /// Writes @p state into @p entity's CharacterState, skipping a write that
    /// would change nothing.
    void WriteCharacterState(ECS::Entity entity, const CharacterState &state);

    /// Writes a world-space pose into @p entity's Transform, converting out of a
    /// parent's space and skipping a write that would change nothing, then
    /// stamps it as this world's own write.
    ///
    /// @p writeRotation is false for characters: the capsule is symmetric about
    /// its up axis, so the simulation has no opinion on facing and overwriting it
    /// would snap a turning character back to forward every frame.
    void WritePose(ECS::Entity entity, Pose pose, bool writeRotation);

    /// Records that a character touched a body, from inside the character's own
    /// sweep. The body-vs-body listener cannot see these: a character's inner
    /// body is kinematic, and against static geometry that pair generates no
    /// contact at all.
    void RecordCharacterTouch(const JPH::BodyID &innerBody, const JPH::BodyID &other, glm::vec3 normal,
                              glm::vec3 characterVelocity);

    /// Answers the character sweep's questions about what it may push and be
    /// pushed by, and records what it touched.
    ///
    /// Jolt asks per contact rather than per character, so these cannot be plain
    /// settings on the character — each answer is looked up through the user data
    /// the character was created with.
    class CharacterContacts final : public JPH::CharacterContactListener
    {
public:
        explicit CharacterContacts(Impl &owner) : _owner(owner) {}

        void OnContactAdded(const JPH::CharacterVirtual *character, const JPH::BodyID &bodyId,
                            const JPH::SubShapeID &subShapeId, JPH::RVec3Arg contactPosition,
                            JPH::Vec3Arg contactNormal, JPH::CharacterContactSettings &settings) override;

        void OnCharacterContactAdded(const JPH::CharacterVirtual *character, const JPH::CharacterVirtual *other,
                                     const JPH::SubShapeID &subShapeId, JPH::RVec3Arg contactPosition,
                                     JPH::Vec3Arg contactNormal, JPH::CharacterContactSettings &settings) override;

private:
        Impl &_owner;
    };

    CharacterContacts characterContacts{*this};
};

} // namespace Assisi::Physics
