/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file PhysicsReconcile.cpp
/// @brief Keeping the simulation in step with the scene: building, editing and
///        destroying bodies as components come and go, and pushing Transforms
///        written by anything else to the bodies they belong to.
///
/// Everything here reads the scene's change ticks and removal log, the way every
/// engine-side table keyed by entity stays current. Nothing outside this module
/// is told to create or destroy a body; it puts components on an entity, edits
/// them, writes its Transform, or destroys it, and the next reconcile follows.

#include "PhysicsInternal.hpp"

#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/TransformPose.hpp>

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>

#include <Assisi/Math/Matrix.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace Assisi::Physics
{

namespace
{

/// Where @p transform puts @p entity in world space. Jolt works in world space,
/// and a parented Transform is an offset from its parent.
Pose WorldPoseOf(const ECS::Scene &scene, ECS::Entity entity, const ECS::Transform &transform)
{
    if (const std::optional<glm::mat4> parent = SimulationParentMatrix(scene, entity); parent.has_value())
    {
        const ECS::Transform world = ECS::PoseUnderParent(transform, *parent);
        return Pose{world.rotation, world.position};
    }
    return Pose{transform.rotation, transform.position};
}

/// @p transform's scale composed with every parent's, which is the size the
/// collider is built at.
glm::vec3 WorldScaleOf(const ECS::Scene &scene, ECS::Entity entity, const ECS::Transform &transform)
{
    if (const std::optional<glm::mat4> parent = SimulationParentMatrix(scene, entity); parent.has_value())
    {
        using Math::ColumnOf;
        using Math::MatrixColumn;
        const glm::vec3 parentScale{glm::length(ColumnOf(*parent, MatrixColumn::Right)),
                                    glm::length(ColumnOf(*parent, MatrixColumn::Up)),
                                    glm::length(ColumnOf(*parent, MatrixColumn::Back))};
        return transform.scale * parentScale;
    }
    return transform.scale;
}

/// Every degree of freedom, as LockedAxis bits.
constexpr uint32_t kAllAxes = (1u << static_cast<uint32_t>(LockedAxis::Count)) - 1u;

/// The motion a body is built with: static without a RigidBody, and kinematic
/// when every axis is locked, since Jolt cannot simulate a body with no freedom
/// left and one that may not move is moved only by its Transform.
BodyMotion MotionOf(const RigidBody *rigidBody)
{
    if (rigidBody == nullptr)
    {
        return BodyMotion::Static;
    }
    if (rigidBody->motion == MotionType::Kinematic || (rigidBody->lockedAxes.bits & kAllAxes) == kAllAxes)
    {
        return BodyMotion::Kinematic;
    }
    return BodyMotion::Dynamic;
}

/// The degrees of freedom @p rigidBody leaves free. LockedAxis numbers them in
/// the order Jolt's flags do.
JPH::EAllowedDOFs AllowedDOFsOf(const RigidBody &rigidBody)
{
    const uint32_t free = kAllAxes & ~rigidBody.lockedAxes.bits;
    return free == 0u ? JPH::EAllowedDOFs::All : static_cast<JPH::EAllowedDOFs>(free);
}

JPH::EMotionQuality MotionQualityOf(const RigidBody &rigidBody)
{
    return rigidBody.ccd ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
}

/// Orders entities by index, then generation, so repeats sit together.
bool EntityBefore(ECS::Entity a, ECS::Entity b)
{
    return a.index < b.index || (a.index == b.index && a.generation < b.generation);
}

/// Whether two colliders build the same shape.
bool SameShape(const Collider &a, const Collider &b)
{
    return a.shape == b.shape && a.halfExtents == b.halfExtents && a.radius == b.radius &&
           a.halfHeight == b.halfHeight && a.offsetPosition == b.offsetPosition && a.offsetRotation == b.offsetRotation;
}

JPH::EMotionType JoltMotionOf(BodyMotion motion)
{
    if (motion == BodyMotion::Static)
    {
        return JPH::EMotionType::Static;
    }
    return motion == BodyMotion::Kinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Dynamic;
}

bool SameFilter(CollisionFilter a, CollisionFilter b)
{
    return a.channel == b.channel && a.collidesWith.bits == b.collidesWith.bits;
}

JPH::RVec3 ToJolt(glm::vec3 position)
{
    return JPH::RVec3(position.x, position.y, position.z);
}

JPH::Vec3 ToJoltVector(glm::vec3 vector)
{
    return JPH::Vec3(vector.x, vector.y, vector.z);
}

/// Normalized: a hand-authored or imported rotation is often a hair off unit
/// length, and Jolt asserts IsNormalized() when it rotates with one.
JPH::Quat ToJolt(glm::quat rotation)
{
    return JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w).Normalized();
}

} // namespace

