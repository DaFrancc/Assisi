/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file PhysicsWorld.cpp
/// @brief The world itself: bringing it up, stepping it, and the rigid bodies in
///        it — creating, destroying, and editing one.
///
/// The rest of PhysicsWorld lives beside this file rather than in it. Contacts
/// are in PhysicsContacts.cpp, scene queries in PhysicsQueries.cpp, characters in
/// PhysicsCharacters.cpp, and the render/replication writeback in
/// PhysicsWriteback.cpp; what they share is PhysicsInternal.hpp.

#include "PhysicsInternal.hpp"

#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/TransformPose.hpp>

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace Assisi::Physics
{

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
} // namespace

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
        return RigidBody{FromJolt(bodyId)};
    }

    _impl->allBodyIds.push_back(bodyId);
    if (effective != BodyMotion::Static)
    {
        _impl->movingBodyIds.push_back(bodyId);
    }

    // Seed both snapshots with the spawn pose so the first interpolated frame
    // (before any step has run) resolves to exactly where the body was placed.
    _impl->snapshots[bodyId.GetIndexAndSequenceNumber()] =
        Impl::MotionSnapshot{pose.position, pose.rotation, pose.position, pose.rotation};

    // A static sensor is told about bodies that touch it, and a sleeping body
    // touches nothing. Waking whatever it now encloses is what lets it report the
    // things that were already sitting there when it appeared.
    if (sensor && effective == BodyMotion::Static)
        _impl->WakeInside(bodyId);

    return RigidBody{FromJolt(bodyId)};
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
    if (!ToJolt(body.bodyId).IsInvalid())
    {
        _impl->bodyEntities[ToJolt(body.bodyId).GetIndexAndSequenceNumber()] = entity;
        _impl->entityBodies[entity]                                 = ToJolt(body.bodyId);
    }

    return body;
}

void PhysicsWorld::RebuildSceneBodies(ECS::Scene &scene, const ParentWorldFn &parentWorld)
{
    Clear();

    // One entity is built once however many descriptors it has, which is what
    // makes carrying both an error the entity-keyed path can report rather than
    // two objects fighting each other.
    for (auto [entity, transform, descriptor] : scene.Query<ECS::Transform, RigidBodyDescriptor>())
    {
        (void)transform;
        (void)descriptor;
        if (!RebuildEntityPhysics(scene, entity, parentWorld))
        {
            Assisi::Core::Log::Error("PhysicsWorld: entity {} has no usable physics.", entity.index);
        }
    }

    for (auto [entity, transform, descriptor] : scene.Query<ECS::Transform, CharacterDescriptor>())
    {
        (void)transform;
        (void)descriptor;
        if (scene.Get<Character>(entity) != nullptr)
        {
            continue; // already built above, or refused there for carrying both
        }
        if (!RebuildEntityPhysics(scene, entity, parentWorld))
        {
            Assisi::Core::Log::Error("PhysicsWorld: entity {} has no usable physics.", entity.index);
        }
    }
}

void PhysicsWorld::Clear()
{
    // Characters first: each owns an inner body that it destroys itself, and one
    // outliving the body set would be destroyed after the bodies it points into.
    _impl->entityCharacters.clear();
    for (auto &[id, record] : _impl->characters)
    {
        (void)id;
        _impl->characterVsCharacter.Remove(record.character);
    }
    _impl->characters.clear();

    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    for (const JPH::BodyID &id : _impl->allBodyIds)
    {
        if (bodies.IsAdded(id))
            bodies.RemoveBody(id);
        bodies.DestroyBody(id);
    }
    _impl->allBodyIds.clear();
    _impl->movingBodyIds.clear();
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
    const JPH::BodyID id = ToJolt(body.bodyId);
    if (id.IsInvalid())
        return;

    // Everything this body was touching has stopped touching it, and the next
    // step cannot say so — the body will be gone and the entity behind it
    // forgotten. Build those Exits now, while both are still knowable, and let the
    // next Update() deliver them.
    _impl->EmitExitsFor(id, _impl->pendingExits);

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
    std::erase_if(_impl->movingBodyIds, matches);
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

    // Characters first. They are swept rather than solved, so they react to the
    // world as the step found it; whatever they push then has the rest of this
    // step to respond, instead of waiting for the next one.
    _impl->StepCharacters(deltaTime);

    /* This world's own scratch allocator, and the shared thread pool. Both are
       Update() arguments; the pool is shared (one set of workers), the allocator
       is per-world so two worlds' steps never touch the same scratch stack (see
       JoltRuntime). */
    _impl->physicsSystem.Update(deltaTime, _impl->collisionSteps, &_impl->tempAlloc,
                                &_impl->jolt.JobSystem());

    _impl->ResolveContactEvents();
}

