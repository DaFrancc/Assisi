/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Physics/PhysicsWorld.hpp>

#include <Assisi/Chiara/Chiara.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/ECS/TransformPose.hpp>

#include <Jolt/Jolt.h>

// A sanitized build steps physics on one thread — see kSanitized below for why.
#if defined(__SANITIZE_THREAD__)
#    define ASSISI_PHYSICS_TSAN 1
#elif defined(__has_feature)
#    if __has_feature(thread_sanitizer)
#        define ASSISI_PHYSICS_TSAN 1
#    endif
#endif

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Object / broad-phase layers
// ---------------------------------------------------------------------------

namespace
{

// This block sits above `namespace Assisi::Physics`, since Jolt needs the filter
// implementations before the world that installs them.
using Assisi::Physics::BodyMotion;
using Assisi::Physics::CollisionChannel;
using Assisi::Physics::CollisionFilter;

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

JPH::ObjectLayer PackLayer(CollisionFilter filter, BodyMotion motion)
{
    const std::uint32_t channel = static_cast<std::uint32_t>(filter.channel) & ChannelMask;
    const std::uint32_t mask    = filter.collidesWith & MaskMask;
    const std::uint32_t packed  = (channel << ChannelShift) | (mask << MaskShift) |
                                  ((static_cast<std::uint32_t>(motion) & MotionMask) << MotionShift);
    return static_cast<JPH::ObjectLayer>(packed);
}

std::uint32_t ChannelOf(JPH::ObjectLayer layer)
{
    return (static_cast<std::uint32_t>(layer) >> ChannelShift) & ChannelMask;
}

/// The channel as a single set bit, ready to test against a collides-with mask.
std::uint32_t ChannelBitOf(JPH::ObjectLayer layer)
{
    return 1u << ChannelOf(layer);
}

std::uint32_t MaskOf(JPH::ObjectLayer layer)
{
    return (static_cast<std::uint32_t>(layer) >> MaskShift) & MaskMask;
}

BodyMotion MotionOf(JPH::ObjectLayer layer)
{
    return static_cast<BodyMotion>((static_cast<std::uint32_t>(layer) >> MotionShift) & MotionMask);
}

bool IsTriggerLayer(JPH::ObjectLayer layer)
{
    return ChannelOf(layer) == static_cast<std::uint32_t>(CollisionChannel::Trigger);
}

/// The rule both bodies and queries are filtered by: each side's mask must admit
/// the other's channel. One-sided agreement is not enough, so either participant
/// can refuse the pair on its own.
bool ChannelsAgree(std::uint32_t maskA, std::uint32_t channelBitA, std::uint32_t maskB,
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
/// opening namespace std for a type that never leaves this file.
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

/* The Jolt state that is genuinely process-global and worth sharing across every
   PhysicsWorld: the library globals (allocator, Factory, type registration) and
   the job-system thread pool. Multi-scene runs several PhysicsWorlds side by side
   a pool per world would spawn
   hardware_concurrency() threads *per resident level* and oversubscribe the
   machine, so the pool is shared and every world's Update() dispatches onto it.

   Refcounted rather than a leaked singleton so a process that stops using physics
   gives its worker threads back, and so construction order is correct by
   construction: the globals are registered before the pool is built (Jolt
   allocates through its own allocator, which RegisterDefaultAllocator installs),
   and the pool outlives every PhysicsWorld that could still be stepping —
   PhysicsWorld::Impl holds its handle as its first member, so it is acquired
   before any other Jolt object of that world and released after all of them.
   Atomic so worlds constructed/destroyed on different threads can't lose a count
   and double-free the factory.

   The scratch allocator is deliberately NOT here — it is per-world (see Impl).
   TempAllocatorImpl is a stack, used throughout a step by the pool workers a
   single Update() dispatches; two worlds' Update()s sharing one would interleave
   their frames, and the accesses cross pool-worker threads with no happens-before
   edge (a data race ThreadSanitizer flags). A per-world allocator is 10 MiB of
   scratch each — cheap — and makes stepping safe whether worlds run sequentially
   or in parallel. The pool stays shared; Jolt is built for many PhysicsSystems on
   one JobSystem. */
/* Under ThreadSanitizer the pool is replaced by Jolt's single-threaded job
   system. Jolt's solver coordinates its workers through its own barriers and
   atomics rather than anything tsan models as a happens-before edge, so a
   threaded step reports races inside `JobSystem.h` and `TempAllocator.h` — Jolt's
   own headers, instrumented only because they are inlined into this TU (the Jolt
   library does not link Assisi::Sanitize). Those reports cannot be fixed here and
   bury any real race in noise. Stepping on one thread removes them at the source
   rather than hiding them behind a suppression, and costs only speed: Jolt's
   results do not depend on worker count, and everything around physics still runs
   threaded. */
struct JoltRuntime
{
#if defined(ASSISI_PHYSICS_TSAN)
    JPH::JobSystemSingleThreaded jobSystem;

    JoltRuntime() { jobSystem.Init(JPH::cMaxPhysicsJobs); }
#else
    // Default-constructed and then Init'd in the body rather than built by the
    // thread-starting constructor: Jolt requires the thread-init function to be
    // set *before* Init, and setting it afterwards compiles fine while silently
    // doing nothing. Without this the physics workers would stay anonymous in
    // every capture and every debugger.
    JPH::JobSystemThreadPool jobSystem;

    JoltRuntime()
    {
        jobSystem.SetThreadInitFunction(
            [](int threadIndex)
            { Assisi::Chiara::RegisterCurrentThread(("jolt-" + std::to_string(threadIndex)).c_str()); });
        jobSystem.Init(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                       static_cast<int>(std::thread::hardware_concurrency()) - 1);
    }
#endif
};

/// Jolt allocation counters. Churn per frame, not residency: JPH::FreeFunction
/// takes no size, so tracking live bytes would need a header on every block,
/// which breaks aligned allocation. Churn is the perf-relevant signal anyway —
/// a physics frame that allocates is a physics frame that will pay for it.
///
/// Relaxed atomics because Jolt allocates from its own worker threads; these are
/// sampled once a frame, so ordering between them does not matter.
std::atomic<std::uint64_t> gJoltAllocCount{0};
std::atomic<std::uint64_t> gJoltAllocBytes{0};

void *CountingAllocate(std::size_t size)
{
    gJoltAllocCount.fetch_add(1, std::memory_order_relaxed);
    gJoltAllocBytes.fetch_add(size, std::memory_order_relaxed);
    return std::malloc(size);
}

void *CountingReallocate(void *block, std::size_t oldSize, std::size_t newSize)
{
    gJoltAllocCount.fetch_add(1, std::memory_order_relaxed);
    if (newSize > oldSize)
    {
        gJoltAllocBytes.fetch_add(newSize - oldSize, std::memory_order_relaxed);
    }
    return std::realloc(block, newSize);
}

void CountingFree(void *block)
{
    std::free(block);
}

void *CountingAlignedAllocate(std::size_t size, std::size_t alignment)
{
    gJoltAllocCount.fetch_add(1, std::memory_order_relaxed);
    gJoltAllocBytes.fetch_add(size, std::memory_order_relaxed);
#if defined(_WIN32)
    return _aligned_malloc(size, alignment);
#else
    // std::aligned_alloc requires size to be a multiple of alignment.
    return std::aligned_alloc(alignment, ((size + alignment - 1) / alignment) * alignment);
#endif
}

void CountingAlignedFree(void *block)
{
#if defined(_WIN32)
    _aligned_free(block);
#else
    std::free(block);
#endif
}

std::atomic<int32_t> gJoltRefCount{0};
JoltRuntime *gJoltRuntime = nullptr;

/// @brief RAII handle to the shared runtime. The first one constructed brings
/// Jolt up; the last one destroyed tears it down.
class JoltRuntimeRef
{
public:
    JoltRuntimeRef()
    {
        if (gJoltRefCount++ == 0)
        {
            /* Must be called before any Jolt allocation — including the runtime's
               own pool and temp allocator below. Counting wrappers rather than
               RegisterDefaultAllocator: all five hooks, because installing only
               some leaves the rest null and Jolt calls them all. */
            JPH::Allocate        = CountingAllocate;
            JPH::Reallocate      = CountingReallocate;
            JPH::Free            = CountingFree;
            JPH::AlignedAllocate = CountingAlignedAllocate;
            JPH::AlignedFree     = CountingAlignedFree;
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
            gJoltRuntime = new JoltRuntime();
            Assisi::Core::Log::Info("Jolt: runtime up ({} worker thread(s), shared by every physics world){}.",
                                    gJoltRuntime->jobSystem.GetMaxConcurrency(),
#if defined(ASSISI_PHYSICS_TSAN)
                                    " — single-threaded, this is a ThreadSanitizer build"
#else
                                    ""
#endif
                                    );
        }
    }

    ~JoltRuntimeRef()
    {
        if (--gJoltRefCount == 0)
        {
            delete gJoltRuntime;
            gJoltRuntime = nullptr;
            JPH::UnregisterTypes();
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
        }
    }

    JoltRuntimeRef(const JoltRuntimeRef &) = delete;
    JoltRuntimeRef &operator=(const JoltRuntimeRef &) = delete;

    // The base type, so the tsan build's single-threaded job system substitutes
    // without every caller caring which one it got.
    JPH::JobSystem &JobSystem() const { return gJoltRuntime->jobSystem; }
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

namespace Assisi::Physics
{

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

    std::vector<JPH::BodyID> allBodyIds;     ///< Every body ever added; used by Clear().
    std::vector<JPH::BodyID> dynamicBodyIds; ///< Subset of allBodyIds; used to wake on gravity change.

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
    /// NullEntity for that side rather than a wrong handle.
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
};

void PhysicsWorld::Impl::RecordTouch(const JPH::Body &body1, const JPH::Body &body2,
                                     const JPH::ContactManifold &manifold)
{
    // Jolt's manifold normal is the direction body2 must move to separate from
    // body1, so it already points away from body1's surface. Which side gets which
    // sign is decided in EmitPair, where the participants are known.
    const JPH::Vec3 n = manifold.mWorldSpaceNormal;

    // Body::GetLinearVelocity asserts on a static body (no motion state to read).
    const auto linearVelocity = [](const JPH::Body &body)
                                {
                                    if (body.IsStatic())
                                        return glm::vec3(0.f);
                                    const JPH::Vec3 v = body.GetLinearVelocity();
                                    return glm::vec3(v.GetX(), v.GetY(), v.GetZ());
                                };

    const TouchRecord record{glm::vec3{n.GetX(), n.GetY(), n.GetZ()},
                             linearVelocity(body1),
                             linearVelocity(body2),
                             body1.GetID(),
                             body2.GetID(),
                             body1.IsSensor() || body2.IsSensor()};

    const std::lock_guard<std::mutex> lock(touchMutex);
    touchedThisStep.push_back(record);
}

CollisionFilter PhysicsWorld::Impl::FilterOf(const JPH::BodyID &id) const
{
    const JPH::ObjectLayer layer = physicsSystem.GetBodyInterface().GetObjectLayer(id);
    return CollisionFilter{MaskOf(layer), static_cast<CollisionChannel>(ChannelOf(layer))};
}

void PhysicsWorld::Impl::WakeInside(const JPH::AABox &bounds, CollisionFilter filter)
{
    const FilterLayerFilter layerFilter{filter};
    physicsSystem.GetBodyInterface().ActivateBodiesInAABox(bounds, {}, layerFilter);
}

void PhysicsWorld::Impl::WakeInside(const JPH::BodyID &id)
{
    JPH::BodyLockRead lock(physicsSystem.GetBodyLockInterface(), id);
    if (!lock.Succeeded())
        return;
    const JPH::AABox bounds = lock.GetBody().GetWorldSpaceBounds();
    const JPH::ObjectLayer layer = lock.GetBody().GetObjectLayer();
    lock.ReleaseLock();

    // The lock is released first: ActivateBodiesInAABox takes its own locks, and
    // holding one while it does would be a deadlock waiting for the right pair of
    // bodies.
    WakeInside(bounds, CollisionFilter{MaskOf(layer), static_cast<CollisionChannel>(ChannelOf(layer))});
}

void PhysicsWorld::Impl::EmitPair(const PairState &state, ContactPhase phase, std::vector<ContactEvent> &out)
{
    const ECS::Entity e1 = EntityFor(state.id1);
    const ECS::Entity e2 = EntityFor(state.id2);

    // Each side is given the normal pointing away from the *other*, which is what
    // a reflection wants and what spares a consumer working out the pair's order.
    if (e1 != ECS::NullEntity)
        out.push_back(ContactEvent{-state.normal, state.velocity1, e1, e2, phase, state.sensor});
    if (e2 != ECS::NullEntity)
        out.push_back(ContactEvent{state.normal, state.velocity2, e2, e1, phase, state.sensor});
}

void PhysicsWorld::Impl::ResolveContactEvents()
{
    // No Jolt worker is inside a callback here, so the buffer is ours alone.
    for (const TouchRecord &touch : touchedThisStep)
    {
        const PairKey key = KeyFor(touch.id1, touch.id2);
        const auto [it, inserted] = pairs.try_emplace(key);
        PairState &state = it->second;

        // Several manifolds, and several collision substeps, report the same pair
        // within one step. The first sets the phase; the rest only refresh what is
        // known about it, so a pair yields one event however often it was seen.
        const bool firstThisStep = state.stamp != step;

        state.normal    = touch.normal;
        state.velocity1 = touch.velocity1;
        state.velocity2 = touch.velocity2;
        state.id1       = touch.id1;
        state.id2       = touch.id2;
        state.sensor    = touch.sensor;
        state.stamp     = step;

        if (firstThisStep)
            EmitPair(state, inserted ? ContactPhase::Enter : ContactPhase::Stay, events);
    }
    touchedThisStep.clear();

    // Anything not seen this step either ended or went quiet. A body that fell
    // asleep has not moved, and Jolt simply stopped testing it, so a pair whose
    // bodies are all asleep is still touching and keeps saying so.
    const JPH::BodyInterface &bodies = physicsSystem.GetBodyInterface();
    for (auto it = pairs.begin(); it != pairs.end();)
    {
        PairState &state = it->second;
        if (state.stamp == step)
        {
            ++it;
            continue;
        }

        const bool eitherAwake = (bodies.IsAdded(state.id1) && bodies.IsActive(state.id1)) ||
                                 (bodies.IsAdded(state.id2) && bodies.IsActive(state.id2));
        if (eitherAwake)
        {
            EmitPair(state, ContactPhase::Exit, events);
            it = pairs.erase(it);
        }
        else
        {
            EmitPair(state, ContactPhase::Stay, events);
            state.stamp = step;
            ++it;
        }
    }

    // Jolt discovers contacts in whatever order its jobs happened to run, so two
    // runs of the same simulation can produce the same events in different orders.
    // A consumer that accumulates — summing damage, picking a ground contact —
    // would then disagree with itself run to run.
    std::sort(events.begin(), events.end(),
              [](const ContactEvent &a, const ContactEvent &b)
              {
                  if (a.entity.index != b.entity.index)
                      return a.entity.index < b.entity.index;
                  if (a.entity.generation != b.entity.generation)
                      return a.entity.generation < b.entity.generation;
                  if (a.other.index != b.other.index)
                      return a.other.index < b.other.index;
                  if (a.other.generation != b.other.generation)
                      return a.other.generation < b.other.generation;
                  return a.phase < b.phase;
              });
}

// ---------------------------------------------------------------------------
// PhysicsWorld
// ---------------------------------------------------------------------------

namespace
{
/* Jolt's BoxShape requires each half extent to be at least its convex radius
   (cDefaultConvexRadius) and asserts below that — reachable from an ordinary
   inspector drag. Clamp silently: a warning here would fire once per drag
   tick. */
JPH::Vec3 ClampedBoxHalfExtents(glm::vec3 halfExtents)
{
    const glm::vec3 clamped = glm::max(halfExtents, glm::vec3(JPH::cDefaultConvexRadius));
    return {clamped.x, clamped.y, clamped.z};
}

// Builds the Jolt collision shape for a descriptor. Radii/half-heights are clamped
// to the convex radius, like the box extents above, so a zeroed dimension field
// (reachable from an inspector drag) can't create a degenerate, asserting shape.
JPH::ShapeRefC MakeShape(const PhysicsWorld::ColliderShapeDesc &shape)
{
    const float radius     = glm::max(shape.radius, JPH::cDefaultConvexRadius);
    const float halfHeight = glm::max(shape.halfHeight, JPH::cDefaultConvexRadius);
    switch (shape.shape)
    {
    case ColliderShape::Sphere:
        return new JPH::SphereShape(radius);
    case ColliderShape::Capsule:
        return new JPH::CapsuleShape(halfHeight, radius);
    case ColliderShape::Cylinder:
        return new JPH::CylinderShape(halfHeight, radius);
    case ColliderShape::Box:
        break;
    }
    return new JPH::BoxShape(ClampedBoxHalfExtents(shape.halfExtents));
}
} // namespace

// Contact-solver tuning (see the constructor). Rather than brute-forcing high
// step rates, we lean on the same cheap mechanism Unity/Unreal use: speculative
// contacts (a predictive margin that stops a body at a surface within one solve)
// plus a small allowed overlap that resolves gently. Fast free-fallers that
// still slip past the fixed margin are handled per-body via CCD (enableCCD).
// Jolt defaults: 0.02 m slop, 0.2 Baumgarte, 0.02 m speculative distance, 0.75
// linear-cast threshold.
constexpr float kPenetrationSlop          = 0.01f; ///< Allowed resting overlap (meters) — Unity-like contact offset.
constexpr float kSpeculativeContactDist   = 0.05f; ///< Predictive contact margin (meters); catches moderate impacts in one solve.
// CCD (LinearCast) engages once a body moves more than this * its shape's inner
// radius in a step. Below Jolt's 0.75 default so CCD-enabled bodies stop sinking
// at lower speeds (no "floaty" landings), but not so low that they sweep on
// nearly every step: 0.3 keeps sweeps to genuinely fast motion. Only costs CPU
// for bodies with CCD on (enableCCD), so the perf downside is bounded. For a 1 m
// box (inner radius 0.5) this triggers at ~9 m/s / a ~4 m drop.
constexpr float kLinearCastThreshold      = 0.3f;

PhysicsWorld::PhysicsWorld()
{
    /* Impl's first member acquires the shared Jolt runtime, so the library is up
       (allocator/Factory/types) before any of its other members construct. */
    _impl = std::make_unique<Impl>();

    _impl->physicsSystem.Init(Impl::kMaxBodies, 0u, Impl::kMaxBodyPairs, Impl::kMaxContactConstraints,
                              _impl->bpLayerInterface, _impl->objVsBPFilter, _impl->objLayerFilter);

    // Prevent impact penetration the cheap way (see the constant block above):
    // a wider speculative-contact margin lets the solver stop a body at a surface
    // within a single step, and a small allowed overlap keeps resting contacts
    // from jittering. Baumgarte and solver iteration counts stay at Jolt's
    // defaults — a gentle correction is less visible than an aggressive one.
    JPH::PhysicsSettings settings   = _impl->physicsSystem.GetPhysicsSettings();
    settings.mPenetrationSlop            = kPenetrationSlop;
    settings.mSpeculativeContactDistance = kSpeculativeContactDist;
    settings.mLinearCastThreshold        = kLinearCastThreshold;
    _impl->physicsSystem.SetPhysicsSettings(settings);

    /* Gravity: 9.81 m/s² downward (−Y). */
    _impl->physicsSystem.SetGravity(JPH::Vec3(0.f, -9.81f, 0.f));

    // Installed for the world's whole life rather than switched on by whoever
    // wants contacts. Trigger volumes are authored in a level, and a switch that
    // had to be flipped somewhere else to make one work is a switch that gets
    // forgotten, leaving a volume that silently does nothing.
    _impl->physicsSystem.SetContactListener(&_impl->collector);

    Assisi::Core::Log::Info("PhysicsWorld: initialized (Jolt).");
}

PhysicsWorld::~PhysicsWorld()
{
    /* Impl's members are destroyed in reverse declaration order, so the shared
       runtime handle (its first member) is released after this world's
       PhysicsSystem and bodies are gone. */
    _impl.reset();
}

RigidBody PhysicsWorld::AddBody(const Pose &pose, const ColliderShapeDesc &shape, BodyMotion motion,
                                CollisionFilter filter)
{
    const bool sensor = filter.channel == CollisionChannel::Trigger;

    // A sensor that fell under gravity would leave the volume it was authored as,
    // so Dynamic collapses to Kinematic here. Static stays static: that is the
    // cheap sensor, which notices only bodies that are awake.
    const BodyMotion effective =
        sensor && motion == BodyMotion::Dynamic ? BodyMotion::Kinematic : motion;

    JPH::EMotionType joltMotion = JPH::EMotionType::Dynamic;
    if (effective == BodyMotion::Static)
        joltMotion = JPH::EMotionType::Static;
    else if (effective == BodyMotion::Kinematic)
        joltMotion = JPH::EMotionType::Kinematic;

    const JPH::ObjectLayer layer = PackLayer(filter, effective);

    JPH::BodyCreationSettings settings(
        MakeShape(shape), JPH::RVec3(pose.position.x, pose.position.y, pose.position.z),
        JPH::Quat(pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w).Normalized(),
        joltMotion, layer);

    // Always allocate motion properties so the motion type can be changed at runtime
    // (e.g. making a Static body Dynamic via SetBodyMotionType).
    settings.mAllowDynamicOrKinematic = true;
    settings.mIsSensor                = sensor;

    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();

    // A kinematic sensor is activated and then never sleeps on its own, which is
    // what lets it find bodies that are already at rest — including ones it is
    // later moved onto.
    const JPH::BodyID bodyId = bodies.CreateAndAddBody(settings, JPH::EActivation::Activate);
    if (bodyId.IsInvalid())
    {
        Assisi::Core::Log::Error(
            "PhysicsWorld: failed to create body (body limit of {} reached?); entity will not simulate.",
            Impl::kMaxBodies);
        return RigidBody{bodyId};
    }

    _impl->allBodyIds.push_back(bodyId);
    if (effective == BodyMotion::Dynamic)
        _impl->dynamicBodyIds.push_back(bodyId);

    // Seed both snapshots with the spawn pose so the first interpolated frame
    // (before any step has run) resolves to exactly where the body was placed.
    _impl->snapshots[bodyId.GetIndexAndSequenceNumber()] =
        Impl::MotionSnapshot{pose.position, pose.rotation, pose.position, pose.rotation};

    // A static sensor is told about bodies that touch it, and a sleeping body
    // touches nothing. Waking whatever it now encloses is what lets it report the
    // things that were already sitting there when it appeared.
    if (sensor && effective == BodyMotion::Static)
        _impl->WakeInside(bodyId);

    return RigidBody{bodyId};
}

RigidBody PhysicsWorld::AddBodyFromDescriptor(ECS::Scene &scene, ECS::Entity entity, const ECS::Transform &transform,
                                              const RigidBodyDescriptor &descriptor, const ParentWorldFn &parentWorld)
{
    const BodyMotion motion = descriptor.isStatic ? BodyMotion::Static : BodyMotion::Dynamic;
    const ColliderShapeDesc shape{.shape       = descriptor.shape,
                                  .halfExtents = descriptor.halfExtents,
                                  .radius      = descriptor.radius,
                                  .halfHeight  = descriptor.halfHeight};

    // Jolt places bodies in world space, and a parented Transform is an offset
    // from its parent — the same mismatch InterpolateTransforms undoes on the way
    // back out. Without this a parented body spawns at its *local* pose and stays
    // there, which for a blueprint member means the instance's placement is
    // simply ignored.
    glm::vec3 position = transform.position;
    glm::quat rotation = transform.rotation;
    if (parentWorld)
    {
        if (const glm::mat4 *parent = parentWorld(entity); parent != nullptr)
        {
            const ECS::Transform pose = ECS::PoseUnderParent(transform, *parent);
            position                  = pose.position;
            rotation                  = pose.rotation;
        }
    }

    const RigidBody body =
        AddBody(Pose{rotation, position}, shape, motion,
                CollisionFilter{descriptor.collidesWith, descriptor.channel});
    if (descriptor.enableCCD)
        SetBodyCCD(body, true);
    (void)scene.Add<RigidBody>(entity, body);

    // The only body-creation path that knows an entity, so the only one that can
    // make a contact nameable in ECS terms. Recorded unconditionally: reporting can
    // be switched on later in the world's life, and rebuilding the map then would
    // mean walking the scene.
    if (!body.bodyId.IsInvalid())
    {
        _impl->bodyEntities[body.bodyId.GetIndexAndSequenceNumber()] = entity;
        _impl->entityBodies[entity]                                 = body.bodyId;
    }

    return body;
}

void PhysicsWorld::RebuildSceneBodies(ECS::Scene &scene, const ParentWorldFn &parentWorld)
{
    Clear();
    for (auto [entity, transform, descriptor] : scene.Query<ECS::Transform, RigidBodyDescriptor>())
        AddBodyFromDescriptor(scene, entity, transform, descriptor, parentWorld);
}

void PhysicsWorld::Clear()
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    for (const JPH::BodyID &id : _impl->allBodyIds)
    {
        if (bodies.IsAdded(id))
            bodies.RemoveBody(id);
        bodies.DestroyBody(id);
    }
    _impl->allBodyIds.clear();
    _impl->dynamicBodyIds.clear();
    _impl->snapshots.clear();
    _impl->bodyEntities.clear();
    _impl->entityBodies.clear();

    // Every pair and event names bodies that no longer exist — and, after a level
    // load, entity handles that mean something entirely different. No Exit is
    // emitted for what was touching: nothing survives that could act on one, and a
    // world being emptied is not a world where things left each other.
    _impl->pairs.clear();
    _impl->touchedThisStep.clear();
    _impl->pendingExits.clear();
    _impl->events.clear();
}

void PhysicsWorld::RemoveBody(const RigidBody &body)
{
    const JPH::BodyID id = body.bodyId;
    if (id.IsInvalid())
        return;

    // Everything this body was touching has stopped touching it, and the next
    // step cannot say so — the body will be gone and the entity behind it
    // forgotten. Build those Exits now, while both are still knowable, and let the
    // next Update() deliver them.
    const std::uint32_t goingKey = id.GetIndexAndSequenceNumber();
    for (auto it = _impl->pairs.begin(); it != _impl->pairs.end();)
    {
        const Impl::PairState &state = it->second;
        if (state.id1.GetIndexAndSequenceNumber() == goingKey ||
            state.id2.GetIndexAndSequenceNumber() == goingKey)
        {
            _impl->EmitPair(state, ContactPhase::Exit, _impl->pendingExits);
            it = _impl->pairs.erase(it);
        }
        else
        {
            ++it;
        }
    }

    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    if (bodies.IsAdded(id))
        bodies.RemoveBody(id);
    bodies.DestroyBody(id);

    // Drop it from the bookkeeping so CaptureState/InterpolateTransforms and a
    // later Clear() never touch the freed id. Match on the index+sequence key
    // rather than BodyID identity to avoid depending on operator==.
    const std::uint32_t key = id.GetIndexAndSequenceNumber();
    const auto matches = [key](const JPH::BodyID &b) { return b.GetIndexAndSequenceNumber() == key; };
    std::erase_if(_impl->allBodyIds, matches);
    std::erase_if(_impl->dynamicBodyIds, matches);
    _impl->snapshots.erase(key);

    // Last, because the Exits queued above were built from it: the entity behind
    // this body stops being knowable here.
    const ECS::Entity gone = _impl->EntityFor(id);
    _impl->bodyEntities.erase(key);
    if (gone != ECS::NullEntity)
        _impl->entityBodies.erase(gone);
}

void PhysicsWorld::Update(float deltaTime)
{
    // The events describe the step about to run, not the one before it — clearing
    // here is what guarantees a consumer sees each one exactly once. Safe without
    // the mutex: no Jolt worker is inside a callback at this point.
    _impl->events.clear();

    // Exits recorded when a body was destroyed. They belong to this step: the pair
    // ended when the body went away, and there was no step in between.
    _impl->events.swap(_impl->pendingExits);
    _impl->pendingExits.clear();

    ++_impl->step;

    /* This world's own scratch allocator, and the shared thread pool. Both are
       Update() arguments; the pool is shared (one set of workers), the allocator
       is per-world so two worlds' steps never touch the same scratch stack (see
       JoltRuntime). */
    _impl->physicsSystem.Update(deltaTime, _impl->collisionSteps, &_impl->tempAlloc,
                                &_impl->jolt.JobSystem());

    _impl->ResolveContactEvents();
}

std::span<const ContactEvent> PhysicsWorld::ContactEvents() const
{
    return {_impl->events.data(), _impl->events.size()};
}

// ---------------------------------------------------------------------------
// World queries
// ---------------------------------------------------------------------------

namespace
{

/// Hides one entity's bodies from a query.
///
/// What makes a self-cast usable: a character sweeping its own capsule forward
/// starts inside itself, and every convex shape reports a hit at distance 0 for
/// a cast that begins inside it. Filtering by entity rather than by body id lets
/// a caller name the thing it knows about.
/// Hides one body from a query.
///
/// Takes a BodyID rather than an entity, and overrides the *unlocked* half of
/// the filter, because both cost less where this runs. Jolt asks this question
/// once per body a query reaches, and asks the unlocked form first — answering
/// there is an integer compare, and skips locking a body only to reject it.
/// Resolving the caller's entity to a BodyID happens once, before the query.
class IgnoreBodyFilter final : public JPH::BodyFilter
{
public:
    explicit IgnoreBodyFilter(const JPH::BodyID &ignore) : _ignore(ignore) {}

    bool ShouldCollide(const JPH::BodyID &bodyId) const override
    {
        return bodyId.GetIndexAndSequenceNumber() != _ignore.GetIndexAndSequenceNumber();
    }

private:
    JPH::BodyID _ignore;
};

/// A sweep shorter than this is treated as no sweep at all.
///
/// Jolt needs a direction, and normalizing a zero-length vector yields NaNs that
/// propagate into every hit fraction. Squared, so the check costs no square root.
constexpr float kMinSweepLengthSq = 1e-12f;

} // namespace

std::optional<QueryHit> PhysicsWorld::CastRay(glm::vec3 origin, glm::vec3 sweep, CollisionFilter filter,
                                              ECS::Entity ignore) const
{
    const float sweepLengthSq = glm::dot(sweep, sweep);
    if (sweepLengthSq < kMinSweepLengthSq)
        return std::nullopt;

    const JPH::RRayCast ray{JPH::RVec3(origin.x, origin.y, origin.z),
                            JPH::Vec3(sweep.x, sweep.y, sweep.z)};

    JPH::RayCastSettings settings;
    // A ray that starts inside a body reports that body at fraction 0. Spelled
    // out rather than left to Jolt's default, because the single-hit entry point
    // has no settings at all and each shape decides for itself there — so the
    // behaviour would differ between a box and a mesh for no stated reason.
    settings.mTreatConvexAsSolid = true;
    settings.SetBackFaceMode(JPH::EBackFaceMode::IgnoreBackFaces);

    JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> collector;
    const FilterLayerFilter layerFilter{filter};
    const IgnoreBodyFilter  bodyFilter{_impl->BodyFor(ignore)};

    // The broad-phase filter accepts everything: a query carries no motion type,
    // so it has no business skipping the tree of things that do not move — which
    // is most of what a ray is aimed at.
    _impl->physicsSystem.GetNarrowPhaseQuery().CastRay(ray, settings, collector, {}, layerFilter,
                                                       bodyFilter);
    if (!collector.HadHit())
        return std::nullopt;

    const float     sweepLength = std::sqrt(sweepLengthSq);
    const glm::vec3 position    = origin + sweep * collector.mHit.mFraction;

    QueryHit hit;
    hit.position = position;
    hit.distance = sweepLength * collector.mHit.mFraction;
    hit.entity   = _impl->EntityFor(collector.mHit.mBodyID);

    // The normal has to come off the body's surface — a ray result carries only
    // which sub-shape was struck, not its orientation.
    JPH::BodyLockRead lock(_impl->physicsSystem.GetBodyLockInterface(), collector.mHit.mBodyID);
    if (lock.Succeeded())
    {
        const JPH::Vec3 n = lock.GetBody().GetWorldSpaceSurfaceNormal(
            collector.mHit.mSubShapeID2, JPH::RVec3(position.x, position.y, position.z));
        hit.normal = glm::vec3(n.GetX(), n.GetY(), n.GetZ());
    }
    return hit;
}

std::optional<QueryHit> PhysicsWorld::CastShape(const ColliderShapeDesc &shape, const Pose &start,
                                                glm::vec3 sweep, CollisionFilter filter,
                                                ECS::Entity ignore) const
{
    const float sweepLengthSq = glm::dot(sweep, sweep);
    if (sweepLengthSq < kMinSweepLengthSq)
        return std::nullopt;

    const JPH::Quat rotation =
        JPH::Quat(start.rotation.x, start.rotation.y, start.rotation.z, start.rotation.w).Normalized();
    const JPH::RMat44 transform =
        JPH::RMat44::sRotationTranslation(rotation, JPH::RVec3(start.position.x, start.position.y,
                                                               start.position.z));

    // Named, not a temporary in the call below: RShapeCast keeps a bare pointer to
    // the shape, so a ShapeRefC that died at the end of that expression would
    // leave the cast pointing at freed memory.
    const JPH::ShapeRefC   swept = MakeShape(shape);
    const JPH::RShapeCast  cast  = JPH::RShapeCast::sFromWorldTransform(
        swept, JPH::Vec3::sReplicate(1.f), transform, JPH::Vec3(sweep.x, sweep.y, sweep.z));

    const JPH::ShapeCastSettings settings;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    const FilterLayerFilter layerFilter{filter};
    const IgnoreBodyFilter  bodyFilter{_impl->BodyFor(ignore)};

    // Zero base offset, so the contact points come back in world space. Jolt is
    // built here at single precision, where the offset buys no accuracy.
    _impl->physicsSystem.GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(), collector,
                                                         {}, layerFilter, bodyFilter);
    if (!collector.HadHit())
        return std::nullopt;

    const JPH::Vec3 contact = collector.mHit.mContactPointOn2;
    const JPH::Vec3 axis    = collector.mHit.mPenetrationAxis.NormalizedOr(JPH::Vec3::sZero());

    QueryHit hit;
    hit.position = glm::vec3(contact.GetX(), contact.GetY(), contact.GetZ());
    // The penetration axis moves the hit body out of the sweep, so its opposite
    // points out of the struck surface — the same sense a ray's normal has.
    hit.normal   = -glm::vec3(axis.GetX(), axis.GetY(), axis.GetZ());
    hit.distance = std::sqrt(sweepLengthSq) * collector.mHit.mFraction;
    hit.entity   = _impl->EntityFor(collector.mHit.mBodyID2);
    return hit;
}

std::vector<ECS::Entity> PhysicsWorld::Overlap(const ColliderShapeDesc &shape, const Pose &at,
                                               CollisionFilter filter, ECS::Entity ignore) const
{
    const JPH::Quat rotation =
        JPH::Quat(at.rotation.x, at.rotation.y, at.rotation.z, at.rotation.w).Normalized();
    const JPH::RMat44 transform = JPH::RMat44::sRotationTranslation(
        rotation, JPH::RVec3(at.position.x, at.position.y, at.position.z));

    const JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    const FilterLayerFilter layerFilter{filter};
    const IgnoreBodyFilter  bodyFilter{_impl->BodyFor(ignore)};

    _impl->physicsSystem.GetNarrowPhaseQuery().CollideShape(
        MakeShape(shape), JPH::Vec3::sReplicate(1.f), transform, settings, JPH::RVec3::sZero(), collector,
        {}, layerFilter, bodyFilter);

    // One entry per entity, not per contact: a shape can meet another in several
    // places, and a caller asking what is inside a volume wants the things, not
    // the number of ways it touched them.
    std::vector<ECS::Entity> found;
    found.reserve(collector.mHits.size());
    for (const JPH::CollideShapeResult &result : collector.mHits)
    {
        const ECS::Entity entity = _impl->EntityFor(result.mBodyID2);
        if (entity == ECS::NullEntity)
            continue;
        if (std::find(found.begin(), found.end(), entity) == found.end())
            found.push_back(entity);
    }
    return found;
}

void PhysicsWorld::SetCollisionSteps(int32_t steps)
{
    _impl->collisionSteps = std::clamp(steps, 1, Impl::kMaxCollisionSteps);
}

int32_t PhysicsWorld::GetCollisionSteps() const
{
    return _impl->collisionSteps;
}

void PhysicsWorld::CaptureState()
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();

