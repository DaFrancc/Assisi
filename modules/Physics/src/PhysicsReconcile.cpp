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
#include <Assisi/Physics/ColliderRole.hpp>

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
    for (const ECS::Entity entity : scratchRemoved)
    {
        // Gone, even when it is back already: an entity revived at its own
        // handle or a Collider removed and added again is a new life, so it gets
        // a new body rather than the old one's motion.
        if (SlotFor(entity) != nullptr)
        {
            DestroySlot(entity.index);
        }
        if (!scene.IsAlive(entity))
        {
            ForgetIgnoredPairs(entity);
        }
    }

    // Everything whose own slot, or whose parts' roles, may have changed: its
    // physics components, its Parent link, or anything just removed. A RigidBody
    // or a Character gained or lost turns the colliders below it into parts or
    // back, and a Parent changed moves them between owners, so each entity
    // brings every collider listed below it.
    scratchChanged.clear();
    scratchChanged.insert(scratchChanged.end(), scratchRemoved.begin(), scratchRemoved.end());
    scene.ChangedSince<Collider>(changeCursor, scratchChanged);
    scene.ChangedSince<RigidBody>(changeCursor, scratchChanged);
    scene.ChangedSince<Character>(changeCursor, scratchChanged);
    scene.ChangedSince<ECS::Parent>(changeCursor, scratchChanged);
    complete = scene.RemovedSince<RigidBody>(changeCursor, scratchChanged) && complete;
    complete = scene.RemovedSince<ECS::Parent>(changeCursor, scratchChanged) && complete;
    if (!complete)
    {
        // The log no longer reaches back to the last reconcile, so what went is
        // unknown: every slot is checked against the scene instead.
        for (std::uint32_t index = 0; index < slots.size(); ++index)
        {
            if (slots[index].kind != SlotKind::Empty)
            {
                scratchChanged.push_back(ECS::Entity{index, slots[index].generation});
            }
        }
    }
    AppendPartsBelow(scratchChanged);

    // An entity listed for several reasons is synced once.
    std::sort(scratchChanged.begin(), scratchChanged.end(), EntityBefore);
    scratchChanged.erase(std::unique(scratchChanged.begin(), scratchChanged.end()), scratchChanged.end());
    for (const ECS::Entity entity : scratchChanged)
    {
        SyncEntity(entity);
    }
    SyncOwners();

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
        RePlacePiecesBelow(entity);
    }
    // An owner whose scale changed is built again.
    SyncOwners();
    PlaceFollowers();

    StopFinishedSweeps();
    ApplyAskedStances();

    changeCursor = now;
}

void PhysicsWorld::Impl::SyncOwners()
{
    // After every part is in line, so an owner is built once from all of them
    // whichever order they were synced in.
    std::sort(ownersToSync.begin(), ownersToSync.end(), EntityBefore);
    ownersToSync.erase(std::unique(ownersToSync.begin(), ownersToSync.end()), ownersToSync.end());
    for (const ECS::Entity owner : ownersToSync)
    {
        SyncOwner(owner, std::find(ownersToReshape.begin(), ownersToReshape.end(), owner) != ownersToReshape.end());
    }
    ownersToSync.clear();
    ownersToReshape.clear();
}