void PhysicsWorld::Impl::ReconcileScene(float stepTime)
{
    // A Clear hands out Entity{0, 0} again, so every handle in the table may now
    // name an unrelated entity and nothing in the logs says which. Start over;
    // the zero cursor below then finds every component the new scene holds.
    if (scene.ClearEpoch() != clearEpoch)
    {
        DestroyAll();
        clearEpoch = scene.ClearEpoch();
        changeCursor = 0;
    }

    // Read before anything below runs, so the next reconcile starts where this
    // one's view of the scene ended.
    const uint64_t now = scene.CurrentChangeTick();

    // Removals first, so an index freed and handed to a new entity is empty by
    // the time the new entity's components are read.
    scratchRemoved.clear();
    bool complete = scene.RemovedSince<Collider>(changeCursor, scratchRemoved);
    complete = scene.RemovedSince<Character>(changeCursor, scratchRemoved) && complete;
    complete = scene.RemovedSince<ECS::Transform>(changeCursor, scratchRemoved) && complete;
    if (complete)
    {
        // Gone, even when it is back already: an entity revived at its own handle
        // or a Collider removed and added again is a new life, so it gets a new
        // body rather than the old one's motion.
        for (const ECS::Entity entity : scratchRemoved)
        {
            if (SlotFor(entity) != nullptr)
            {
                DestroySlot(entity.index);
                SyncEntity(entity);
            }
        }
    }
    else
    {
        // The log no longer reaches back to the last reconcile, so which ones
        // went is unknown: every body is checked against the scene instead.
        for (std::uint32_t index = 0; index < slots.size(); ++index)
        {
            if (slots[index].kind != SlotKind::Empty)
            {
                SyncEntity(ECS::Entity{index, slots[index].generation});
            }
        }
    }

    // A RigidBody removed changes the body's kind rather than ending it, so it
    // is synced like an edit.
    scratchChanged.clear();
    scene.ChangedSince<Collider>(changeCursor, scratchChanged);
    scene.ChangedSince<RigidBody>(changeCursor, scratchChanged);
    scene.ChangedSince<Character>(changeCursor, scratchChanged);
    if (!scene.RemovedSince<RigidBody>(changeCursor, scratchChanged))
    {
        for (std::uint32_t index = 0; index < slots.size(); ++index)
        {
            if (slots[index].kind == SlotKind::Body)
            {
                scratchChanged.push_back(ECS::Entity{index, slots[index].generation});
            }
        }
    }
    // An entity whose Collider and RigidBody both changed is listed twice, and
    // is synced once.
    std::sort(scratchChanged.begin(), scratchChanged.end(), EntityBefore);
    scratchChanged.erase(std::unique(scratchChanged.begin(), scratchChanged.end()), scratchChanged.end());
    for (const ECS::Entity entity : scratchChanged)
    {
        SyncEntity(entity);
    }

    scratchChanged.clear();
    scene.ChangedSince<BodyState>(changeCursor, scratchChanged);
    for (const ECS::Entity entity : scratchChanged)
    {
        PushBodyState(entity);
    }

    scratchChanged.clear();
    scene.ChangedSince<ECS::Transform>(changeCursor, scratchChanged);
    for (const ECS::Entity entity : scratchChanged)
    {
        PushTransform(entity, stepTime);
    }

    StopFinishedSweeps();
    ApplyAskedStances();

    changeCursor = now;
}

void PhysicsWorld::Impl::SyncEntity(ECS::Entity entity)
{
    BodySlot *held = entity.index < slots.size() && slots[entity.index].kind != SlotKind::Empty
                         ? &slots[entity.index]
                         : nullptr;

    // A slot left by an earlier life of this index, whose removal this
    // reconcile is only now hearing about from the new one's components.
    if (held != nullptr && held->generation != entity.generation)
    {
        DestroySlot(entity.index);
        held = nullptr;
    }

    SlotKind wanted = SlotKind::Empty;
    const Character *character = nullptr;
    const Collider *collider = nullptr;
    if (scene.IsAlive(entity) && scene.Has<ECS::Transform>(entity))
    {
        character = scene.Get<Character>(entity);
        collider = scene.Get<Collider>(entity);
        if (character != nullptr)
        {
            wanted = SlotKind::Character;
        }
        else if (collider != nullptr)
        {
            wanted = SlotKind::Body;
        }
    }

    if (held != nullptr && held->kind != wanted)
    {
        DestroySlot(entity.index);
        held = nullptr;
    }

    if (wanted == SlotKind::Body)
    {
        const RigidBody *rigidBody = scene.Get<RigidBody>(entity);
        if (held == nullptr)
        {
            CreateBody(entity, *collider, rigidBody);
        }
        else
        {
            EditBody(entity, *collider, rigidBody);
        }
    }
    else if (wanted == SlotKind::Character)
    {
        if (held == nullptr)
        {
            CreateCharacter(entity, *character);
        }
        else
        {
            EditCharacter(entity, *character);
        }
    }
}

