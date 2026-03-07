/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Physics/PhysicsWorld.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Runtime/Components.hpp>

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <thread>
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
    static constexpr unsigned int kMaxBodies = 1024;
    static constexpr unsigned int kMaxBodyPairs = 65536;
    static constexpr unsigned int kMaxContactConstraints = 10240;

    BPLayerInterface bpLayerInterface;
    ObjVsBPFilter objVsBPFilter;
    ObjLayerFilter objLayerFilter;

    JPH::TempAllocatorImpl tempAlloc{10u * 1024u * 1024u}; // 10 MiB
    JPH::JobSystemThreadPool jobSystem{JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                                       static_cast<int>(std::thread::hardware_concurrency()) - 1};
    JPH::PhysicsSystem physicsSystem;

    std::vector<JPH::BodyID> allBodyIds;     ///< Every body ever added; used by Clear().
    std::vector<JPH::BodyID> dynamicBodyIds; ///< Subset of allBodyIds; used to wake on gravity change.
};

// ---------------------------------------------------------------------------
// PhysicsWorld
// ---------------------------------------------------------------------------

PhysicsWorld::PhysicsWorld()
{
    /* Must be called before any Jolt allocations (including Impl member ctors). */
    JPH::RegisterDefaultAllocator();

    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    /* Only now safe to construct TempAllocatorImpl and JobSystemThreadPool. */
    _impl = std::make_unique<Impl>();

    _impl->physicsSystem.Init(Impl::kMaxBodies, 0u, Impl::kMaxBodyPairs, Impl::kMaxContactConstraints,
                              _impl->bpLayerInterface, _impl->objVsBPFilter, _impl->objLayerFilter);

    /* Gravity: 9.81 m/s² downward (−Y). */
    _impl->physicsSystem.SetGravity(JPH::Vec3(0.f, -9.81f, 0.f));

    Assisi::Core::Log::Info("PhysicsWorld: initialized (Jolt).");
}

PhysicsWorld::~PhysicsWorld()
{
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

RigidBodyComponent PhysicsWorld::AddBox(glm::vec3 position, glm::quat rotation, glm::vec3 halfExtents,
                                        BodyMotion motion)
{
    const JPH::EMotionType joltMotion =
        motion == BodyMotion::Static ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic;

    const JPH::ObjectLayer layer = motion == BodyMotion::Static ? Layers::kStatic : Layers::kDynamic;

    JPH::BodyCreationSettings settings(
        new JPH::BoxShape(JPH::Vec3(halfExtents.x, halfExtents.y, halfExtents.z)),
        JPH::RVec3(position.x, position.y, position.z),
        JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w).Normalized(), joltMotion, layer);

    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    const JPH::BodyID bodyId = bodies.CreateAndAddBody(settings, JPH::EActivation::Activate);

    _impl->allBodyIds.push_back(bodyId);
    if (motion == BodyMotion::Dynamic)
        _impl->dynamicBodyIds.push_back(bodyId);

    return RigidBodyComponent{bodyId};
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
}

void PhysicsWorld::Update(float deltaTime)
{
    constexpr int kCollisionSteps = 1;
    _impl->physicsSystem.Update(deltaTime, kCollisionSteps, &_impl->tempAlloc, &_impl->jobSystem);
}

void PhysicsWorld::SyncTransforms(Assisi::ECS::Scene &scene)
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();

    for (auto [entity, transform, rb] :
         scene.Query<Assisi::Runtime::TransformComponent, RigidBodyComponent>())
    {
        if (!bodies.IsAdded(rb.bodyId) || bodies.GetMotionType(rb.bodyId) == JPH::EMotionType::Static)
        {
            continue;
        }

        const JPH::RVec3 pos = bodies.GetPosition(rb.bodyId);
        const JPH::Quat rot = bodies.GetRotation(rb.bodyId);

        transform.position = glm::vec3(pos.GetX(), pos.GetY(), pos.GetZ());
        transform.rotation = glm::quat(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ());
    }
}

std::pair<glm::vec3, glm::quat> PhysicsWorld::GetBodyTransform(const RigidBodyComponent &body) const
{
    const JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    const JPH::RVec3 pos = bodies.GetPosition(body.bodyId);
    const JPH::Quat rot = bodies.GetRotation(body.bodyId);
    return {glm::vec3(pos.GetX(), pos.GetY(), pos.GetZ()),
            glm::quat(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ())};
}

void PhysicsWorld::SetBodyTransform(const RigidBodyComponent &body, glm::vec3 position, glm::quat rotation)
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    bodies.SetPositionAndRotation(body.bodyId, JPH::RVec3(position.x, position.y, position.z),
                                  JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w),
                                  JPH::EActivation::Activate);
    bodies.SetLinearVelocity(body.bodyId, JPH::Vec3::sZero());
    bodies.SetAngularVelocity(body.bodyId, JPH::Vec3::sZero());
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