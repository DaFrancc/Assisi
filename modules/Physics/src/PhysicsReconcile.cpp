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

PhysicsWorld::ColliderShapeDesc ShapeOf(const RigidBodyDescriptor &descriptor)
{
    return PhysicsWorld::ColliderShapeDesc{.shape = descriptor.shape,
                                           .halfExtents = descriptor.halfExtents,
                                           .radius = descriptor.radius,
                                           .halfHeight = descriptor.halfHeight};
}

/// The motion a descriptor asks for. A sensor that fell under gravity would
/// leave the volume it was authored as, so a non-static sensor is kinematic: it
/// never sleeps on its own, which is what lets it find bodies already at rest.
BodyMotion MotionOf(const RigidBodyDescriptor &descriptor)
{
    if (descriptor.isStatic)
    {
        return BodyMotion::Static;
    }
    return descriptor.channel == CollisionChannel::Trigger ? BodyMotion::Kinematic : BodyMotion::Dynamic;
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
    bool complete = scene.RemovedSince<RigidBodyDescriptor>(changeCursor, scratchRemoved);
    complete = scene.RemovedSince<CharacterDescriptor>(changeCursor, scratchRemoved) && complete;
    complete = scene.RemovedSince<ECS::Transform>(changeCursor, scratchRemoved) && complete;
    if (complete)
    {
        // Gone, even when it is back already: an entity revived at its own handle
        // or a descriptor removed and added again is a new life, so it gets a new
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

    scratchChanged.clear();
    scene.ChangedSince<RigidBodyDescriptor>(changeCursor, scratchChanged);
    scene.ChangedSince<CharacterDescriptor>(changeCursor, scratchChanged);
    for (const ECS::Entity entity : scratchChanged)
    {
        SyncEntity(entity);
    }

    scratchChanged.clear();
    scene.ChangedSince<ECS::Transform>(changeCursor, scratchChanged);
    for (const ECS::Entity entity : scratchChanged)
    {
        PushTransform(entity, stepTime);
    }

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
    const CharacterDescriptor *character = nullptr;
    const RigidBodyDescriptor *body = nullptr;
    if (scene.IsAlive(entity) && scene.Has<ECS::Transform>(entity))
    {
        character = scene.Get<CharacterDescriptor>(entity);
        body = scene.Get<RigidBodyDescriptor>(entity);
        if (character != nullptr)
        {
            wanted = SlotKind::Character;
        }
        else if (body != nullptr)
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
        if (held == nullptr)
        {
            CreateBody(entity, *body);
        }
        else
        {
            EditBody(entity, *body);
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

void PhysicsWorld::Impl::CreateBody(ECS::Entity entity, const RigidBodyDescriptor &descriptor)
{
    const ECS::Transform &transform = *scene.Get<ECS::Transform>(entity);
    const Pose pose = WorldPoseOf(scene, entity, transform);
    const glm::vec3 worldScale = WorldScaleOf(scene, entity, transform);
    const ColliderShapeDesc shape = ShapeOf(descriptor);
    const CollisionFilter filter{descriptor.collidesWith, descriptor.channel};
    const BodyMotion motion = MotionOf(descriptor);
    const bool sensor = descriptor.channel == CollisionChannel::Trigger;

    JPH::BodyCreationSettings settings(MakeScaledShape(shape, worldScale), ToJolt(pose.position),
                                       ToJolt(pose.rotation), JoltMotionOf(motion), PackLayer(filter, motion));

    // A static body carries no motion block. Making it move is an edit to the
    // descriptor, which builds a new body rather than converting this one.
    settings.mAllowDynamicOrKinematic = motion != BodyMotion::Static;
    settings.mIsSensor = sensor;
    settings.mUserData = UserDataOf(entity);
    if (motion != BodyMotion::Static)
    {
        settings.mMotionQuality = descriptor.enableCCD ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
    }

    // A kinematic sensor is activated and then never sleeps on its own, which is
    // what lets it find bodies that are already at rest.
    const JPH::BodyID id = physicsSystem.GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::Activate);
    if (id.IsInvalid())
    {
        Core::Log::Error("PhysicsWorld: entity {} (gen {}) gets no body - the world holds at most {} bodies.",
                         entity.index, entity.generation, maxBodies);
        return;
    }

    BodySlot &slot = SlotAt(entity);
    slot = BodySlot{};
    slot.shape = shape;
    slot.worldScale = worldScale;
    slot.body = id;
    slot.filter = filter;
    slot.generation = entity.generation;
    slot.motion = motion;
    slot.kind = SlotKind::Body;
    slot.ccd = descriptor.enableCCD;
    StampTransform(entity);

    // A static sensor is told about bodies that touch it, and a sleeping body
    // touches nothing. Waking whatever it now encloses is what lets it report the
    // things that were already sitting there when it appeared.
    if (sensor && motion == BodyMotion::Static)
    {
        WakeInside(id);
    }
}

void PhysicsWorld::Impl::EditBody(ECS::Entity entity, const RigidBodyDescriptor &descriptor)
{
    BodySlot &slot = *SlotFor(entity);

    // Static bodies are built without a motion block and a sensor's motion
    // follows its channel, so a change of motion is a new body.
    if (MotionOf(descriptor) != slot.motion)
    {
        DestroySlot(entity.index);
        CreateBody(entity, descriptor);
        return;
    }

    JPH::BodyInterface &bodies = physicsSystem.GetBodyInterface();
    const JPH::EActivation wake =
        slot.motion == BodyMotion::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate;

    const ColliderShapeDesc shape = ShapeOf(descriptor);
    if (!(shape == slot.shape))
    {
        bodies.SetShape(slot.body, MakeScaledShape(shape, slot.worldScale), /*inUpdateMassProperties=*/ true, wake);
        slot.shape = shape;
    }

    const CollisionFilter filter{descriptor.collidesWith, descriptor.channel};
    if (!SameFilter(filter, slot.filter))
    {
        bodies.SetObjectLayer(slot.body, PackLayer(filter, slot.motion));
        {
            // Sensor-ness is a body flag rather than part of the layer, and Jolt
            // exposes no interface-level setter for it.
            JPH::BodyLockWrite lock(physicsSystem.GetBodyLockInterface(), slot.body);
            if (lock.Succeeded())
            {
                lock.GetBody().SetIsSensor(filter.channel == CollisionChannel::Trigger);
            }
        }
        if (slot.motion != BodyMotion::Static)
        {
            bodies.ActivateBody(slot.body);
        }
        slot.filter = filter;
    }

    if (slot.motion != BodyMotion::Static && descriptor.enableCCD != slot.ccd)
    {
        bodies.SetMotionQuality(slot.body,
                                descriptor.enableCCD ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete);
        slot.ccd = descriptor.enableCCD;
    }
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

            const FilterLayerFilter layerFilter{record.queryFilter};
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
        bodies.SetShape(slot->body, MakeScaledShape(slot->shape, slot->worldScale), /*inUpdateMassProperties=*/ true,
                        wake);
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
    }

    if (slot.followed)
    {
        std::erase(awake, index);
    }
    slot = BodySlot{};
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
    for (const BodySlot &slot : slots)
    {
        if (slot.kind == SlotKind::Body)
        {
            bodies.RemoveBody(slot.body);
            bodies.DestroyBody(slot.body);
        }
    }
    slots.clear();
    awake.clear();

    // Every pair and event names bodies that no longer exist. No Exit is emitted
    // for what was touching: nothing survives that could act on one, and a world
    // being emptied is not a world where things left each other.
    pairs.clear();
    touchedThisStep.clear();
    pendingExits.clear();
    events.clear();
}

} // namespace Assisi::Physics
