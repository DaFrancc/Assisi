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

#include <Assisi/ECS/Query.hpp>
#include <Assisi/ECS/Transform.hpp>

#include <Jolt/Jolt.h>

// A sanitized build steps physics on one thread — see JoltRuntime.cpp for why.
#if defined(__SANITIZE_THREAD__)
#    define ASSISI_PHYSICS_TSAN 1
#elif defined(__has_feature)
#    if __has_feature(thread_sanitizer)
#        define ASSISI_PHYSICS_TSAN 1
#    endif
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
#include <unordered_map>
#include <vector>

namespace Assisi::Physics
{

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------
//
// The public BodyId is an opaque number; inside, it is Jolt's own packed
// index+sequence. Keeping the two spellings apart is what lets the simulation
// underneath be replaced without touching a component, a level file, or any code
// that merely holds a handle.

static_assert(JPH::BodyID::cInvalidBodyID == InvalidPhysicsHandle,
              "BodyId's empty value must be the one Jolt treats as invalid, or a default-constructed "
              "RigidBody would name a real body.");

inline JPH::BodyID ToJolt(BodyId id)
{
    return JPH::BodyID(id.value);
}

inline BodyId FromJolt(const JPH::BodyID &id)
{
    return BodyId{id.GetIndexAndSequenceNumber()};
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
static constexpr std::uint32_t MaskBits    = 16;
static constexpr std::uint32_t MotionBits  = 2;

static constexpr std::uint32_t ChannelShift = 0;
static constexpr std::uint32_t MaskShift    = ChannelShift + ChannelBits;
static constexpr std::uint32_t MotionShift  = MaskShift + MaskBits;

static constexpr std::uint32_t ChannelMask = (1u << ChannelBits) - 1u;
static constexpr std::uint32_t MaskMask    = (1u << MaskBits) - 1u;
static constexpr std::uint32_t MotionMask  = (1u << MotionBits) - 1u;

static_assert(static_cast<std::uint32_t>(CollisionChannel::Count) <= MaskBits,
              "Every channel needs a bit in the collides-with mask.");
static_assert(static_cast<std::uint32_t>(CollisionChannel::Count) <= ChannelMask + 1u,
              "Every channel needs to be nameable by the channel index.");
static_assert(static_cast<std::uint32_t>(BodyMotion::Count) <= MotionMask + 1u,
              "Every motion type needs to fit the motion field.");

inline JPH::ObjectLayer PackLayer(CollisionFilter filter, BodyMotion motion)
{
    const std::uint32_t channel = static_cast<std::uint32_t>(filter.channel) & ChannelMask;
    const std::uint32_t mask    = filter.collidesWith & MaskMask;
    const std::uint32_t packed  = (channel << ChannelShift) | (mask << MaskShift) |
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
        : _mask(filter.collidesWith), _channelBit(1u << static_cast<std::uint32_t>(filter.channel))
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

    JoltRuntimeRef(const JoltRuntimeRef &)            = delete;
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
// CharacterDescriptor; these are the numbers only someone debugging the sweep
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
/// whatever a descriptor says. Left unclamped, the stair sweep vaults the
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

/// Moves @p from toward @p to by at most @p maxDelta, landing exactly on @p to
/// rather than overshooting and oscillating around it.
///
/// A zero @p maxDelta holds @p from unchanged, which is what an acceleration of
/// zero has to mean: keep the velocity you had and ignore the request.
JPH::Vec3 MoveToward(JPH::Vec3Arg from, JPH::Vec3Arg to, float maxDelta);

/// Builds the Jolt collision shape for a descriptor. Radii and half-heights are
/// clamped to the convex radius, so a zeroed dimension field — reachable from an
/// inspector drag — cannot create a degenerate, asserting shape.
JPH::ShapeRefC MakeShape(const PhysicsWorld::ColliderShapeDesc &shape);

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct PhysicsWorld::Impl
{
    /* First member: brings the shared Jolt runtime up before any other member's
       constructor allocates through Jolt, and releases it after they are gone. */
    JoltRuntimeRef jolt;

    static constexpr uint32_t kMaxBodies = 1024;
    static constexpr uint32_t kMaxBodyPairs = 65536;
    static constexpr uint32_t kMaxContactConstraints = 10240;

    // Collision substeps per Update(); runtime-adjustable via SetCollisionSteps.
    // Defaults to 1 (a single solve, like Unity/Unreal at their fixed rate);
    // raise it to trade CPU for shallower impact penetration.
    static constexpr int32_t kDefaultCollisionSteps = 1;
    static constexpr int32_t kMaxCollisionSteps     = 16;
    int32_t collisionSteps = kDefaultCollisionSteps;

    BPLayerInterface bpLayerInterface;
    ObjVsBPFilter objVsBPFilter;
    ObjLayerFilter objLayerFilter;

    // Per-world scratch for this world's Update() (see JoltRuntime for why it is
    // not shared). Constructed after `jolt`, so the Jolt allocator is installed.
    JPH::TempAllocatorImpl tempAlloc{10u * 1024u * 1024u}; // 10 MiB

    JPH::PhysicsSystem physicsSystem;

    std::vector<JPH::BodyID> allBodyIds; ///< Every body ever added; used by Clear().

    /// Every body that can move — dynamic and kinematic both. The set whose pose
    /// is worth snapshotting each step and writing back for rendering, and the
    /// set to wake when gravity changes.
    ///
    /// Kinematic bodies belong here because something can drive one through the
    /// simulation (MoveBodyKinematic), and a body that moves with no writeback
    /// would be drawn wherever it was authored for the rest of the level's life.
    std::vector<JPH::BodyID> movingBodyIds;

    /// The last two stepped poses of a dynamic body, blended at render time so
    /// motion stays smooth when the display refreshes faster than physics steps.
    struct MotionSnapshot
    {
        glm::vec3 prevPosition{};
        glm::quat prevRotation{1.f, 0.f, 0.f, 0.f};
        glm::vec3 curPosition{};
        glm::quat curRotation{1.f, 0.f, 0.f, 0.f};
    };

    /// Keyed by BodyID's packed index+sequence so a lookup survives a body
    /// flipping motion type (which keeps its ID). Populated in AddBody, torn down
    /// in Clear.
    std::unordered_map<JPH::uint32, MotionSnapshot> snapshots;

    // --- Contact events ------------------------------------------------------

    /// The entity behind each body, keyed like `snapshots`. Only bodies created
    /// through AddBodyFromDescriptor appear — it is the one entry point that knows
    /// an entity — so a contact against a body from the raw AddBody reports
    /// NullEntity for that side rather than a wrong handle. A character's inner
    /// body is registered here too, which is what lets a cast or a contact name
    /// the entity behind a character.
    std::unordered_map<JPH::uint32, ECS::Entity> bodyEntities;

    /// The same association read the other way, for the one question that asks it
    /// that way: "which body is this entity's?". A query given an entity to ignore
    /// resolves it once here, rather than mapping every candidate body back to an
    /// entity to compare — the filter runs per body reached, and this runs once.
    std::unordered_map<ECS::Entity, JPH::BodyID> entityBodies;

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
    struct PairState
    {
        std::uint64_t stamp = 0;
        glm::vec3 normal{0.f};
        glm::vec3 velocity1{0.f};
        glm::vec3 velocity2{0.f};
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
    /// Held here because the entity behind a removed body is forgotten with it, so
    /// the event has to be built while the answer is still knowable.
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

    ECS::Entity EntityFor(const JPH::BodyID &id) const
    {
        const auto it = bodyEntities.find(id.GetIndexAndSequenceNumber());
        return it == bodyEntities.end() ? ECS::NullEntity : it->second;
    }

    /// The body @p entity owns, or an invalid id if it has none.
    JPH::BodyID BodyFor(ECS::Entity entity) const
    {
        const auto it = entityBodies.find(entity);
        return it == entityBodies.end() ? JPH::BodyID{} : it->second;
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
    /// is ever tested. Waking the bodies is what creates that step. Costs one
    /// broad-phase query and a brief loss of rest for whatever was inside.
    void WakeInside(const JPH::AABox &bounds, CollisionFilter filter);

    /// The same, for a body already in the simulation: wakes what its own bounds
    /// enclose, under its own filter.
    void WakeInside(const JPH::BodyID &id);

    /// The filter a body was created with, read back out of its packed layer.
    CollisionFilter FilterOf(const JPH::BodyID &id) const;

    /// Appends to @p out one event per side of a pair that has an entity.
    void EmitPair(const PairState &state, ContactPhase phase, std::vector<ContactEvent> &out);

    /// Emits an Exit for every pair naming @p id and forgets those pairs, while
    /// the entity behind the body is still knowable.
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
        void OnContactPersisted(const JPH::Body &body1, const JPH::Body &body2,
                                const JPH::ContactManifold &manifold, JPH::ContactSettings &settings) override
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

    /// Reserved so a default-constructed Character component names nothing. A
    /// handle that accidentally addressed the first character ever created would
    /// steer somebody else's.
    static constexpr std::uint32_t kInvalidCharacterId = 0;

    /// One character: the solver object, the two shapes it switches between, the
    /// authored numbers its step consults, and the intent it was last given.
    ///
    /// The descriptor's values are copied in rather than read back from the
    /// scene each step, because the step runs inside PhysicsWorld, which has no
    /// scene to read. Editing a descriptor therefore rebuilds the character —
    /// see ReconfigureEntityPhysics.
    struct CharacterRecord
    {
        JPH::Ref<JPH::CharacterVirtual> character;
        JPH::RefConst<JPH::Shape>       standingShape;
        JPH::RefConst<JPH::Shape>       crouchingShape;

        /// Where it is trying to go, as set by MoveCharacter. Kept between steps
        /// so a caller that stops asking keeps walking rather than stopping dead
        /// on a step nobody addressed it.
        glm::vec3 desiredVelocity{0.f};

        /// The last two stepped poses, for render interpolation. Held here
        /// rather than in `snapshots` because that map is keyed by the ids of
        /// bodies this world created and destroys, and a character's inner body
        /// is created and destroyed by Jolt.
        MotionSnapshot snapshot;

        ECS::Entity entity{ECS::NullEntity};

        /// What the character's sweep may be stopped by. The descriptor's mask
        /// minus Trigger: a sensor reports a character but must not block it, and
        /// a sweep that stopped at one would be an invisible wall. The inner body
        /// keeps the full mask, which is what lets the sensor see it.
        CollisionFilter queryFilter;

        float jumpSpeed          = 0.f;
        float groundAcceleration = 0.f;
        float airAcceleration    = 0.f;
        float gravityScale       = 1.f;
        float coyoteTime         = 0.f;
        float jumpBufferTime     = 0.f;
        float maxStepHeight      = 0.f;
        float radius             = 0.f;
        float standingHalfHeight = 0.f;
        float crouchHalfHeight   = 0.f;

        /// Seconds since last standing on walkable ground; what coyote time is
        /// measured against.
        float timeSinceGrounded = 0.f;

        /// Seconds a pending jump request has left before it is forgotten.
        float jumpBufferRemaining = 0.f;

        /// A jump asked for since the last step. Separate from the buffer above,
        /// which is only how long the request *outlives* this step: folding the
        /// two together would make a character with no buffer unable to jump at
        /// all, since the request would expire before it was ever tested.
        bool jumpRequested = false;

        Stance stance = Stance::Standing;

        /// Set when a jump fires, cleared on landing. Without it the jump buffer
        /// would fire a second jump in the same flight the moment the first one
        /// left the coyote window open.
        bool jumpedSinceGrounded = false;

        bool canPushBodies = true;
        bool canBePushed   = true;
        bool frozen        = false;
    };

    /// Ordered, not hashed: characters are stepped in this order, and one that
    /// depended on the iteration order of an unordered_map would simulate
    /// differently between two runs of the same level.
    std::map<std::uint32_t, CharacterRecord> characters;

    /// Which character an entity owns, for the entity-keyed entry points. The
    /// mirror of `entityBodies`, and the reason a lookup does not have to go
    /// through the scene — which matters where component destruction is deferred
    /// and the handle component may already be gone.
    std::unordered_map<ECS::Entity, std::uint32_t> entityCharacters;

    std::uint32_t nextCharacterId = kInvalidCharacterId + 1;

    /// What every character collides against every other one through. A
    /// character is not in the broad phase, so without this registry two of them
    /// would each sweep through a world the other is not part of and walk
    /// straight through each other.
    JPH::CharacterVsCharacterCollisionSimple characterVsCharacter;

    CharacterRecord *FindCharacter(const Character &character)
    {
        const auto it = characters.find(character.id.value);
        return it == characters.end() ? nullptr : &it->second;
    }

    const CharacterRecord *FindCharacter(const Character &character) const
    {
        const auto it = characters.find(character.id.value);
        return it == characters.end() ? nullptr : &it->second;
    }

    /// Sweeps every unfrozen character by one step, before the bodies are
    /// solved. See PhysicsWorld::Update.
    void StepCharacters(float deltaTime);

    /// Blends a snapshot's two poses, snapping instead when they are close
    /// enough that blending would only add wobble.
    static Pose BlendSnapshot(const MotionSnapshot &snapshot, float alpha);

    /// Writes a world-space render pose into @p entity's Transform, converting
    /// out of a parent's space and skipping a write that would change nothing.
    ///
    /// @p writeRotation is false for characters: the capsule is symmetric about
    /// its up axis, so the simulation has no opinion on facing and overwriting it
    /// would snap a turning character back to forward every frame.
    static void WriteRenderPose(ECS::Entity entity, ECS::Mut<ECS::Transform> transform, Pose pose,
                                bool writeRotation, const ParentWorldFn &parentWorld);

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

        void OnCharacterContactAdded(const JPH::CharacterVirtual *character,
                                     const JPH::CharacterVirtual *other,
                                     const JPH::SubShapeID &subShapeId, JPH::RVec3Arg contactPosition,
                                     JPH::Vec3Arg contactNormal,
                                     JPH::CharacterContactSettings &settings) override;

private:
        Impl &_owner;
    };

    CharacterContacts characterContacts{*this};
};

} // namespace Assisi::Physics