void PhysicsWorld::SetCollisionSteps(int32_t steps)
{
    _impl->collisionSteps = std::clamp(steps, 1, Impl::kMaxCollisionSteps);
}

int32_t PhysicsWorld::GetCollisionSteps() const
{
    return _impl->collisionSteps;
}

// ---------------------------------------------------------------------------
// Body state
// ---------------------------------------------------------------------------

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
    return bodies.IsAdded(ToJolt(body.bodyId)) && bodies.IsActive(ToJolt(body.bodyId));
}

void PhysicsWorld::DeactivateBody(const RigidBody &body)
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    if (!bodies.IsAdded(ToJolt(body.bodyId)))
        return;
    bodies.DeactivateBody(ToJolt(body.bodyId));
}

void PhysicsWorld::ApplyBodyState(const RigidBody &body, glm::vec3 position, glm::quat rotation,
                                  glm::vec3 linearVelocity, glm::vec3 angularVelocity, bool activate)
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    if (!bodies.IsAdded(ToJolt(body.bodyId)))
        return;

    // A static body can be *placed*, it just has no motion to place it with —
    // the same split SetBodyTransform makes. Refusing the whole call for one
    // would be a trap: a correction for a body the two ends disagree about the
    // motion type of would silently do nothing, which is the worst available
    // outcome for a peer that is trying to tell us where something is.
    const bool isStatic = bodies.GetMotionType(ToJolt(body.bodyId)) == JPH::EMotionType::Static;

    // Normalized for the same reason AddBody and SetBodyTransform do it: a
    // quaternion that crossed a wire (or a level file) is often a hair off unit
    // length, and Jolt asserts IsNormalized() when it rotates with one.
    bodies.SetPositionAndRotation(ToJolt(body.bodyId), JPH::RVec3(position.x, position.y, position.z),
                                  JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w).Normalized(),
                                  (activate && !isStatic) ? JPH::EActivation::Activate
                                                          : JPH::EActivation::DontActivate);

    if (!isStatic)
    {
        // Before the deactivate below, not after: Jolt ignores velocity written
        // to a sleeping body, so zeroing an about-to-sleep body has to happen
        // while it is still awake.
        bodies.SetLinearVelocity(ToJolt(body.bodyId), JPH::Vec3(linearVelocity.x, linearVelocity.y, linearVelocity.z));
        bodies.SetAngularVelocity(ToJolt(body.bodyId), JPH::Vec3(angularVelocity.x, angularVelocity.y, angularVelocity.z));

        if (!activate)
            bodies.DeactivateBody(ToJolt(body.bodyId));
    }

    // Collapse both snapshots onto the corrected pose. Without this the next
    // InterpolateTransforms() blends from the pre-correction pose and smears the
    // jump across a frame — which the view-side error smoothing is *also* trying
    // to absorb, so the two double-count into a wobble at every correction.
    const auto it = _impl->snapshots.find(ToJolt(body.bodyId).GetIndexAndSequenceNumber());
    if (it != _impl->snapshots.end())
        it->second = Impl::MotionSnapshot{position, rotation, position, rotation};
}

std::pair<glm::vec3, glm::quat> PhysicsWorld::GetBodyTransform(const RigidBody &body) const
{
    const JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    const JPH::RVec3 pos = bodies.GetPosition(ToJolt(body.bodyId));
    const JPH::Quat rot = bodies.GetRotation(ToJolt(body.bodyId));
    return {glm::vec3(pos.GetX(), pos.GetY(), pos.GetZ()),
            glm::quat(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ())};
}

std::pair<glm::vec3, glm::vec3> PhysicsWorld::GetBodyVelocity(const RigidBody &body) const
{
    const JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();

    // Static bodies have no motion state; querying velocity on them is meaningless
    // (and GetLinearVelocity would just return zero anyway). Report zero for those
    // and for handles whose body isn't in the simulation.
    if (!bodies.IsAdded(ToJolt(body.bodyId)) || bodies.GetMotionType(ToJolt(body.bodyId)) == JPH::EMotionType::Static)
    {
        return {glm::vec3(0.f), glm::vec3(0.f)};
    }

    const JPH::Vec3 lin = bodies.GetLinearVelocity(ToJolt(body.bodyId));
    const JPH::Vec3 ang = bodies.GetAngularVelocity(ToJolt(body.bodyId));
    return {glm::vec3(lin.GetX(), lin.GetY(), lin.GetZ()),
            glm::vec3(ang.GetX(), ang.GetY(), ang.GetZ())};
}