PhysicsWorld::Impl::WantedSlot PhysicsWorld::Impl::WantedFor(ECS::Entity entity) const
{
    if (!scene.IsAlive(entity) || !scene.Has<ECS::Transform>(entity))
    {
        return WantedSlot{};
    }
    if (scene.Has<Character>(entity))
    {
        return WantedSlot{ECS::NullEntity, SlotKind::Character};
    }
    // Built from its own Collider and its pieces once they are known.
    if (scene.Has<RigidBody>(entity))
    {
        return WantedSlot{ECS::NullEntity, SlotKind::Body};
    }
    if (UsableCollider(entity) == nullptr)
    {
        return WantedSlot{};
    }

    const ColliderPlacement placement = ResolveColliderPlacement(scene, entity);
    switch (placement.role)
    {
    case ColliderRole::Piece:
        return WantedSlot{placement.owner, SlotKind::Piece};
    case ColliderRole::Follower:
        return WantedSlot{placement.owner, SlotKind::Follower};
    case ColliderRole::Own:
    case ColliderRole::Static:
    case ColliderRole::Count:
        break;
    }
    return WantedSlot{ECS::NullEntity, SlotKind::Body};
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

    const WantedSlot wanted = WantedFor(entity);
    if (held != nullptr && (held->kind != wanted.kind || held->owner != wanted.owner))
    {
        DestroySlot(entity.index);
        held = nullptr;
    }

    switch (wanted.kind)
    {
    case SlotKind::Body:
        if (scene.Has<RigidBody>(entity))
        {
            ownersToSync.push_back(entity);
        }
        else
        {
            SyncStatic(entity, *scene.Get<Collider>(entity));
        }
        break;
    case SlotKind::Piece:
        if (held == nullptr)
        {
            CreatePiece(entity, *scene.Get<Collider>(entity), wanted.owner);
        }
        else
        {
            EditPiece(entity, *scene.Get<Collider>(entity));
        }
        break;
    case SlotKind::Follower:
        if (held == nullptr)
        {
            CreateFollower(entity, *scene.Get<Collider>(entity), wanted.owner);
        }
        else
        {
            EditFollower(entity, *scene.Get<Collider>(entity));
        }
        break;
    case SlotKind::Character:
        if (held == nullptr)
        {
            CreateCharacter(entity, *scene.Get<Character>(entity));
        }
        else
        {
            EditCharacter(entity, *scene.Get<Character>(entity));
        }
        break;
    case SlotKind::Empty:
    case SlotKind::Count:
        break;
    }

    if (scene.Has<Collider>(entity))
    {
        RegisterAncestors(entity);
    }
}

void PhysicsWorld::Impl::SyncStatic(ECS::Entity entity, const Collider &collider)
{
    const BodySlot *held = SlotFor(entity);
    const bool reshape = held == nullptr || held->motion != BodyMotion::Static || !SameShape(collider, held->collider);
    JPH::ShapeRefC shape;
    if (reshape)
    {
        const ECS::Transform &transform = *scene.Get<ECS::Transform>(entity);
        const glm::vec3 worldScale = WorldScaleOf(scene, entity, transform);
        WarnOnClampedScale(entity, collider, worldScale);
        shape = MakeColliderShape(collider, worldScale, entity);
        if (shape == nullptr)
        {
            return;
        }
    }
    if (held == nullptr)
    {
        CreateBody(entity, shape, collider, nullptr);
    }
    else
    {
        EditBody(entity, shape, collider, nullptr);
    }
    if (BodySlot *built = SlotFor(entity); built != nullptr)
    {
        built->ownCollider = true;
    }
}