    for (const JPH::BodyID &id : _impl->dynamicBodyIds)
    {
        if (!bodies.IsAdded(id) || bodies.GetMotionType(id) == JPH::EMotionType::Static)
        {
            continue;
        }

        const auto it = _impl->snapshots.find(id.GetIndexAndSequenceNumber());
        if (it == _impl->snapshots.end())
        {
            continue;
        }

        const JPH::RVec3 pos = bodies.GetPosition(id);
        const JPH::Quat rot = bodies.GetRotation(id);

        // Retire the previous current, then record this step's pose as current.
        it->second.prevPosition = it->second.curPosition;
        it->second.prevRotation = it->second.curRotation;
        it->second.curPosition  = glm::vec3(pos.GetX(), pos.GetY(), pos.GetZ());
        it->second.curRotation  = glm::quat(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ());
    }
}

void PhysicsWorld::InterpolateTransforms(Assisi::ECS::Scene &scene, float alpha, const ParentWorldFn &parentWorld)
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();

    // Below these per-physics-step deltas a body is treated as at rest, so the pose
    // is snapped to the current step instead of blended (see the per-body use).
    constexpr float kRestPositionDeltaSq = 1e-8f; // (0.1 mm)^2 of translation between steps
    constexpr float kRestRotationDelta   = 1e-7f; // 1 - |dot(prev, cur)|; ~0.0009 rad between steps

    // QueryMut, not Query: Transform is ACOMP(tracked) and this is the physics
    // writeback, so the new pose has to stamp a change tick. PropagateTransforms's
    // dirty-skip and network delta replication both filter on that tick, and a
    // write through a plain Query's `Transform&` stamps nothing — the body would
    // move with both consumers still reporting it unchanged. The proxy stamps
    // exactly like Scene::GetMut.
    //
    // RigidBody comes along as a Mut proxy because QueryMut wraps every type, but
    // it is only read — through the const Get(), which never stamps (and RigidBody
    // is ACOMP(transient) and untracked anyway, so there is no tick lane to touch).
    for (auto [entity, transform, rb] :
         scene.QueryMut<Assisi::ECS::Transform, RigidBody>())
    {
        // Only a body the solver moves has a pose worth writing back. A static one
        // never moves, and a kinematic one is moved by whatever drives it — for
        // both, the Transform is the input, and writing to it would overwrite the
        // author or the driver with a snapshot CaptureState never refreshes.
        const JPH::BodyID bodyId = rb.Get().bodyId;
        if (!bodies.IsAdded(bodyId) || bodies.GetMotionType(bodyId) != JPH::EMotionType::Dynamic)
        {
            continue;
        }

        const auto it = _impl->snapshots.find(bodyId.GetIndexAndSequenceNumber());
        if (it == _impl->snapshots.end())
        {
            continue;
        }

        const Impl::MotionSnapshot &s = it->second;

        // A body settling toward sleep produces consecutive step poses that differ
        // by a hair; blending them with a per-frame-varying alpha makes the render
        // pose wobble (~0.001 rad). Below the rest deltas, snap to the current step
        // so it renders stable. Snapping still tracks a slow creep exactly (it
        // writes curPosition/curRotation every frame) — it only drops the blend.
        const glm::vec3 positionDelta = s.curPosition - s.prevPosition;
        glm::vec3 targetPosition = glm::dot(positionDelta, positionDelta) < kRestPositionDeltaSq
                                             ? s.curPosition
                                             : glm::mix(s.prevPosition, s.curPosition, alpha);

        // 1 - |dot(prev, cur)| is ~0 for near-identical orientations; abs folds the
        // quaternion q/-q double cover. slerp keeps angular speed constant across
        // the blend and is renormalised since the result feeds the render matrix.
        const float rotationDelta  = 1.f - glm::abs(glm::dot(s.prevRotation, s.curRotation));
        glm::quat targetRotation = rotationDelta < kRestRotationDelta
                                         ? s.curRotation
                                         : glm::normalize(glm::slerp(s.prevRotation, s.curRotation, alpha));

        // Jolt reports world space; a Transform under a parent is an offset *from*
        // that parent. Writing one into the other and letting PropagateTransforms
        // multiply by the parent again applies the parent twice — silently, and
        // once more every frame. Convert instead.
        if (parentWorld)
        {
            if (const glm::mat4 *parent = parentWorld(entity); parent != nullptr)
            {
                targetPosition = glm::vec3(glm::inverse(*parent) * glm::vec4(targetPosition, 1.f));
                targetRotation = glm::normalize(glm::inverse(ECS::WorldRotationOf(*parent)) * targetRotation);
            }
        }

        // Nothing moved: skip the write rather than stamp a change tick for a pose
        // identical to the one already there. Every mutable access through the
        // proxy stamps, so a resting body would otherwise read as changed every
        // frame for the rest of the session — dirty-subtree work for
        // PropagateTransforms, and bandwidth for a visual-only mirror, which has
        // no body channel and travels by Transform delta.
        //
        // Exact comparison rather than epsilon'd: a resting body's snapshot poses
        // are frozen, so the computed target is bit-identical frame to frame, and
        // the rest-snap branches above already absorbed the near-rest jitter.
        // Anything genuinely in motion differs in the low bits and is written.
        const Assisi::ECS::Transform &current = transform.Get();
        if (current.position == targetPosition && current.rotation == targetRotation)
            continue;

        // Taken once, after every skip: binding the reference costs one tick per
        // body that actually moves rather than one per field written.
        Assisi::ECS::Transform &t = transform.GetMut();
        t.position                = targetPosition;
        t.rotation                = targetRotation;
    }
}

