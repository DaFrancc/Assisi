/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file PhysicsWorld.cpp
/// @brief The world itself: bringing it up, stepping it, and reading and
///        writing one entity's body.
///
/// The rest of PhysicsWorld lives beside this file rather than in it. Keeping
/// bodies in step with the scene is in PhysicsReconcile.cpp, contacts in
/// PhysicsContacts.cpp, scene queries in PhysicsQueries.cpp, characters in
/// PhysicsCharacters.cpp, and the render/replication writeback in
/// PhysicsWriteback.cpp; what they share is PhysicsInternal.hpp.

#include "PhysicsInternal.hpp"

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Scene.hpp>

#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
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

/// Closer to 1 than this on every axis, a scale is no scale: wrapping the shape
/// would cost a level of indirection on every collision test for nothing.
constexpr float kUnitScaleTolerance = 1e-6f;
} // namespace

JPH::ShapeRefC MakeShape(const PhysicsWorld::ColliderShapeDesc &shape)
{
    const float radius = glm::max(shape.radius, JPH::cDefaultConvexRadius);
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

JPH::ShapeRefC MakeScaledShape(const PhysicsWorld::ColliderShapeDesc &shape, glm::vec3 scale)
{
    JPH::ShapeRefC base = MakeShape(shape);
    if (glm::all(glm::lessThan(glm::abs(scale - glm::vec3(1.f)), glm::vec3(kUnitScaleTolerance))))
    {
        return base;
    }
    const JPH::Vec3 valid = base->MakeScaleValid(JPH::Vec3(scale.x, scale.y, scale.z));
    return new JPH::ScaledShape(base, valid);
}

// Contact-solver tuning (see the constructor). Rather than brute-forcing high
// step rates, we lean on the same cheap mechanism Unity/Unreal use: speculative
// contacts (a predictive margin that stops a body at a surface within one solve)
// plus a small allowed overlap that resolves gently. Fast free-fallers that
// still slip past the fixed margin are handled per-body via CCD (enableCCD).
// Jolt defaults: 0.02 m slop, 0.2 Baumgarte, 0.02 m speculative distance, 0.75
// linear-cast threshold.
constexpr float kPenetrationSlop = 0.01f; ///< Allowed resting overlap (meters) — Unity-like contact offset.
constexpr float kSpeculativeContactDist =
    0.05f; ///< Predictive contact margin (meters); catches moderate impacts in one solve.
// CCD (LinearCast) engages once a body moves more than this * its shape's inner
// radius in a step. Below Jolt's 0.75 default so CCD-enabled bodies stop sinking
// at lower speeds (no "floaty" landings), but not so low that they sweep on
// nearly every step: 0.3 keeps sweeps to genuinely fast motion. Only costs CPU
// for bodies with CCD on (enableCCD), so the perf downside is bounded. For a 1 m
// box (inner radius 0.5) this triggers at ~9 m/s / a ~4 m drop.
constexpr float kLinearCastThreshold = 0.3f;

PhysicsWorld::PhysicsWorld(ECS::Scene &scene, uint32_t maxBodies)
{
    /* Impl's first member acquires the shared Jolt runtime, so the library is up
       (allocator/Factory/types) before any of its other members construct. */
    _impl = std::make_unique<Impl>(scene);
    _impl->maxBodies = maxBodies;
    _impl->clearEpoch = scene.ClearEpoch();

    _impl->physicsSystem.Init(maxBodies, 0u, Impl::kMaxBodyPairs, Impl::kMaxContactConstraints,
                              _impl->bpLayerInterface, _impl->objVsBPFilter, _impl->objLayerFilter);

    // Prevent impact penetration the cheap way (see the constant block above):
    // a wider speculative-contact margin lets the solver stop a body at a surface
    // within a single step, and a small allowed overlap keeps resting contacts
    // from jittering. Baumgarte and solver iteration counts stay at Jolt's
    // defaults — a gentle correction is less visible than an aggressive one.
    JPH::PhysicsSettings settings = _impl->physicsSystem.GetPhysicsSettings();
    settings.mPenetrationSlop = kPenetrationSlop;
    settings.mSpeculativeContactDistance = kSpeculativeContactDist;
    settings.mLinearCastThreshold = kLinearCastThreshold;
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
    // Characters own inner bodies they destroy themselves, so everything goes
    // before the PhysicsSystem those bodies live in. Impl's members are then
    // destroyed in reverse declaration order, releasing the shared runtime last.
    _impl->DestroyAll();
    _impl.reset();
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

PhysicsWorld::Impl::BodySlot *PhysicsWorld::Impl::SlotFor(ECS::Entity entity)
{
    if (entity.index >= slots.size())
    {
        return nullptr;
    }
    BodySlot &slot = slots[entity.index];
    if (slot.kind == SlotKind::Empty || slot.generation != entity.generation)
    {
        return nullptr;
    }
    return &slot;
}

const PhysicsWorld::Impl::BodySlot *PhysicsWorld::Impl::SlotFor(ECS::Entity entity) const
{
    if (entity.index >= slots.size())
    {
        return nullptr;
    }
    const BodySlot &slot = slots[entity.index];
    if (slot.kind == SlotKind::Empty || slot.generation != entity.generation)
    {
        return nullptr;
    }
    return &slot;
}

PhysicsWorld::Impl::BodySlot &PhysicsWorld::Impl::SlotAt(ECS::Entity entity)
{
    if (entity.index >= slots.size())
    {
        slots.resize(static_cast<std::size_t>(entity.index) + 1u);
    }
    return slots[entity.index];
}

ECS::Entity PhysicsWorld::Impl::EntityFor(const JPH::BodyID &id) const
{
    if (id.IsInvalid())
    {
        return ECS::NullEntity;
    }
    return EntityOfUserData(physicsSystem.GetBodyInterface().GetUserData(id));
}

JPH::BodyID PhysicsWorld::Impl::BodyFor(ECS::Entity entity) const
{
    const BodySlot *slot = SlotFor(entity);
    return slot == nullptr ? JPH::BodyID{} : slot->body;
}

void PhysicsWorld::Impl::CollapseSnapshot(BodySlot &slot, const Pose &pose)
{
    slot.snapshot = MotionSnapshot{pose.rotation, pose.rotation, pose.position, pose.position};
}

void PhysicsWorld::Impl::Follow(std::uint32_t index)
{
    BodySlot &slot = slots[index];
    slot.settled = false;
    if (!slot.followed)
    {
        slot.followed = true;
        awake.push_back(index);
    }
}

// ---------------------------------------------------------------------------
// Stepping
// ---------------------------------------------------------------------------

void PhysicsWorld::Reconcile()
{
    ASSISI_ASSERT(!_impl->stepping, "PhysicsWorld::Reconcile called while the world is stepping");
    _impl->ReconcileScene(0.f);
}

void PhysicsWorld::Rebuild()
{
    ASSISI_ASSERT(!_impl->stepping, "PhysicsWorld::Rebuild called while the world is stepping");
    _impl->DestroyAll();
    // From the start of the scene's history, so the reconcile below finds every
    // descriptor it holds and builds each one new.
    _impl->changeCursor = 0;
    _impl->ReconcileScene(0.f);
}

void PhysicsWorld::Update(float deltaTime)
{
    ASSISI_ASSERT(!_impl->stepping, "PhysicsWorld::Update called while the world is stepping");

    // The events describe the step about to run, not the one before it — clearing
    // here is what guarantees a consumer sees each one exactly once. Safe without
    // the mutex: no Jolt worker is inside a callback at this point.
    _impl->events.clear();

    // First, so the step simulates the scene as it is now: bodies for what was
    // added, none for what was destroyed, and every Transform written since the
    // last step already pushed. The Exits of what it destroys join those below.
    _impl->ReconcileScene(deltaTime);

    // Exits recorded when a body was destroyed. They belong to this step: the pair
    // ended when the body went away, and there was no step in between.
    _impl->events.swap(_impl->pendingExits);
    _impl->pendingExits.clear();

    ++_impl->step;

    _impl->stepping = true;

    // Characters first. They are swept rather than solved, so they react to the
    // world as the step found it; whatever they push then has the rest of this
    // step to respond, instead of waiting for the next one.
    _impl->StepCharacters(deltaTime);

    /* This world's own scratch allocator, and the shared thread pool. Both are
       Update() arguments; the pool is shared (one set of workers), the allocator
       is per-world so two worlds' steps never touch the same scratch stack (see
       JoltRuntime). */
    _impl->physicsSystem.Update(deltaTime, _impl->collisionSteps, &_impl->tempAlloc, &_impl->jolt.JobSystem());

    _impl->stepping = false;

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
// One entity's body
// ---------------------------------------------------------------------------

bool PhysicsWorld::HasBody(ECS::Entity entity) const
{
    return _impl->SlotFor(entity) != nullptr;
}

void PhysicsWorld::Teleport(ECS::Entity entity, const Pose &pose)
{
    ASSISI_ASSERT(!_impl->stepping, "PhysicsWorld::Teleport called while the world is stepping");

    Impl::BodySlot *slot = _impl->SlotFor(entity);
    if (slot == nullptr)
    {
        return;
    }

    const JPH::Quat rotation =
        JPH::Quat(pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w).Normalized();
    const JPH::RVec3 position(pose.position.x, pose.position.y, pose.position.z);
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();

    if (slot->kind == Impl::SlotKind::Character)
    {
        Impl::CharacterRecord *record = _impl->FindCharacter(entity);
        record->character->SetPosition(position);
        record->character->SetRotation(rotation);
        record->character->SetLinearVelocity(JPH::Vec3::sZero());
        bodies.SetPositionAndRotation(slot->body, position, rotation, JPH::EActivation::Activate);

        // What it was touching is about to be wrong. Re-finding it here rather
        // than waiting a step keeps the ground state honest for anything that
        // reads it between the teleport and the next Update.
        const FilterLayerFilter layerFilter{record->queryFilter};
        record->character->RefreshContacts({}, layerFilter, {}, {}, _impl->tempAlloc);
    }
    else
    {
        const bool isStatic = slot->motion == BodyMotion::Static;
        bodies.SetPositionAndRotation(slot->body, position, rotation,
                                      isStatic ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
        if (!isStatic)
        {
            bodies.SetLinearVelocity(slot->body, JPH::Vec3::sZero());
            bodies.SetAngularVelocity(slot->body, JPH::Vec3::sZero());
        }
    }
    Impl::CollapseSnapshot(*slot, pose);

    // The Transform is written now rather than left to the writeback, which
    // follows only bodies that are awake: a static or sleeping one would be drawn
    // where it was for good. Stamped as this world's own, so it is not pushed back,
    // and written over any write still waiting to be pushed, which this replaces.
    _impl->WritePose(entity, pose, slot->kind != Impl::SlotKind::Character);
}

void PhysicsWorld::AdoptTransform(ECS::Entity entity)
{
    const Impl::BodySlot *slot = _impl->SlotFor(entity);
    if (slot == nullptr)
    {
        return;
    }
    _impl->StampTransform(entity);
    if (slot->kind == Impl::SlotKind::Body && slot->motion != BodyMotion::Static)
    {
        _impl->Follow(entity.index);
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
            continue;

        const JPH::RVec3 position = bodies.GetPosition(id);
        const JPH::Quat rotation = bodies.GetRotation(id);
        const JPH::Vec3 linear = bodies.GetLinearVelocity(id);
        const JPH::Vec3 angular = bodies.GetAngularVelocity(id);

        out.push_back(ActiveBodyState{
                entity,
                glm::vec3(position.GetX(), position.GetY(), position.GetZ()),
                glm::quat(rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ()),
                glm::vec3(linear.GetX(), linear.GetY(), linear.GetZ()),
                glm::vec3(angular.GetX(), angular.GetY(), angular.GetZ()),
            });
    }
}

bool PhysicsWorld::IsBodyActive(ECS::Entity entity) const
{
    const JPH::BodyID id = _impl->BodyFor(entity);
    return !id.IsInvalid() && _impl->physicsSystem.GetBodyInterface().IsActive(id);
}

void PhysicsWorld::DeactivateBody(ECS::Entity entity)
{
    const JPH::BodyID id = _impl->BodyFor(entity);
    if (id.IsInvalid())
        return;
    _impl->physicsSystem.GetBodyInterface().DeactivateBody(id);
}

void PhysicsWorld::ApplyBodyState(ECS::Entity entity, const Pose &pose, glm::vec3 linearVelocity,
                                  glm::vec3 angularVelocity, bool activate)
{
    ASSISI_ASSERT(!_impl->stepping, "PhysicsWorld::ApplyBodyState called while the world is stepping");

    Impl::BodySlot *slot = _impl->SlotFor(entity);
    if (slot == nullptr || slot->kind != Impl::SlotKind::Body)
        return;

    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();

    // A static body can be *placed*, it just has no motion to place it with.
    // Refusing the whole call for one would be a trap: a correction for a body
    // the two ends disagree about the motion type of would silently do nothing,
    // which is the worst available outcome for a peer that is trying to tell us
    // where something is.
    const bool isStatic = slot->motion == BodyMotion::Static;

    // Normalized: a quaternion that crossed a wire is often a hair off unit
    // length, and Jolt asserts IsNormalized() when it rotates with one.
    const JPH::Quat rotation =
        JPH::Quat(pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w).Normalized();
    bodies.SetPositionAndRotation(slot->body, JPH::RVec3(pose.position.x, pose.position.y, pose.position.z),
                                  rotation,
                                  (activate && !isStatic) ? JPH::EActivation::Activate
                                                          : JPH::EActivation::DontActivate);

    if (!isStatic)
    {
        // Before the deactivate below, not after: Jolt ignores velocity written
        // to a sleeping body, so zeroing an about-to-sleep body has to happen
        // while it is still awake.
        bodies.SetLinearVelocity(slot->body, JPH::Vec3(linearVelocity.x, linearVelocity.y, linearVelocity.z));
        bodies.SetAngularVelocity(slot->body, JPH::Vec3(angularVelocity.x, angularVelocity.y, angularVelocity.z));

        if (!activate)
            bodies.DeactivateBody(slot->body);
    }

    Impl::CollapseSnapshot(*slot, pose);

    // Followed until the writeback has put the Transform at the corrected pose,
    // which for a body left asleep would otherwise never happen.
    _impl->Follow(entity.index);
}

Pose PhysicsWorld::GetBodyPose(ECS::Entity entity) const
{
    const JPH::BodyID id = _impl->BodyFor(entity);
    if (id.IsInvalid())
    {
        return Pose{};
    }
    const JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    const JPH::RVec3 pos = bodies.GetPosition(id);
    const JPH::Quat rot = bodies.GetRotation(id);
    return Pose{glm::quat(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ()), glm::vec3(pos.GetX(), pos.GetY(), pos.GetZ())};
}

std::pair<glm::vec3, glm::vec3> PhysicsWorld::GetBodyVelocity(ECS::Entity entity) const
{
    const Impl::BodySlot *slot = _impl->SlotFor(entity);
    if (slot == nullptr || slot->motion == BodyMotion::Static)
    {
        return {glm::vec3(0.f), glm::vec3(0.f)};
    }

    const JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    const JPH::Vec3 lin = bodies.GetLinearVelocity(slot->body);
    const JPH::Vec3 ang = bodies.GetAngularVelocity(slot->body);
    return {glm::vec3(lin.GetX(), lin.GetY(), lin.GetZ()), glm::vec3(ang.GetX(), ang.GetY(), ang.GetZ())};
}

bool PhysicsWorld::IsBodyCCDEnabled(ECS::Entity entity) const
{
    const Impl::BodySlot *slot = _impl->SlotFor(entity);
    if (slot == nullptr || slot->motion == BodyMotion::Static)
    {
        return false;
    }
    return _impl->physicsSystem.GetBodyInterface().GetMotionQuality(slot->body) == JPH::EMotionQuality::LinearCast;
}

glm::vec3 PhysicsWorld::GetColliderScale(ECS::Entity entity) const
{
    const Impl::BodySlot *slot = _impl->SlotFor(entity);
    if (slot == nullptr || slot->kind != Impl::SlotKind::Body)
    {
        return glm::vec3(1.f);
    }
    // Read back from the shape rather than recomputed, so it is whatever Jolt
    // made of the scale it was asked for.
    const JPH::ShapeRefC shape = _impl->physicsSystem.GetBodyInterface().GetShape(slot->body);
    if (shape->GetSubType() != JPH::EShapeSubType::Scaled)
    {
        return glm::vec3(1.f);
    }
    const JPH::Vec3 scale = static_cast<const JPH::ScaledShape *>(shape.GetPtr())->GetScale();
    return glm::vec3(scale.GetX(), scale.GetY(), scale.GetZ());
}

void PhysicsWorld::SetBodyLinearVelocity(ECS::Entity entity, glm::vec3 velocity)
{
    ASSISI_ASSERT(!_impl->stepping, "PhysicsWorld::SetBodyLinearVelocity called while the world is stepping");

    const Impl::BodySlot *slot = _impl->SlotFor(entity);
    if (slot == nullptr || slot->kind != Impl::SlotKind::Body || slot->motion == BodyMotion::Static)
        return;

    // Activate first, then set: a body Jolt has put to sleep on a surface ignores
    // velocity written while it is asleep, which reads as the call silently doing
    // nothing — exactly the case a contact response hits, since landing is what
    // puts a body to sleep in the first place.
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    bodies.ActivateBody(slot->body);
    bodies.SetLinearVelocity(slot->body, JPH::Vec3(velocity.x, velocity.y, velocity.z));
}

CollisionFilter PhysicsWorld::GetBodyCollisionFilter(ECS::Entity entity) const
{
    const JPH::BodyID id = _impl->BodyFor(entity);
    if (id.IsInvalid())
        return CollisionFilter{};
    return _impl->FilterOf(id);
}

void PhysicsWorld::SetGravity(glm::vec3 gravity)
{
    _impl->physicsSystem.SetGravity(JPH::Vec3(gravity.x, gravity.y, gravity.z));

    /* Wake everything that moves so it responds to the new gravity immediately.
       A kinematic body among them ignores gravity and is simply woken for
       nothing, which is cheaper than keeping a second list to spare it. */
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();
    for (const Impl::BodySlot &slot : _impl->slots)
    {
        if (slot.kind == Impl::SlotKind::Body && slot.motion != BodyMotion::Static)
        {
            bodies.ActivateBody(slot.body);
        }
    }
}

glm::vec3 PhysicsWorld::GetGravity() const
{
    const JPH::Vec3 g = _impl->physicsSystem.GetGravity();
    return glm::vec3(g.GetX(), g.GetY(), g.GetZ());
}

} // namespace Assisi::Physics
