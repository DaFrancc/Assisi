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
        WritePose(entity,
                  Pose{glm::quat(rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ()),
                       glm::vec3(position.GetX(), position.GetY(), position.GetZ())},
                  /*writeRotation=*/ true);

        const bool asleep = !bodies.IsActive(slot.body);
        const JPH::Vec3 linear = bodies.GetLinearVelocity(slot.body);
        const JPH::Vec3 angular = bodies.GetAngularVelocity(slot.body);
        WriteBodyState(entity, BodyState{.linearVelocity = glm::vec3(linear.GetX(), linear.GetY(), linear.GetZ()),
                                         .angularVelocity = glm::vec3(angular.GetX(), angular.GetY(), angular.GetZ()),
                                         .asleep = asleep});

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
    for (const std::pair<const std::uint32_t, CharacterRecord> &entry : characters)
    {
        const CharacterRecord &record = entry.second;
        const JPH::RVec3 position = record.character->GetPosition();
        const Pose pose{glm::quat(1.f, 0.f, 0.f, 0.f), glm::vec3(position.GetX(), position.GetY(), position.GetZ())};
        WritePose(record.entity, pose, /*writeRotation=*/ false);
        WriteCharacterState(record.entity, BuildCharacterState(record));
    }
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