void PhysicsWorld::GetActiveBodyStates(std::vector<ActiveBodyState> &out) const
{
    out.clear();

    JPH::BodyIDVector active;
    _impl->physicsSystem.GetActiveBodies(JPH::EBodyType::RigidBody, active);
    if (active.empty())
        return;

    const JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    out.reserve(active.size());
    for (const JPH::BodyID &id : active)
    {
        const ECS::Entity entity = _impl->EntityFor(id);
        if (entity == ECS::NullEntity)
            continue; // a raw AddBody body: nothing a caller could name it by

        const JPH::RVec3 position = bodies.GetPosition(id);
        const JPH::Quat rotation = bodies.GetRotation(id);
        const JPH::Vec3 linear   = bodies.GetLinearVelocity(id);
        const JPH::Vec3 angular  = bodies.GetAngularVelocity(id);

        out.push_back(ActiveBodyState{
                entity,
                glm::vec3(position.GetX(), position.GetY(), position.GetZ()),
                glm::quat(rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ()),
                glm::vec3(linear.GetX(), linear.GetY(), linear.GetZ()),
                glm::vec3(angular.GetX(), angular.GetY(), angular.GetZ()),
            });
    }
}

bool PhysicsWorld::IsBodyActive(const RigidBody &body) const
{
    const JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    return bodies.IsAdded(body.bodyId) && bodies.IsActive(body.bodyId);
}