void PhysicsWorld::Impl::CreateBody(ECS::Entity entity, const Collider &collider, const RigidBody *rigidBody)
{
    const ECS::Transform &transform = *scene.Get<ECS::Transform>(entity);
    const Pose pose = WorldPoseOf(scene, entity, transform);
    const glm::vec3 worldScale = WorldScaleOf(scene, entity, transform);
    const CollisionFilter filter{collider.collidesWith, collider.channel};
    const BodyMotion motion = MotionOf(rigidBody);
    const bool sensor = collider.channel == CollisionChannel::Trigger;
    WarnOnClampedScale(entity, collider.shape, worldScale);

    JPH::BodyCreationSettings settings(MakeColliderShape(collider, worldScale), ToJolt(pose.position),
                                       ToJolt(pose.rotation), JoltMotionOf(motion), LayerFor(filter, motion));
    settings.mIsSensor = sensor;
    settings.mUserData = UserDataOf(entity);
    settings.mFriction = collider.friction;
    settings.mRestitution = collider.restitution;

    // A static body carries no motion block. Gaining a RigidBody builds a new
    // body rather than converting this one.
    settings.mAllowDynamicOrKinematic = rigidBody != nullptr;
    if (rigidBody != nullptr)
    {
        settings.mMotionQuality = MotionQualityOf(*rigidBody);
        settings.mLinearDamping = rigidBody->linearDamping;
        settings.mAngularDamping = rigidBody->angularDamping;
        settings.mGravityFactor = rigidBody->gravityScale;
        settings.mAllowSleeping = rigidBody->allowSleep;
        settings.mAllowedDOFs = AllowedDOFsOf(*rigidBody);
        if (rigidBody->mass > 0.f)
        {
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = rigidBody->mass;
        }

        // A velocity written before the body existed — a projectile spawned
        // moving — is where it starts.
        if (const BodyState *state = scene.Get<BodyState>(entity); state != nullptr)
        {
            settings.mLinearVelocity = ToJoltVector(state->linearVelocity);
            settings.mAngularVelocity = ToJoltVector(state->angularVelocity);
        }
    }

    // A kinematic sensor is activated and then, unless it may sleep, never
    // sleeps on its own, which is what lets it find bodies already at rest.
    const JPH::BodyID id = physicsSystem.GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::Activate);
    if (id.IsInvalid())
    {
        Core::Log::Error("PhysicsWorld: entity {} (gen {}) gets no body - the world holds at most {} bodies.",
                         entity.index, entity.generation, maxBodies);
        return;
    }

    BodySlot &slot = SlotAt(entity);
    slot = BodySlot{};
    slot.collider = collider;
    slot.rigidBody = rigidBody != nullptr ? *rigidBody : RigidBody{};
    slot.stateTick = scene.ChangeTick<BodyState>(entity);
    slot.worldScale = worldScale;
    slot.body = id;
    slot.filter = filter;
    slot.generation = entity.generation;
    slot.motion = motion;
    slot.kind = SlotKind::Body;
    StampTransform(entity);
    if (motion != BodyMotion::Static)
    {
        Follow(entity.index);
    }

    // A static sensor is told about bodies that touch it, and a sleeping body
    // touches nothing. Waking whatever it now encloses is what lets it report the
    // things that were already sitting there when it appeared.
    if (sensor && motion == BodyMotion::Static)
    {
        WakeInside(id);
    }
}