bool PhysicsWorld::IsBodyCCDEnabled(const RigidBody &body) const
{
    const JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    if (!bodies.IsAdded(ToJolt(body.bodyId)))
    {
        return false;
    }
    return bodies.GetMotionQuality(ToJolt(body.bodyId)) == JPH::EMotionQuality::LinearCast;
}

void PhysicsWorld::SetBodyTransform(const RigidBody &body, glm::vec3 position, glm::quat rotation)
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    if (!bodies.IsAdded(ToJolt(body.bodyId)))
        return;

    const bool isStatic = bodies.GetMotionType(ToJolt(body.bodyId)) == JPH::EMotionType::Static;

    // A static sensor only hears about bodies that are awake, so the space it is
    // leaving has to be woken as well as the space it is arriving in: whatever it
    // was containing must be re-tested to notice the pair ended, and whatever it
    // lands on must be re-tested to notice the pair began. Captured before the
    // move, used after it.
    const bool wakesAround = isStatic && IsTriggerLayer(bodies.GetObjectLayer(ToJolt(body.bodyId)));
    JPH::AABox touched;
    if (wakesAround)
    {
        JPH::BodyLockRead lock(_impl->physicsSystem.GetBodyLockInterface(), ToJolt(body.bodyId));
        if (lock.Succeeded())
            touched = lock.GetBody().GetWorldSpaceBounds();
    }

    // Normalize before handing the quaternion to Jolt: a hand-authored or imported
    // rotation is often a hair off unit length (e.g. a level's [0.707, 0.707, 0, 0]
    // has length^2 0.9997), and Jolt asserts IsNormalized() when it rotates with it.
    // AddBody normalizes for the same reason.
    bodies.SetPositionAndRotation(ToJolt(body.bodyId), JPH::RVec3(position.x, position.y, position.z),
                                  JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w).Normalized(),
                                  isStatic ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);

    // Velocity is only meaningful for dynamic bodies; static bodies have no active motion.
    if (!isStatic)
    {
        bodies.SetLinearVelocity(ToJolt(body.bodyId), JPH::Vec3::sZero());
        bodies.SetAngularVelocity(ToJolt(body.bodyId), JPH::Vec3::sZero());
    }

    // Collapse both snapshots onto the teleport target. Without this the next
    // InterpolateTransforms() would blend from the pre-teleport pose and slide
    // the body across the gap over one frame instead of snapping to it.
    const auto it = _impl->snapshots.find(ToJolt(body.bodyId).GetIndexAndSequenceNumber());
    if (it != _impl->snapshots.end())
    {
        it->second = Impl::MotionSnapshot{position, rotation, position, rotation};
    }

    if (wakesAround)
    {
        JPH::ObjectLayer layer = 0;
        {
            JPH::BodyLockRead lock(_impl->physicsSystem.GetBodyLockInterface(), ToJolt(body.bodyId));
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

void PhysicsWorld::MoveBodyKinematic(const RigidBody &body, glm::vec3 position, glm::quat rotation,
                                     float deltaTime)
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    if (!bodies.IsAdded(ToJolt(body.bodyId)) || deltaTime <= 0.f)
    {
        return;
    }
    if (bodies.GetMotionType(ToJolt(body.bodyId)) == JPH::EMotionType::Static)
    {
        return;
    }

    // MoveKinematic, not SetPositionAndRotation: it works out the velocity that
    // carries the body there over the step and leaves it on the body. That
    // velocity is the whole point — it is what pushes resting bodies along and
    // what a character standing on this reads to ride it.
    bodies.MoveKinematic(ToJolt(body.bodyId), JPH::RVec3(position.x, position.y, position.z),
                         JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w).Normalized(),
                         deltaTime);
}

void PhysicsWorld::SetBodyLinearVelocity(const RigidBody &body, glm::vec3 velocity)
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    if (!bodies.IsAdded(ToJolt(body.bodyId)) || bodies.GetMotionType(ToJolt(body.bodyId)) == JPH::EMotionType::Static)
        return;

    // Activate first, then set: a body Jolt has put to sleep on a surface ignores
    // velocity written while it is asleep, which reads as the call silently doing
    // nothing — exactly the case a contact response hits, since landing is what
    // puts a body to sleep in the first place.
    bodies.ActivateBody(ToJolt(body.bodyId));
    bodies.SetLinearVelocity(ToJolt(body.bodyId), JPH::Vec3(velocity.x, velocity.y, velocity.z));
}