void PhysicsWorld::DeactivateBody(const RigidBody &body)
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    if (!bodies.IsAdded(body.bodyId))
        return;
    bodies.DeactivateBody(body.bodyId);
}

void PhysicsWorld::ApplyBodyState(const RigidBody &body, glm::vec3 position, glm::quat rotation,
                                  glm::vec3 linearVelocity, glm::vec3 angularVelocity, bool activate)
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    if (!bodies.IsAdded(body.bodyId))
        return;

    // A static body can be *placed*, it just has no motion to place it with —
    // the same split SetBodyTransform makes. Refusing the whole call for one
    // would be a trap: a correction for a body the two ends disagree about the
    // motion type of would silently do nothing, which is the worst available
    // outcome for a peer that is trying to tell us where something is.
    const bool isStatic = bodies.GetMotionType(body.bodyId) == JPH::EMotionType::Static;

    // Normalized for the same reason AddBody and SetBodyTransform do it: a
    // quaternion that crossed a wire (or a level file) is often a hair off unit
    // length, and Jolt asserts IsNormalized() when it rotates with one.
    bodies.SetPositionAndRotation(body.bodyId, JPH::RVec3(position.x, position.y, position.z),
                                  JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w).Normalized(),
                                  (activate && !isStatic) ? JPH::EActivation::Activate
                                                          : JPH::EActivation::DontActivate);

    if (!isStatic)
    {
        // Before the deactivate below, not after: Jolt ignores velocity written
        // to a sleeping body, so zeroing an about-to-sleep body has to happen
        // while it is still awake.
        bodies.SetLinearVelocity(body.bodyId, JPH::Vec3(linearVelocity.x, linearVelocity.y, linearVelocity.z));
        bodies.SetAngularVelocity(body.bodyId, JPH::Vec3(angularVelocity.x, angularVelocity.y, angularVelocity.z));

        if (!activate)
            bodies.DeactivateBody(body.bodyId);
    }

    // Collapse both snapshots onto the corrected pose. Without this the next
    // InterpolateTransforms() blends from the pre-correction pose and smears the
    // jump across a frame — which the view-side error smoothing is *also* trying
    // to absorb, so the two double-count into a wobble at every correction.
    const auto it = _impl->snapshots.find(body.bodyId.GetIndexAndSequenceNumber());
    if (it != _impl->snapshots.end())
        it->second = Impl::MotionSnapshot{position, rotation, position, rotation};
}