void PhysicsWorld::Impl::EditBody(ECS::Entity entity, const Collider &collider, const RigidBody *rigidBody)
{
    BodySlot &slot = *SlotFor(entity);

    // A static body is built without a motion block, so gaining or losing one is
    // a new body.
    const BodyMotion motion = MotionOf(rigidBody);
    if ((motion == BodyMotion::Static) != (slot.motion == BodyMotion::Static))
    {
        DestroySlot(entity.index);
        CreateBody(entity, collider, rigidBody);
        return;
    }

    JPH::BodyInterface &bodies = physicsSystem.GetBodyInterface();
    const bool moving = motion != BodyMotion::Static;
    const JPH::EActivation wake = moving ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;
    const RigidBody tuning = rigidBody != nullptr ? *rigidBody : RigidBody{};

    if (!SameShape(collider, slot.collider))
    {
        bodies.SetShape(slot.body, MakeColliderShape(collider, slot.worldScale), /*inUpdateMassProperties=*/ true,
                        wake);
    }
    if (collider.friction != slot.collider.friction)
    {
        bodies.SetFriction(slot.body, collider.friction);
    }
    if (collider.restitution != slot.collider.restitution)
    {
        bodies.SetRestitution(slot.body, collider.restitution);
    }

    const CollisionFilter filter{collider.collidesWith, collider.channel};
    if (!SameFilter(filter, slot.filter) || motion != slot.motion)
    {
        bodies.SetObjectLayer(slot.body, LayerFor(filter, motion));
        {
            // Sensor-ness is a body flag rather than part of the layer, and Jolt
            // exposes no interface-level setter for it.
            JPH::BodyLockWrite lock(physicsSystem.GetBodyLockInterface(), slot.body);
            if (lock.Succeeded())
            {
                lock.GetBody().SetIsSensor(filter.channel == CollisionChannel::Trigger);
            }
        }
        slot.filter = filter;
    }

    if (moving)
    {
        if (motion != slot.motion)
        {
            bodies.SetMotionType(slot.body, JoltMotionOf(motion), JPH::EActivation::Activate);

            // Frozen where it is: a kinematic body moves at whatever velocity
            // it holds, and one made kinematic to stop it should stop.
            if (motion == BodyMotion::Kinematic)
            {
                bodies.SetLinearAndAngularVelocity(slot.body, JPH::Vec3::sZero(), JPH::Vec3::sZero());
            }
        }
        if (tuning.ccd != slot.rigidBody.ccd)
        {
            bodies.SetMotionQuality(slot.body, MotionQualityOf(tuning));
        }
        if (tuning.gravityScale != slot.rigidBody.gravityScale)
        {
            bodies.SetGravityFactor(slot.body, tuning.gravityScale);
        }
        {
            JPH::BodyLockWrite lock(physicsSystem.GetBodyLockInterface(), slot.body);
            if (lock.Succeeded())
            {
                JPH::Body &body = lock.GetBody();
                body.GetMotionProperties()->SetLinearDamping(tuning.linearDamping);
                body.GetMotionProperties()->SetAngularDamping(tuning.angularDamping);
                body.SetAllowSleeping(tuning.allowSleep);
            }
        }

        // After SetShape, which resets the mass to the shape's own.
        ApplyMass(slot.body, tuning);
        bodies.ActivateBody(slot.body);
        Follow(entity.index);
    }

    slot.collider = collider;
    slot.rigidBody = tuning;
    slot.motion = motion;
}

void PhysicsWorld::Impl::ApplyMass(const JPH::BodyID &body, const RigidBody &rigidBody)
{
    JPH::BodyLockWrite lock(physicsSystem.GetBodyLockInterface(), body);
    if (!lock.Succeeded() || !lock.GetBody().IsDynamic())
    {
        return;
    }
    JPH::MassProperties mass = lock.GetBody().GetShape()->GetMassProperties();
    if (rigidBody.mass > 0.f)
    {
        mass.ScaleToMass(rigidBody.mass);
    }
    lock.GetBody().GetMotionProperties()->SetMassProperties(AllowedDOFsOf(rigidBody), mass);
}

void PhysicsWorld::Impl::PushBodyState(ECS::Entity entity)
{
    BodySlot *slot = SlotFor(entity);
    if (slot == nullptr || slot->kind != SlotKind::Body || slot->motion == BodyMotion::Static)
    {
        return;
    }

    // This world's own write, or the one it already pushed.
    const uint64_t tick = scene.ChangeTick<BodyState>(entity);
    if (tick == slot->stateTick)
    {
        return;
    }
    slot->stateTick = tick;

    const BodyState *state = scene.Get<BodyState>(entity);
    if (state == nullptr)
    {
        return;
    }

    // Activated first: a body asleep on a surface ignores velocity written
    // while it sleeps.
    JPH::BodyInterface &bodies = physicsSystem.GetBodyInterface();
    bodies.ActivateBody(slot->body);
    bodies.SetLinearAndAngularVelocity(slot->body, ToJoltVector(state->linearVelocity),
                                       ToJoltVector(state->angularVelocity));
    Follow(entity.index);
}