void PhysicsWorld::SetBodyCollisionFilter(const RigidBody &body, CollisionFilter filter)
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    if (!bodies.IsAdded(ToJolt(body.bodyId)))
        return;

    const bool sensor = filter.channel == CollisionChannel::Trigger;

    // The motion type rides in the layer beside the channel, so it has to be
    // carried across rather than defaulted — repacking without it would quietly
    // move the body to another broad-phase tree.
    BodyMotion motion = MotionOf(bodies.GetObjectLayer(ToJolt(body.bodyId)));
    if (sensor && motion == BodyMotion::Dynamic)
        motion = BodyMotion::Kinematic;

    bodies.SetObjectLayer(ToJolt(body.bodyId), PackLayer(filter, motion));

    if (motion != BodyMotion::Static)
        bodies.ActivateBody(ToJolt(body.bodyId));

    // Sensor-ness is a body flag rather than part of the layer, and Jolt exposes
    // no interface-level setter for it.
    JPH::BodyLockWrite lock(_impl->physicsSystem.GetBodyLockInterface(), ToJolt(body.bodyId));
    if (lock.Succeeded())
        lock.GetBody().SetIsSensor(sensor);
}

CollisionFilter PhysicsWorld::GetBodyCollisionFilter(const RigidBody &body) const
{
    if (!_impl->physicsSystem.GetBodyInterface().IsAdded(ToJolt(body.bodyId)))
        return CollisionFilter{};
    return _impl->FilterOf(ToJolt(body.bodyId));
}

void PhysicsWorld::ReshapeBody(const RigidBody &body, const ColliderShapeDesc &shape)
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    if (!bodies.IsAdded(ToJolt(body.bodyId)))
        return;

    bodies.SetShape(ToJolt(body.bodyId), MakeShape(shape), /*inUpdateMassProperties=*/ true,
                    JPH::EActivation::DontActivate);
}

void PhysicsWorld::SetBodyCCD(const RigidBody &body, bool enable)
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    if (!bodies.IsAdded(ToJolt(body.bodyId)))
        return;

    // Set motion quality even when the body is currently Static, rather than
    // guarding on Dynamic: the inspector freezes the selected body to Static while
    // a widget is active, so a guard would silently drop the CCD checkbox. Motion
    // quality is a stored property (our bodies always have motion properties, since
    // AddBody sets mAllowDynamicOrKinematic), so it sticks and takes effect once
    // the body is Dynamic again. Jolt no-ops safely if a body genuinely has none.
    const JPH::EMotionQuality quality =
        enable ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
    bodies.SetMotionQuality(ToJolt(body.bodyId), quality);
}

void PhysicsWorld::SetBodyMotionType(const RigidBody &body, BodyMotion motion)
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    if (!bodies.IsAdded(ToJolt(body.bodyId)))
        return;

    const CollisionFilter filter = _impl->FilterOf(ToJolt(body.bodyId));

    // Same rule AddBody applies: a sensor is never dynamic, because one falling
    // under gravity would leave the volume it was authored as.
    const BodyMotion effective = filter.channel == CollisionChannel::Trigger &&
                                 motion == BodyMotion::Dynamic
                                     ? BodyMotion::Kinematic
                                     : motion;

    auto &ids = _impl->movingBodyIds;
    if (effective == BodyMotion::Static)
    {
        // Jolt asserts that a body is inactive before switching it to Static.
        bodies.DeactivateBody(ToJolt(body.bodyId));
        bodies.SetMotionType(ToJolt(body.bodyId), JPH::EMotionType::Static, JPH::EActivation::DontActivate);
        ids.erase(std::remove(ids.begin(), ids.end(), ToJolt(body.bodyId)), ids.end());
    }
    else
    {
        const JPH::EMotionType motionType = effective == BodyMotion::Kinematic
                                                ? JPH::EMotionType::Kinematic
                                                : JPH::EMotionType::Dynamic;
        bodies.SetMotionType(ToJolt(body.bodyId), motionType, JPH::EActivation::Activate);

        // Kinematic counts as moving: something can drive one through the
        // simulation, and its render pose has to follow when it does.
        if (std::find(ids.begin(), ids.end(), ToJolt(body.bodyId)) == ids.end())
        {
            ids.push_back(ToJolt(body.bodyId));
        }
    }

    // The layer records the motion type, and the broad-phase tree a body lives in
    // is read from it. Left stale, a body made dynamic would keep saying it never
    // moves and would never be tested against the things it now falls onto.
    bodies.SetObjectLayer(ToJolt(body.bodyId), PackLayer(filter, effective));
}

void PhysicsWorld::SetGravity(glm::vec3 gravity)
{
    _impl->physicsSystem.SetGravity(JPH::Vec3(gravity.x, gravity.y, gravity.z));

    /* Wake everything that moves so it responds to the new gravity immediately.
       A kinematic body among them ignores gravity and is simply woken for
       nothing, which is cheaper than keeping a second list to spare it. */
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    for (const JPH::BodyID &id : _impl->movingBodyIds)
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