std::pair<glm::vec3, glm::quat> PhysicsWorld::GetBodyTransform(const RigidBody &body) const
{
    const JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    const JPH::RVec3 pos = bodies.GetPosition(body.bodyId);
    const JPH::Quat rot = bodies.GetRotation(body.bodyId);
    return {glm::vec3(pos.GetX(), pos.GetY(), pos.GetZ()),
            glm::quat(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ())};
}

std::pair<glm::vec3, glm::vec3> PhysicsWorld::GetBodyVelocity(const RigidBody &body) const
{
    const JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();

    // Static bodies have no motion state; querying velocity on them is meaningless
    // (and GetLinearVelocity would just return zero anyway). Report zero for those
    // and for handles whose body isn't in the simulation.
    if (!bodies.IsAdded(body.bodyId) || bodies.GetMotionType(body.bodyId) == JPH::EMotionType::Static)
    {
        return {glm::vec3(0.f), glm::vec3(0.f)};
    }

    const JPH::Vec3 lin = bodies.GetLinearVelocity(body.bodyId);
    const JPH::Vec3 ang = bodies.GetAngularVelocity(body.bodyId);
    return {glm::vec3(lin.GetX(), lin.GetY(), lin.GetZ()),
            glm::vec3(ang.GetX(), ang.GetY(), ang.GetZ())};
}