void PhysicsWorld::Impl::CreateBody(ECS::Entity entity, const JPH::ShapeRefC &shape, const Collider &face,
                                    const RigidBody *rigidBody)
{
    const ECS::Transform &transform = *scene.Get<ECS::Transform>(entity);
    const Pose pose = WorldPoseOf(scene, entity, transform);
    const glm::vec3 worldScale = WorldScaleOf(scene, entity, transform);
    if (!IsFinite(pose) || !IsFinite(worldScale))
    {
        Core::Log::Error("PhysicsWorld: entity {} (gen {}) gets no body - its Transform is not a number.",
                         entity.index, entity.generation);
        return;
    }
    const CollisionFilter filter{face.collidesWith, face.channel};
    const BodyMotion motion = MotionOf(rigidBody);
    const bool sensor = face.channel == CollisionChannel::Trigger;

    JPH::BodyCreationSettings settings(shape, ToJolt(pose.position), ToJolt(pose.rotation), JoltMotionOf(motion),
                                       LayerFor(filter, motion));
    settings.mIsSensor = sensor;
    settings.mUserData = UserDataOf(entity);
    settings.mFriction = face.friction;
    settings.mRestitution = face.restitution;

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
        if (motion == BodyMotion::Kinematic)
        {
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
            settings.mMassPropertiesOverride = MassOfShape(*shape);
        }
        else if (rigidBody->mass > 0.f)
        {
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = rigidBody->mass;
        }

        // A velocity written before the body existed — a projectile spawned
        // moving — is where it starts.
        if (const BodyState *state = scene.Get<BodyState>(entity);
            state != nullptr && IsFinite(state->linearVelocity) && IsFinite(state->angularVelocity))
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
    slot.collider = face;
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

void PhysicsWorld::Impl::EditBody(ECS::Entity entity, const JPH::ShapeRefC &shape, const Collider &face,
                                  const RigidBody *rigidBody)
{
    BodySlot &slot = *SlotFor(entity);

    // A static body is built without a motion block, so gaining or losing one is
    // a new body.
    const BodyMotion motion = MotionOf(rigidBody);
    if ((motion == BodyMotion::Static) != (slot.motion == BodyMotion::Static))
    {
        DestroySlot(entity.index);
        CreateBody(entity, shape, face, rigidBody);
        return;
    }

    JPH::BodyInterface &bodies = physicsSystem.GetBodyInterface();
    const bool moving = motion != BodyMotion::Static;
    const JPH::EActivation wake = moving ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;
    const RigidBody tuning = rigidBody != nullptr ? *rigidBody : RigidBody{};

    // The mass is set below: a kinematic body's shape may have none of its own.
    if (shape != nullptr)
    {
        bodies.SetShape(slot.body, shape, /*inUpdateMassProperties=*/ false, wake);
    }
    if (face.friction != slot.collider.friction)
    {
        bodies.SetFriction(slot.body, face.friction);
    }
    if (face.restitution != slot.collider.restitution)
    {
        bodies.SetRestitution(slot.body, face.restitution);
    }
    SetBodyFilter(slot, CollisionFilter{face.collidesWith, face.channel}, motion);

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

        // After SetShape and SetMotionType, so it weighs the shape and motion
        // the body now has.
        ApplyMass(slot.body, tuning);
        bodies.ActivateBody(slot.body);
        Follow(entity.index);
    }

    slot.collider = face;
    slot.rigidBody = tuning;
    slot.motion = motion;
}

