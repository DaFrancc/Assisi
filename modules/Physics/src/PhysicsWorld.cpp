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
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Object / broad-phase layers
// ---------------------------------------------------------------------------

namespace
{

namespace Layers
{
static constexpr JPH::ObjectLayer kStatic = 0;
static constexpr JPH::ObjectLayer kDynamic = 1;
// (no kCount here: the broad-phase layer count is BPLayers::kCount, which Jolt
// actually queries; an object-layer count had no reader.)
} // namespace Layers

namespace BPLayers
{
static constexpr JPH::BroadPhaseLayer kStatic(0);
static constexpr JPH::BroadPhaseLayer kDynamic(1);
static constexpr unsigned int kCount = 2;
} // namespace BPLayers

// Maps object layers → broad-phase layers.
class BPLayerInterface final : public JPH::BroadPhaseLayerInterface
{
public:
    unsigned int GetNumBroadPhaseLayers() const override { return BPLayers::kCount; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
    {
        return layer == Layers::kStatic ? BPLayers::kStatic : BPLayers::kDynamic;
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
    {
        return layer == BPLayers::kStatic ? "Static" : "Dynamic";
    }
#endif
};

// Decides whether an object layer should be tested against a broad-phase layer.
class ObjVsBPFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bpLayer) const override
    {
        switch (layer)
        {
        case Layers::kStatic:
            return bpLayer == BPLayers::kDynamic;
        case Layers::kDynamic:
            return true;
        default:
            return false;
        }
    }
};

// Decides whether two object layers should collide at all.
class ObjLayerFilter final : public JPH::ObjectLayerPairFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer layerA, JPH::ObjectLayer layerB) const override
    {
        switch (layerA)
        {
        case Layers::kStatic:
            return layerB == Layers::kDynamic;
        case Layers::kDynamic:
            return true;
        default:
            return false;
        }
    }
};

// ---------------------------------------------------------------------------
// Shared Jolt runtime
// ---------------------------------------------------------------------------

/* The Jolt state that is genuinely process-global and worth sharing across every
   PhysicsWorld: the library globals (allocator, Factory, type registration) and
   the job-system thread pool. Multi-scene runs several PhysicsWorlds side by side
   (docs/multi-scene-design-notes.md §1); a pool per world would spawn
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
   their frames. Sharing it was only ever "safe" while worlds stepped strictly
   sequentially, and even then the accesses cross pool-worker threads without a
   happens-before edge (a real data race ThreadSanitizer flags). A per-world
   allocator is 10 MiB of scratch each — cheap — and makes stepping safe whether
   worlds run sequentially or (later) in parallel. The pool stays shared; Jolt is
   built for many PhysicsSystems on one JobSystem. */
/* Under ThreadSanitizer the pool is replaced by Jolt's single-threaded job
   system. Jolt's solver coordinates its workers through its own barriers and
   atomics rather than through anything tsan models as a happens-before edge, so
   a threaded step reports races inside `JobSystem.h` and `TempAllocator.h` —
   Jolt's own headers, which reach the instrumentation only because they are
   headers inlined into this TU (the Jolt library itself does not link
   Assisi::Sanitize). Those reports are not ours and cannot be fixed here, and
   left alone they bury any real race in noise: four suites go red for reasons
   nobody can act on, which is the same as having no tsan gate at all.

   Stepping on one thread removes them at the source rather than hiding them
   behind a suppression. It costs nothing that matters — Jolt's results do not
   depend on worker count, so a sanitized run exercises the same physics, just
   more slowly, which a sanitized build is anyway. Everything *around* physics
   still runs threaded, so the async-travel worker, the job system and the
   blueprint cache are all still under test. Speed is not a question a sanitized
   build answers (see Chiara's TestOverhead for the same reasoning). */
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

    // --- Contact reporting (off unless SetContactReporting turns it on) -------

    /// The entity behind each body, keyed like `snapshots`. Only bodies created
    /// through AddBodyFromDescriptor appear — it is the one entry point that knows
    /// an entity — so a contact against a body from the raw AddBody reports
    /// NullEntity for that side rather than a wrong handle.
    std::unordered_map<JPH::uint32, ECS::Entity> bodyEntities;

    bool contactReporting = false;

    /// Written from Jolt's worker jobs during Update(), read from the main thread
    /// between steps. The mutex only guards the append: contacts are rare relative
    /// to the collision work that produced them, so this never becomes the
    /// bottleneck, and per-thread buffers would cost more to merge than they save.
    std::mutex contactMutex;
    std::vector<Contact> contacts;

    ECS::Entity EntityFor(const JPH::BodyID &id) const
    {
        const auto it = bodyEntities.find(id.GetIndexAndSequenceNumber());
        return it == bodyEntities.end() ? ECS::NullEntity : it->second;
    }

    /// Records both sides of a contact. Called from Jolt's narrow phase — i.e.
    /// *before* the solver runs, which is the whole reason the velocities are
    /// captured here rather than read back afterwards.
    void RecordContact(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold);

    /// Installed as the PhysicsSystem's contact listener only while reporting is
    /// on, so a world that does not want contacts never even pays the virtual call.
    class ContactCollector final : public JPH::ContactListener
    {
public:
        explicit ContactCollector(Impl &owner) : _owner(owner) {}

        void OnContactAdded(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold,
                            JPH::ContactSettings &settings) override
        {
            (void)settings; // we observe contacts, we don't retune them
            _owner.RecordContact(body1, body2, manifold);
        }

        // OnContactPersisted is deliberately not overridden. A body resting on a
        // surface persists its contact every step; reporting those would make a
        // contact-driven response (a bounce) re-fire forever into something that
        // is simply lying still.

private:
        Impl &_owner;
    };

    ContactCollector collector{*this};
};