bool PhysicsWorld::IsBodyCCDEnabled(const RigidBody &body) const
{
    const JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    if (!bodies.IsAdded(body.bodyId))
    {
        return false;
    }
    return bodies.GetMotionQuality(body.bodyId) == JPH::EMotionQuality::LinearCast;
}

void PhysicsWorld::SetBodyTransform(const RigidBody &body, glm::vec3 position, glm::quat rotation)
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    if (!bodies.IsAdded(body.bodyId))
        return;

    const bool isStatic = bodies.GetMotionType(body.bodyId) == JPH::EMotionType::Static;

    // A static sensor only hears about bodies that are awake, so the space it is
    // leaving has to be woken as well as the space it is arriving in: whatever it
    // was containing must be re-tested to notice the pair ended, and whatever it
    // lands on must be re-tested to notice the pair began. Captured before the
    // move, used after it.
    const bool wakesAround = isStatic && IsTriggerLayer(bodies.GetObjectLayer(body.bodyId));
    JPH::AABox touched;
    if (wakesAround)
    {
        JPH::BodyLockRead lock(_impl->physicsSystem.GetBodyLockInterface(), body.bodyId);
        if (lock.Succeeded())
            touched = lock.GetBody().GetWorldSpaceBounds();
    }

    // Normalize before handing the quaternion to Jolt: a hand-authored or imported
    // rotation is often a hair off unit length (e.g. a level's [0.707, 0.707, 0, 0]
    // has length^2 0.9997), and Jolt asserts IsNormalized() when it rotates with it.
    // AddBody normalizes for the same reason.
    bodies.SetPositionAndRotation(body.bodyId, JPH::RVec3(position.x, position.y, position.z),
                                  JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w).Normalized(),
                                  isStatic ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);

    // Velocity is only meaningful for dynamic bodies; static bodies have no active motion.
    if (!isStatic)
    {
        bodies.SetLinearVelocity(body.bodyId, JPH::Vec3::sZero());
        bodies.SetAngularVelocity(body.bodyId, JPH::Vec3::sZero());
    }

    // Collapse both snapshots onto the teleport target. Without this the next
    // InterpolateTransforms() would blend from the pre-teleport pose and slide
    // the body across the gap over one frame instead of snapping to it.
    const auto it = _impl->snapshots.find(body.bodyId.GetIndexAndSequenceNumber());
    if (it != _impl->snapshots.end())
    {
        it->second = Impl::MotionSnapshot{position, rotation, position, rotation};
    }

    if (wakesAround)
    {
        JPH::ObjectLayer layer = 0;
        {
            JPH::BodyLockRead lock(_impl->physicsSystem.GetBodyLockInterface(), body.bodyId);
            if (!lock.Succeeded())
                return;
            touched.Encapsulate(lock.GetBody().GetWorldSpaceBounds());
            layer = lock.GetBody().GetObjectLayer();
        }

        // Outside the lock: waking takes its own body locks, and everything read
        // above came from the locked body rather than through an interface that
        // would have taken the same one again.
        _impl->WakeInside(touched,
                          CollisionFilter{MaskOf(layer), static_cast<CollisionChannel>(ChannelOf(layer))});
    }
}

