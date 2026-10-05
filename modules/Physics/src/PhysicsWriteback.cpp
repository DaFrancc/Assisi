/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file PhysicsWriteback.cpp
/// @brief Getting simulated poses back out: after every step, each moving
///        body's pose is written into its Transform, which is the simulation
///        pose, and its velocities into its BodyState or CharacterState.
///        Smoothing between steps for display is the ECS's render blend.
///
/// Only bodies the simulation woke are followed, until each is written at rest,
/// so the cost is the number of things moving rather than the number of things.

#include "PhysicsInternal.hpp"

#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/TransformPose.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace Assisi::Physics
{

std::optional<glm::mat4> SimulationParentMatrix(const ECS::Scene &scene, ECS::Entity entity)
{
    const ECS::Parent *parent = scene.Get<ECS::Parent>(entity);
    if (parent == nullptr || parent->parent == ECS::NullEntity || !scene.Has<ECS::Transform>(parent->parent))
    {
        return std::nullopt;
    }
    const ECS::Transform world = ECS::WorldTransformOf(scene, parent->parent);
    return glm::translate(glm::mat4(1.f), world.position) * glm::mat4_cast(world.rotation) *
           glm::scale(glm::mat4(1.f), world.scale);
}

void PhysicsWorld::Impl::WriteBack()
{
    // The no-lock interface: the step is over, and every read below would
    // otherwise take and release a lock per body for nothing.
    const JPH::BodyInterface &bodies = physicsSystem.GetBodyInterfaceNoLock();

    // Whatever the step left awake joins the bodies being followed. Characters'
    // inner bodies are among them and are skipped: a character is written
    // through its own record below.
    JPH::BodyIDVector active;
    physicsSystem.GetActiveBodies(JPH::EBodyType::RigidBody, active);
    for (const JPH::BodyID &id : active)
    {
        const ECS::Entity entity = EntityOfUserData(bodies.GetUserData(id));
        const BodySlot *slot = SlotFor(entity);
        if (slot != nullptr && slot->kind == SlotKind::Body)
        {
            Follow(entity.index);
        }
    }

    // Ordered removal is not needed — the writes are independent of one another —
    // so a body that has come to rest is swapped out of the list.
    for (std::size_t i = 0; i < awake.size();)
    {
        const std::uint32_t index = awake[i];
        BodySlot &slot = slots[index];
        const ECS::Entity entity{index, slot.generation};
        const JPH::RVec3 position = bodies.GetPosition(slot.body);
        const JPH::Quat rotation = bodies.GetRotation(slot.body);
        const JPH::Vec3 linear = bodies.GetLinearVelocity(slot.body);
        const JPH::Vec3 angular = bodies.GetAngularVelocity(slot.body);
        const Pose pose{glm::quat(rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ()),
                        glm::vec3(position.GetX(), position.GetY(), position.GetZ())};
        const BodyState state{.linearVelocity = glm::vec3(linear.GetX(), linear.GetY(), linear.GetZ()),
                              .angularVelocity = glm::vec3(angular.GetX(), angular.GetY(), angular.GetZ()),
                              .asleep = !bodies.IsActive(slot.body)};
        if (!IsFinite(pose) || !IsFinite(state.linearVelocity) || !IsFinite(state.angularVelocity))
        {
            RestoreBody(entity);
            ++i;
            continue;
        }

        WritePose(entity, pose, /*writeRotation=*/ true);
        WriteBodyState(entity, state);
        const bool asleep = state.asleep;

        // Asleep: those writes put it exactly at rest, and there is nothing
        // more to follow until something wakes it.
        if (asleep)
        {
            slot.followed = false;
            awake[i] = awake.back();
            awake.pop_back();
            continue;
        }
        ++i;
    }

    // Characters are swept rather than solved and are never asleep, so every
    // one is written every step.
    for (std::pair<const std::uint32_t, CharacterRecord> &entry : characters)
    {
        CharacterRecord &record = entry.second;
        const JPH::RVec3 position = record.character->GetPosition();
        const JPH::Vec3 velocity = record.character->GetLinearVelocity();
        if (!IsFinite(glm::vec3(position.GetX(), position.GetY(), position.GetZ())) ||
            !IsFinite(glm::vec3(velocity.GetX(), velocity.GetY(), velocity.GetZ())))
        {
            RestoreCharacter(record);
            continue;
        }
        const Pose pose{glm::quat(1.f, 0.f, 0.f, 0.f), glm::vec3(position.GetX(), position.GetY(), position.GetZ())};
        WritePose(record.entity, pose, /*writeRotation=*/ false);
        WriteCharacterState(record.entity, BuildCharacterState(record));
    }
}

Pose PhysicsWorld::Impl::TransformPose(ECS::Entity entity) const
{
    const ECS::Transform &transform = *scene.Get<ECS::Transform>(entity);
    if (const std::optional<glm::mat4> parent = SimulationParentMatrix(scene, entity); parent.has_value())
    {
        const ECS::Transform world = ECS::PoseUnderParent(transform, *parent);
        return Pose{world.rotation, world.position};
    }
    return Pose{transform.rotation, transform.position};
}

void PhysicsWorld::Impl::RestoreBody(ECS::Entity entity)
{
    Core::Log::Error("PhysicsWorld: entity {} (gen {}) came out of the step with a pose or velocity that is not a "
                     "number; it is put back where it last was, at rest.",
                     entity.index, entity.generation);
    const BodySlot &slot = *SlotFor(entity);
    const Pose pose = TransformPose(entity);
    JPH::BodyInterface &bodies = physicsSystem.GetBodyInterfaceNoLock();
    bodies.SetPositionAndRotation(slot.body, JPH::RVec3(pose.position.x, pose.position.y, pose.position.z),
                                  JPH::Quat(pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w)
                                  .Normalized(),
                                  JPH::EActivation::DontActivate);
    bodies.SetLinearAndAngularVelocity(slot.body, JPH::Vec3::sZero(), JPH::Vec3::sZero());
    WriteBodyState(entity, BodyState{});
}

void PhysicsWorld::Impl::RestoreCharacter(CharacterRecord &record)
{
    Core::Log::Error("PhysicsWorld: character {} (gen {}) came out of the step with a position or velocity that is "
                     "not a number; it is put back where it last was, at rest.",
                     record.entity.index, record.entity.generation);
    const glm::vec3 feet = TransformPose(record.entity).position;
    const JPH::RVec3 position(feet.x, feet.y, feet.z);
    record.character->SetPosition(position);
    record.character->SetLinearVelocity(JPH::Vec3::sZero());
    physicsSystem.GetBodyInterfaceNoLock().SetPosition(SlotFor(record.entity)->body, position,
                                                       JPH::EActivation::DontActivate);
}

void PhysicsWorld::Impl::WriteBodyState(ECS::Entity entity, const BodyState &state)
{
    BodySlot *slot = SlotFor(entity);
    const BodyState *current = scene.Get<BodyState>(entity);
    if (slot == nullptr || current == nullptr)
    {
        return;
    }
    if (current->linearVelocity == state.linearVelocity && current->angularVelocity == state.angularVelocity &&
        current->asleep == state.asleep)
    {
        return;
    }
    // GetMut, so the write stamps; recorded as this world's own so the next
    // reconcile does not push it back.
    *scene.GetMut<BodyState>(entity) = state;
    slot->stateTick = scene.ChangeTick<BodyState>(entity);
}

void PhysicsWorld::Impl::WriteCharacterState(ECS::Entity entity, const CharacterState &state)
{
    const CharacterState *current = scene.Get<CharacterState>(entity);
    if (current == nullptr)
    {
        return;
    }
    // A standing crowd would otherwise rewrite every field every step.
    if (current->velocity == state.velocity && current->groundNormal == state.groundNormal &&
        current->groundVelocity == state.groundVelocity && current->groundEntity == state.groundEntity &&
        current->timeSinceGrounded == state.timeSinceGrounded && current->eyeHeight == state.eyeHeight &&
        current->ground == state.ground && current->stance == state.stance && current->canJump == state.canJump)
    {
        return;
    }
    *scene.GetMut<CharacterState>(entity) = state;
}

void PhysicsWorld::Impl::WritePose(ECS::Entity entity, Pose pose, bool writeRotation)
{
    const ECS::Transform *current = scene.Get<ECS::Transform>(entity);
    if (current == nullptr)
    {
        return;
    }

    // Jolt reports world space; a Transform under a parent is an offset *from*
    // that parent. Writing one into the other would apply the parent twice.
    if (const std::optional<glm::mat4> parent = SimulationParentMatrix(scene, entity); parent.has_value())
    {
        pose.position = glm::vec3(glm::inverse(*parent) * glm::vec4(pose.position, 1.f));
        pose.rotation = glm::normalize(glm::inverse(ECS::WorldRotationOf(*parent)) * pose.rotation);
    }

    // Nothing moved: skip the write rather than stamp a change tick for a pose
    // identical to the one already there, which would read as a change to
    // propagation, the render blend and replication alike.
    if (current->position == pose.position && (!writeRotation || current->rotation == pose.rotation))
    {
        return;
    }

    // GetMut, so the write stamps a change tick: propagation's dirty list and
    // network delta replication both filter on it.
    ECS::Transform &transform = *scene.GetMut<ECS::Transform>(entity);
    transform.position = pose.position;
    if (writeRotation)
    {
        transform.rotation = pose.rotation;
    }
    StampTransform(entity);
}

} // namespace Assisi::Physics