void PhysicsWorld::Impl::RecordContact(const JPH::Body &body1, const JPH::Body &body2,
                                       const JPH::ContactManifold &manifold)
{
    const ECS::Entity e1 = EntityFor(body1.GetID());
    const ECS::Entity e2 = EntityFor(body2.GetID());
    if (e1 == ECS::NullEntity && e2 == ECS::NullEntity)
        return; // nothing on either side a system could act on

    // Jolt's manifold normal is the direction body2 must move to separate from
    // body1, so it already points away from body1's surface. Each side gets the
    // one that points away from the *other*, which is what a reflection wants.
    const JPH::Vec3 n = manifold.mWorldSpaceNormal;
    const glm::vec3 awayFromBody1{n.GetX(), n.GetY(), n.GetZ()};

    // Body::GetLinearVelocity asserts on a static body (no motion state to read).
    const auto linearVelocity = [](const JPH::Body &body)
                                {
                                    if (body.IsStatic())
                                        return glm::vec3(0.f);
                                    const JPH::Vec3 v = body.GetLinearVelocity();
                                    return glm::vec3(v.GetX(), v.GetY(), v.GetZ());
                                };

    const std::lock_guard<std::mutex> lock(contactMutex);
    if (e1 != ECS::NullEntity)
        contacts.push_back(Contact{e1, e2, -awayFromBody1, linearVelocity(body1)});
    if (e2 != ECS::NullEntity)
        contacts.push_back(Contact{e2, e1, awayFromBody1, linearVelocity(body2)});
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

    Assisi::Core::Log::Info("PhysicsWorld: initialized (Jolt).");
}

PhysicsWorld::~PhysicsWorld()
{
    /* Impl's members are destroyed in reverse declaration order, so the shared
       runtime handle (its first member) is released after this world's
       PhysicsSystem and bodies are gone. */
    _impl.reset();
}

RigidBody PhysicsWorld::AddBody(glm::vec3 position, glm::quat rotation, const ColliderShapeDesc &shape,
                                BodyMotion motion)
{
    const JPH::EMotionType joltMotion =
        motion == BodyMotion::Static ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic;

    const JPH::ObjectLayer layer = motion == BodyMotion::Static ? Layers::kStatic : Layers::kDynamic;

    JPH::BodyCreationSettings settings(
        MakeShape(shape), JPH::RVec3(position.x, position.y, position.z),
        JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w).Normalized(), joltMotion, layer);

    // Always allocate motion properties so the motion type can be changed at runtime
    // (e.g. making a Static body Dynamic via SetBodyMotionType).
    settings.mAllowDynamicOrKinematic = true;

    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    const JPH::BodyID bodyId = bodies.CreateAndAddBody(settings, JPH::EActivation::Activate);
    if (bodyId.IsInvalid())
    {
        Assisi::Core::Log::Error(
            "PhysicsWorld: failed to create body (body limit of {} reached?); entity will not simulate.",
            Impl::kMaxBodies);
        return RigidBody{bodyId};
    }

    _impl->allBodyIds.push_back(bodyId);
    if (motion == BodyMotion::Dynamic)
        _impl->dynamicBodyIds.push_back(bodyId);

    // Seed both snapshots with the spawn pose so the first interpolated frame
    // (before any step has run) resolves to exactly where the body was placed.
    _impl->snapshots[bodyId.GetIndexAndSequenceNumber()] =
        Impl::MotionSnapshot{position, rotation, position, rotation};

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

    const RigidBody body = AddBody(position, rotation, shape, motion);
    if (descriptor.enableCCD)
        SetBodyCCD(body, true);
    (void)scene.Add<RigidBody>(entity, body);

    // The only body-creation path that knows an entity, so the only one that can
    // make a contact nameable in ECS terms. Recorded unconditionally: reporting can
    // be switched on later in the world's life, and rebuilding the map then would
    // mean walking the scene.
    if (!body.bodyId.IsInvalid())
        _impl->bodyEntities[body.bodyId.GetIndexAndSequenceNumber()] = entity;

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
    // Logged contacts name bodies that no longer exist — and, after a level load,
    // entity handles that mean something entirely different.
    _impl->contacts.clear();
}