void PhysicsWorld::Impl::SetBodyFilter(BodySlot &slot, CollisionFilter filter, BodyMotion motion)
{
    if (SameFilter(filter, slot.filter) && motion == slot.motion)
    {
        return;
    }
    physicsSystem.GetBodyInterface().SetObjectLayer(slot.body, LayerFor(filter, motion));
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

void PhysicsWorld::Impl::ApplyMass(const JPH::BodyID &body, const RigidBody &rigidBody)
{
    JPH::BodyLockWrite lock(physicsSystem.GetBodyLockInterface(), body);
    if (!lock.Succeeded() || lock.GetBody().IsStatic())
    {
        return;
    }
    JPH::MassProperties mass = MassOfShape(*lock.GetBody().GetShape());
    if (lock.GetBody().IsDynamic() && rigidBody.mass > 0.f)
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

    JPH::BodyInterface &bodies = physicsSystem.GetBodyInterface();
    if (!IsFinite(state->linearVelocity) || !IsFinite(state->angularVelocity))
    {
        // Overwritten with what the body is doing, so the refusal is not met
        // again on the next reconcile.
        Core::Log::Warn("PhysicsWorld: entity {} (gen {}) was given a velocity that is not a number; it keeps the "
                        "one it had.",
                        entity.index, entity.generation);
        const JPH::Vec3 linear = bodies.GetLinearVelocity(slot->body);
        const JPH::Vec3 angular = bodies.GetAngularVelocity(slot->body);
        WriteBodyState(entity, BodyState{.linearVelocity = glm::vec3(linear.GetX(), linear.GetY(), linear.GetZ()),
                                         .angularVelocity = glm::vec3(angular.GetX(), angular.GetY(), angular.GetZ()),
                                         .asleep = !bodies.IsActive(slot->body)});
        return;
    }

    // Activated first: a body asleep on a surface ignores velocity written
    // while it sleeps.
    bodies.ActivateBody(slot->body);
    bodies.SetLinearAndAngularVelocity(slot->body, ToJoltVector(state->linearVelocity),
                                       ToJoltVector(state->angularVelocity));
    Follow(entity.index);
}

void PhysicsWorld::Impl::WarnOnClampedScale(ECS::Entity entity, const Collider &collider, glm::vec3 scale) const
{
    if (glm::all(glm::lessThan(glm::abs(ClampedScale(collider, scale) - scale), glm::vec3(kScaleClampTolerance))))
    {
        return;
    }
    Core::Log::Warn("PhysicsWorld: entity {} (gen {}) has a round collider, or a model with a round piece, at a "
                    "scale it cannot take ({}, {}, {}); it is built at the nearest one it can.",
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
    if (!IsFinite(transform.position) || !IsFinite(transform.rotation) || !IsFinite(transform.scale))
    {
        // Put back as this world last had it, so the next reconcile has
        // nothing to refuse again and nothing draws the entity at NaN.
        Core::Log::Warn("PhysicsWorld: entity {} (gen {}) was given a Transform that is not a number; it is put back "
                        "where it was.",
                        entity.index, entity.generation);
        ECS::Transform &restored = *scene.GetMut<ECS::Transform>(entity);
        restored.position = slot->stamp.position;
        restored.rotation = slot->stamp.rotation;
        restored.scale = slot->stamp.scale;
        StampTransform(entity);
        return;
    }

    const bool moved = transform.position != slot->stamp.position;
    const bool turned = transform.rotation != slot->stamp.rotation;
    const bool scaled = transform.scale != slot->stamp.scale;
    const Pose world = WorldPoseOf(scene, entity, transform);
    JPH::BodyInterface &bodies = physicsSystem.GetBodyInterface();

    // A piece moves inside its owner's shape; a follower is put where its
    // Transform now composes to at the end of the reconcile, with the rest.
    if (slot->kind == SlotKind::Piece || slot->kind == SlotKind::Follower)
    {
        if (slot->kind == SlotKind::Piece)
        {
            RePlacePiece(entity);
        }
        StampTransform(entity);
        return;
    }

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
            const OwnerBodiesFilter bodyFilter{*this, entity, /*exceptions=*/ true};
            record.character->RefreshContacts({}, layerFilter, bodyFilter, {}, tempAlloc);
        }
        StampTransform(entity);
        return;
    }

    const JPH::EActivation wake =
        slot->motion == BodyMotion::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate;
    if (scaled && slot->motion != BodyMotion::Static)
    {
        // Its own collider and every piece are scaled with it, so its whole
        // shape is built again once the reconcile's Transforms are all in.
        ownersToSync.push_back(entity);
        ownersToReshape.push_back(entity);
    }
    else if (scaled)
    {
        slot->worldScale = WorldScaleOf(scene, entity, transform);
        WarnOnClampedScale(entity, slot->collider, slot->worldScale);
        if (const JPH::ShapeRefC shape = MakeColliderShape(slot->collider, slot->worldScale, entity);
            shape != nullptr)
        {
            // A static body has no mass to update.
            bodies.SetShape(slot->body, shape, /*inUpdateMassProperties=*/ false, wake);
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
    const ECS::Entity entity{index, slot.generation};
    if (slot.kind != SlotKind::Piece)
    {
        EmitExitsFor(slot.body, pendingExits);
    }

    switch (slot.kind)
    {
    case SlotKind::Character:
    {
        // The CharacterVirtual owns its inner body and destroys it with itself.
        const std::map<std::uint32_t, CharacterRecord>::iterator it = characters.find(index);
        if (it != characters.end())
        {
            characterVsCharacter.Remove(it->second.character);
            characters.erase(it);
        }
        break;
    }
    case SlotKind::Piece:
        // Its owner is built again without it.
        ownersToSync.push_back(slot.owner);
        break;
    case SlotKind::Body:
    case SlotKind::Follower:
    {
        JPH::BodyInterface &bodies = physicsSystem.GetBodyInterface();
        bodies.RemoveBody(slot.body);
        bodies.DestroyBody(slot.body);
        if (slot.kind == SlotKind::Body)
        {
            ForgetBodyState(entity);
        }
        else
        {
            std::erase(followers, index);
        }
        break;
    }
    case SlotKind::Empty:
    case SlotKind::Count:
        break;
    }

    if (slot.followed)
    {
        std::erase(awake, index);
    }
    UnregisterAncestors(entity, slot);
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
        if (slot.kind == SlotKind::Body || slot.kind == SlotKind::Follower)
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
    partsBelow.clear();
    followers.clear();
    ownersToSync.clear();
    ownersToReshape.clear();

    // Exceptions name entities, and after a clear or a rebuild those handles
    // name other things or nothing that was meant.
    ignoredPairs.clear();

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