void PhysicsWorld::SetBodyLinearVelocity(const RigidBody &body, glm::vec3 velocity)
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    if (!bodies.IsAdded(body.bodyId) || bodies.GetMotionType(body.bodyId) == JPH::EMotionType::Static)
        return;

    // Activate first, then set: a body Jolt has put to sleep on a surface ignores
    // velocity written while it is asleep, which reads as the call silently doing
    // nothing — exactly the case a contact response hits, since landing is what
    // puts a body to sleep in the first place.
    bodies.ActivateBody(body.bodyId);
    bodies.SetLinearVelocity(body.bodyId, JPH::Vec3(velocity.x, velocity.y, velocity.z));
}

void PhysicsWorld::SetBodyCollisionFilter(const RigidBody &body, CollisionFilter filter)
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    if (!bodies.IsAdded(body.bodyId))
        return;

    const bool sensor = filter.channel == CollisionChannel::Trigger;

    // The motion type rides in the layer beside the channel, so it has to be
    // carried across rather than defaulted — repacking without it would quietly
    // move the body to another broad-phase tree.
    BodyMotion motion = MotionOf(bodies.GetObjectLayer(body.bodyId));
    if (sensor && motion == BodyMotion::Dynamic)
        motion = BodyMotion::Kinematic;

    bodies.SetObjectLayer(body.bodyId, PackLayer(filter, motion));

    if (motion != BodyMotion::Static)
        bodies.ActivateBody(body.bodyId);

    // Sensor-ness is a body flag rather than part of the layer, and Jolt exposes
    // no interface-level setter for it.
    JPH::BodyLockWrite lock(_impl->physicsSystem.GetBodyLockInterface(), body.bodyId);
    if (lock.Succeeded())
        lock.GetBody().SetIsSensor(sensor);
}

