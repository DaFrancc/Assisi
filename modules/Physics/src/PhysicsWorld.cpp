/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Physics/PhysicsWorld.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Transform.hpp>

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
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
static constexpr JPH::ObjectLayer kCount = 2;
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

} // anonymous namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

namespace Assisi::Physics
{

struct PhysicsWorld::Impl
{
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

    JPH::TempAllocatorImpl tempAlloc{10u * 1024u * 1024u}; // 10 MiB
    JPH::JobSystemThreadPool jobSystem{JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                                       static_cast<int>(std::thread::hardware_concurrency()) - 1};
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
};

// ---------------------------------------------------------------------------
// PhysicsWorld
// ---------------------------------------------------------------------------

namespace
{
/* Jolt's allocator, factory, and type registration are process-global, not
   per-PhysicsSystem. Refcount them so multiple PhysicsWorld instances share a
   single init/teardown instead of leaking the factory or tearing it out from
   under a sibling instance. Atomic so worlds constructed/destroyed on
   different threads can't lose a count and double-free the factory. */
std::atomic<int32_t> gJoltRefCount{0};

void AcquireJoltGlobals()
{
    if (gJoltRefCount++ == 0)
    {
        /* Must be called before any Jolt allocations (including Impl member ctors). */
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
}

void ReleaseJoltGlobals()
{
    if (--gJoltRefCount == 0)
    {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
}

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
    AcquireJoltGlobals();

    /* Only now safe to construct TempAllocatorImpl and JobSystemThreadPool. */
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
    /* Tear down this instance's PhysicsSystem before releasing the shared Jolt
       globals it depends on (factory/type registration). */
    _impl.reset();
    ReleaseJoltGlobals();
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
}

void PhysicsWorld::Update(float deltaTime)
{
    _impl->physicsSystem.Update(deltaTime, _impl->collisionSteps, &_impl->tempAlloc, &_impl->jobSystem);
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
        const JPH::Quat  rot = bodies.GetRotation(id);

        // Retire the previous current, then record this step's pose as current.
        it->second.prevPosition = it->second.curPosition;
        it->second.prevRotation = it->second.curRotation;
        it->second.curPosition  = glm::vec3(pos.GetX(), pos.GetY(), pos.GetZ());
        it->second.curRotation  = glm::quat(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ());
    }
}

void PhysicsWorld::InterpolateTransforms(Assisi::ECS::Scene &scene, float alpha)
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();

    // Transform is ACOMP(tracked): writing it through the query reference bypasses
    // Scene::GetMut's stamping, so each moved body must report the change itself,
    // or PropagateTransforms would not see the new pose. Resolve the id once.
    const Assisi::Core::Reflect::ComponentId transformId =
        Assisi::Core::Reflect::ComponentIdOf<Assisi::ECS::Transform>();

    // Below these per-physics-step deltas a body is treated as at rest, so the pose
    // is snapped to the current step instead of blended (see the per-body use).
    constexpr float kRestPositionDeltaSq = 1e-8f; // (0.1 mm)^2 of translation between steps
    constexpr float kRestRotationDelta   = 1e-7f; // 1 - |dot(prev, cur)|; ~0.0009 rad between steps

    for (auto [entity, transform, rb] :
         scene.Query<Assisi::ECS::Transform, RigidBody>())
    {
        if (!bodies.IsAdded(rb.bodyId) || bodies.GetMotionType(rb.bodyId) == JPH::EMotionType::Static)
        {
            continue;
        }

        const auto it = _impl->snapshots.find(rb.bodyId.GetIndexAndSequenceNumber());
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
        transform.position = glm::dot(positionDelta, positionDelta) < kRestPositionDeltaSq
                                 ? s.curPosition
                                 : glm::mix(s.prevPosition, s.curPosition, alpha);

        // 1 - |dot(prev, cur)| is ~0 for near-identical orientations; abs folds the
        // quaternion q/-q double cover. slerp keeps angular speed constant across
        // the blend and is renormalised since the result feeds the render matrix.
        const float rotationDelta = 1.f - glm::abs(glm::dot(s.prevRotation, s.curRotation));
        transform.rotation = rotationDelta < kRestRotationDelta
                                 ? s.curRotation
                                 : glm::normalize(glm::slerp(s.prevRotation, s.curRotation, alpha));

        scene.MarkChanged(entity, transformId);
    }
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

void PhysicsWorld::ReshapeBody(const RigidBody &body, const ColliderShapeDesc &shape)
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    if (!bodies.IsAdded(body.bodyId))
        return;

    bodies.SetShape(body.bodyId, MakeShape(shape), /*inUpdateMassProperties=*/true,
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

} // namespace Assisi::Physics