void PhysicsWorld::Impl::WarnOnClampedScale(ECS::Entity entity, ColliderShape shape, glm::vec3 scale) const
{
    if (glm::all(glm::lessThan(glm::abs(ClampedShapeScale(shape, scale) - scale), glm::vec3(kScaleClampTolerance))))
    {
        return;
    }
    Core::Log::Warn("PhysicsWorld: entity {} (gen {}) has a round collider at a scale it cannot take ({}, {}, {}); "
                    "it is built at the nearest one it can.",
                    entity.index, entity.generation, scale.x, scale.y, scale.z);
}

void PhysicsWorld::Impl::PushTransform(ECS::Entity entity, float stepTime)
{
    BodySlot *slot = SlotFor(entity);
    if (slot == nullptr)
    {
        return;
    }

    // This world's own write, or the one it already pushed.
    const uint64_t tick = scene.ChangeTick<ECS::Transform>(entity);
    if (tick == slot->stamp.tick)
    {
        return;
    }

    const ECS::Transform &transform = *scene.Get<ECS::Transform>(entity);
    const bool moved = transform.position != slot->stamp.position;
    const bool turned = transform.rotation != slot->stamp.rotation;
    const bool scaled = transform.scale != slot->stamp.scale;
    const Pose world = WorldPoseOf(scene, entity, transform);
    JPH::BodyInterface &bodies = physicsSystem.GetBodyInterface();

    if (slot->kind == SlotKind::Character)
    {
        // A character's rotation is the way it faces, which the sweep does not
        // use, and the look systems write it every frame. Only a move places it.
        if (moved)
        {
            CharacterRecord &record = *FindCharacter(entity);
            record.character->SetPosition(ToJolt(world.position));
            bodies.SetPosition(slot->body, ToJolt(world.position), JPH::EActivation::Activate);

            const FilterLayerFilter layerFilter{layers, record.queryFilter};
            record.character->RefreshContacts({}, layerFilter, {}, {}, tempAlloc);
        }
        StampTransform(entity);
        return;
    }

    const JPH::EActivation wake =
        slot->motion == BodyMotion::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate;
    if (scaled)
    {
        slot->worldScale = WorldScaleOf(scene, entity, transform);
        WarnOnClampedScale(entity, slot->collider.shape, slot->worldScale);
        bodies.SetShape(slot->body, MakeColliderShape(slot->collider, slot->worldScale),
                        /*inUpdateMassProperties=*/ true, wake);
        if (slot->motion != BodyMotion::Static)
        {
            ApplyMass(slot->body, slot->rigidBody);
        }
    }

    if (moved || turned)
    {
        const JPH::RVec3 position = ToJolt(world.position);
        const JPH::Quat rotation = ToJolt(world.rotation);

        if (slot->motion == BodyMotion::Kinematic && stepTime > 0.f)
        {
            // Swept over the coming step rather than placed: the velocity that
            // carries it there is what pushes resting bodies along and what a
            // character standing on it reads to ride it.
            bodies.MoveKinematic(slot->body, position, rotation, stepTime);
            sweptThisStep.push_back(entity.index);
        }
        else if (slot->motion == BodyMotion::Static)
        {
            // Whatever rested against it where it was, and whatever it now lands
            // on, has to be re-tested: a sleeping body generates no contacts, so
            // it would float where the wall used to be, and a sensor would keep
            // reporting what it left.
            JPH::AABox touched = BoundsOf(slot->body);
            bodies.SetPositionAndRotation(slot->body, position, rotation, JPH::EActivation::DontActivate);
            touched.Encapsulate(BoundsOf(slot->body));
            WakeInside(touched, slot->filter);
        }
        else
        {
            // Placed with its velocity kept: a write says where, not how fast.
            bodies.SetPositionAndRotation(slot->body, position, rotation, JPH::EActivation::Activate);
        }

        if (slot->motion != BodyMotion::Static)
        {
            Follow(entity.index);
        }
    }

    StampTransform(entity);
}