void PhysicsWorld::RemoveBody(const RigidBody &body)
{
    const JPH::BodyID id = body.bodyId;
    if (id.IsInvalid())
        return;

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

    // Drop any logged contact naming the entity whose body just went away, on
    // either side — acting on one would look up a RigidBody component pointing at
    // a destroyed Jolt body.
    const ECS::Entity gone = _impl->EntityFor(id);
    _impl->bodyEntities.erase(key);
    if (gone != ECS::NullEntity)
    {
        std::erase_if(_impl->contacts, [gone](const Contact &contact)
                      { return contact.entity == gone || contact.other == gone; });
    }
}

void PhysicsWorld::Update(float deltaTime)
{
    // The log describes the step about to run, not the one before it — clearing
    // here is what guarantees a consumer sees each impact exactly once. Safe
    // without the mutex: no Jolt worker is inside a callback at this point.
    _impl->contacts.clear();

    /* This world's own scratch allocator, and the shared thread pool. Both are
       Update() arguments; the pool is shared (one set of workers), the allocator
       is per-world so two worlds' steps never touch the same scratch stack (see
       JoltRuntime). */
    _impl->physicsSystem.Update(deltaTime, _impl->collisionSteps, &_impl->tempAlloc,
                                &_impl->jolt.JobSystem());
}

void PhysicsWorld::SetContactReporting(bool enable)
{
    if (_impl->contactReporting == enable)
        return;

    _impl->contactReporting = enable;

    // Unhooking the listener rather than early-returning inside it is what makes
    // "off" genuinely free: Jolt skips the call entirely instead of making a
    // virtual call per contact to reach a branch that does nothing.
    _impl->physicsSystem.SetContactListener(enable ? &_impl->collector : nullptr);

    if (!enable)
        _impl->contacts.clear();
}

bool PhysicsWorld::IsContactReporting() const
{
    return _impl->contactReporting;
}

std::span<const Contact> PhysicsWorld::Contacts() const
{
    return {_impl->contacts.data(), _impl->contacts.size()};
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
    // exactly like Scene::GetMut, which is what the old explicit MarkChanged-by-id
    // call here was standing in for.
    //
    // RigidBody comes along as a Mut proxy because QueryMut wraps every type, but
    // it is only read — through the const Get(), which never stamps (and RigidBody
    // is ACOMP(transient) and untracked anyway, so there is no tick lane to touch).
    for (auto [entity, transform, rb] :
         scene.QueryMut<Assisi::ECS::Transform, RigidBody>())
    {
        const JPH::BodyID bodyId = rb.Get().bodyId;
        if (!bodies.IsAdded(bodyId) || bodies.GetMotionType(bodyId) == JPH::EMotionType::Static)
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

        // Nothing moved: skip the write entirely rather than stamp a change tick
        // for a pose identical to the one already there.
        //
        // Every mutable access through the proxy stamps, and a resting body would
        // otherwise be marked changed on every single frame for the rest of the
        // session — a permanent false positive that PropagateTransforms pays for
        // in dirty-subtree work and that replication pays for in bandwidth, since
        // a visual-only mirror (one whose descriptor its entity declines to send)
        // has no body channel and travels by Transform delta.
        //
        // Exact comparison is right here rather than epsilon'd: a resting body's
        // snapshot poses are frozen — nothing integrates them — so the computed
        // target is bit-identical frame to frame, and the rest-snap branches above
        // already absorbed the near-rest jitter that would otherwise need a
        // tolerance. Anything genuinely in motion differs in the low bits and is
        // written.
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

    const bool isStatic = bodies.GetMotionType(body.bodyId) == JPH::EMotionType::Static;

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

    // Set motion quality even when the body is currently Static. Motion quality
    // is a stored property (our bodies always have motion properties, since
    // AddBody sets mAllowDynamicOrKinematic), so it sticks and takes effect once
    // the body is Dynamic again. Guarding on Dynamic here used to make this a
    // silent no-op: the inspector freezes the selected body to Static while a
    // widget is active, so the CCD checkbox toggled on a frozen body and never
    // applied. Jolt no-ops safely if a body genuinely has no motion properties.
    const JPH::EMotionQuality quality =
        enable ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
    bodies.SetMotionQuality(body.bodyId, quality);
}

void PhysicsWorld::SetBodyMotionType(const RigidBody &body, BodyMotion motion)
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();

    if (motion == BodyMotion::Static)
    {
        // Jolt asserts that a body is inactive before switching it to Static.
        bodies.DeactivateBody(body.bodyId);
        bodies.SetMotionType(body.bodyId, JPH::EMotionType::Static, JPH::EActivation::DontActivate);

        auto &ids = _impl->dynamicBodyIds;
        ids.erase(std::remove(ids.begin(), ids.end(), body.bodyId), ids.end());
    }
    else
    {
        bodies.SetMotionType(body.bodyId, JPH::EMotionType::Dynamic, JPH::EActivation::Activate);

        auto &ids = _impl->dynamicBodyIds;
        if (std::find(ids.begin(), ids.end(), body.bodyId) == ids.end())
            ids.push_back(body.bodyId);
    }
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