CollisionFilter PhysicsWorld::GetBodyCollisionFilter(const RigidBody &body) const
{
    if (!_impl->physicsSystem.GetBodyInterface().IsAdded(body.bodyId))
        return CollisionFilter{};
    return _impl->FilterOf(body.bodyId);
}

void PhysicsWorld::ReshapeBody(const RigidBody &body, const ColliderShapeDesc &shape)
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    if (!bodies.IsAdded(body.bodyId))
        return;

    bodies.SetShape(body.bodyId, MakeShape(shape), /*inUpdateMassProperties=*/ true,
                    JPH::EActivation::DontActivate);
}

void PhysicsWorld::SetBodyCCD(const RigidBody &body, bool enable)
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    if (!bodies.IsAdded(body.bodyId))
        return;

    // Set motion quality even when the body is currently Static, rather than
    // guarding on Dynamic: the inspector freezes the selected body to Static while
    // a widget is active, so a guard would silently drop the CCD checkbox. Motion
    // quality is a stored property (our bodies always have motion properties, since
    // AddBody sets mAllowDynamicOrKinematic), so it sticks and takes effect once
    // the body is Dynamic again. Jolt no-ops safely if a body genuinely has none.
    const JPH::EMotionQuality quality =
        enable ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
    bodies.SetMotionQuality(body.bodyId, quality);
}

void PhysicsWorld::SetBodyMotionType(const RigidBody &body, BodyMotion motion)
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    if (!bodies.IsAdded(body.bodyId))
        return;

    const CollisionFilter filter = _impl->FilterOf(body.bodyId);

    // Same rule AddBody applies: a sensor is never dynamic, because one falling
    // under gravity would leave the volume it was authored as.
    const BodyMotion effective = filter.channel == CollisionChannel::Trigger &&
                                 motion == BodyMotion::Dynamic
                                     ? BodyMotion::Kinematic
                                     : motion;

    auto &ids = _impl->dynamicBodyIds;
    if (effective == BodyMotion::Static)
    {
        // Jolt asserts that a body is inactive before switching it to Static.
        bodies.DeactivateBody(body.bodyId);
        bodies.SetMotionType(body.bodyId, JPH::EMotionType::Static, JPH::EActivation::DontActivate);
        ids.erase(std::remove(ids.begin(), ids.end(), body.bodyId), ids.end());
    }
    else if (effective == BodyMotion::Kinematic)
    {
        bodies.SetMotionType(body.bodyId, JPH::EMotionType::Kinematic, JPH::EActivation::Activate);
        ids.erase(std::remove(ids.begin(), ids.end(), body.bodyId), ids.end());
    }
    else
    {
        bodies.SetMotionType(body.bodyId, JPH::EMotionType::Dynamic, JPH::EActivation::Activate);
        if (std::find(ids.begin(), ids.end(), body.bodyId) == ids.end())
            ids.push_back(body.bodyId);
    }

    // The layer records the motion type, and the broad-phase tree a body lives in
    // is read from it. Left stale, a body made dynamic would keep saying it never
    // moves and would never be tested against the things it now falls onto.
    bodies.SetObjectLayer(body.bodyId, PackLayer(filter, effective));
}

void PhysicsWorld::SetGravity(glm::vec3 gravity)
{
    _impl->physicsSystem.SetGravity(JPH::Vec3(gravity.x, gravity.y, gravity.z));

    /* Wake all dynamic bodies so they respond to the new gravity immediately. */
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    for (const JPH::BodyID &id : _impl->dynamicBodyIds)
    {
        if (bodies.IsAdded(id))
        {
            bodies.ActivateBody(id);
        }
    }
}

glm::vec3 PhysicsWorld::GetGravity() const
{
    const JPH::Vec3 g = _impl->physicsSystem.GetGravity();
    return glm::vec3(g.GetX(), g.GetY(), g.GetZ());
}

JoltAllocationStats GetJoltAllocationStats()
{
    JoltAllocationStats stats;
    stats.count = gJoltAllocCount.load(std::memory_order_relaxed);
    stats.bytes = gJoltAllocBytes.load(std::memory_order_relaxed);
    return stats;
}

} // namespace Assisi::Physics