void PhysicsWorld::Impl::StopFinishedSweeps()
{
    JPH::BodyInterface &bodies = physicsSystem.GetBodyInterface();
    for (const std::uint32_t index : sweptLastStep)
    {
        if (std::find(sweptThisStep.begin(), sweptThisStep.end(), index) != sweptThisStep.end())
        {
            continue;
        }
        const BodySlot &slot = slots[index];
        if (slot.kind == SlotKind::Body && slot.motion == BodyMotion::Kinematic)
        {
            bodies.SetLinearAndAngularVelocity(slot.body, JPH::Vec3::sZero(), JPH::Vec3::sZero());
        }
    }
    sweptLastStep.swap(sweptThisStep);
    sweptThisStep.clear();
}

void PhysicsWorld::Impl::ApplyAskedStances()
{
    for (std::pair<const std::uint32_t, CharacterRecord> &entry : characters)
    {
        CharacterRecord &record = entry.second;
        const CharacterIntent *intent = scene.Get<CharacterIntent>(record.entity);
        if (intent == nullptr || intent->stance == record.stance)
        {
            continue;
        }
        // Asked every reconcile rather than on the edge of a change: standing
        // up under something low fails, and retrying is what lets a character
        // stand by itself once it has walked clear.
        if (ApplyStance(record, intent->stance))
        {
            WriteCharacterState(record.entity, BuildCharacterState(record));
        }
    }
}

void PhysicsWorld::Impl::StampTransform(ECS::Entity entity)
{
    BodySlot *slot = SlotFor(entity);
    const ECS::Transform *transform = scene.Get<ECS::Transform>(entity);
    if (slot == nullptr || transform == nullptr)
    {
        return;
    }
    slot->stamp =
        TransformStamp{transform->rotation, transform->position, transform->scale,
                       scene.ChangeTick<ECS::Transform>(entity)};
}

void PhysicsWorld::Impl::DestroySlot(std::uint32_t index)
{
    BodySlot &slot = slots[index];
    if (slot.kind == SlotKind::Empty)
    {
        return;
    }

    // Everything this body was touching has stopped touching it, and the next
    // step cannot say so — the body will be gone. Build those Exits now and let
    // the next Update() deliver them.
    EmitExitsFor(slot.body, pendingExits);

    if (slot.kind == SlotKind::Character)
    {
        // The CharacterVirtual owns its inner body and destroys it with itself.
        const std::map<std::uint32_t, CharacterRecord>::iterator it = characters.find(index);
        if (it != characters.end())
        {
            characterVsCharacter.Remove(it->second.character);
            characters.erase(it);
        }
    }
    else
    {
        JPH::BodyInterface &bodies = physicsSystem.GetBodyInterface();
        bodies.RemoveBody(slot.body);
        bodies.DestroyBody(slot.body);
        ForgetBodyState(ECS::Entity{index, slot.generation});
    }

    if (slot.followed)
    {
        std::erase(awake, index);
    }
    slot = BodySlot{};
}

void PhysicsWorld::Impl::ForgetBodyState(ECS::Entity entity)
{
    if (!scene.IsAlive(entity))
    {
        return;
    }
    const BodyState *state = scene.Get<BodyState>(entity);
    if (state == nullptr ||
        (state->linearVelocity == glm::vec3(0.f) && state->angularVelocity == glm::vec3(0.f) && !state->asleep))
    {
        return;
    }
    *scene.GetMut<BodyState>(entity) = BodyState{};
}

void PhysicsWorld::Impl::DestroyAll()
{
    // Characters first: each owns an inner body that it destroys itself.
    for (std::pair<const std::uint32_t, CharacterRecord> &entry : characters)
    {
        characterVsCharacter.Remove(entry.second.character);
    }
    characters.clear();

    JPH::BodyInterface &bodies = physicsSystem.GetBodyInterface();
    for (std::uint32_t index = 0; index < slots.size(); ++index)
    {
        const BodySlot &slot = slots[index];
        if (slot.kind == SlotKind::Body)
        {
            bodies.RemoveBody(slot.body);
            bodies.DestroyBody(slot.body);
            ForgetBodyState(ECS::Entity{index, slot.generation});
        }
    }
    slots.clear();
    awake.clear();
    sweptThisStep.clear();
    sweptLastStep.clear();

    // Every pair and event names bodies that no longer exist. No Exit is emitted
    // for what was touching: nothing survives that could act on one, and a world
    // being emptied is not a world where things left each other.
    pairs.clear();
    touchedThisStep.clear();
    removedThisStep.clear();
    activated.clear();
    activatedAtStart.clear();
    pendingExits.clear();
    events.clear();

    // Asked of bodies that are gone; a body built again starts at rest.
    requests.clear();
}

} // namespace Assisi::Physics
