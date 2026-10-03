/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file PhysicsWriteback.cpp
/// @brief Getting simulated poses back out: snapshots and render interpolation.
///
/// Physics steps at a fixed rate and rendering does not, so a pose written
/// straight from the last step beats against the step rate on a high-refresh
/// display. Each step is snapshotted instead, and a render frame blends the last
/// two — which is why CaptureState and InterpolateTransforms are a pair, and why
/// anything that teleports an object has to collapse both halves of its snapshot
/// or the jump is smeared across a frame.
///
/// Only bodies the simulation woke are followed, until each is written at rest,
/// so the cost is the number of things moving rather than the number of things.

#include "PhysicsInternal.hpp"

#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/TransformPose.hpp>

#include <cstdint>
#include <map>
#include <vector>

namespace Assisi::Physics
{

void PhysicsWorld::CaptureState()
{
    // The no-lock interface: no step is running, and every read below would
    // otherwise take and release a lock per body for nothing.
    const JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterfaceNoLock();

    // Whatever the step left awake joins the bodies being followed. Characters'
    // inner bodies are among them and are skipped: a character is followed
    // through its own record below.
    JPH::BodyIDVector active;
    _impl->physicsSystem.GetActiveBodies(JPH::EBodyType::RigidBody, active);
    for (const JPH::BodyID &id : active)
    {
        const ECS::Entity entity = EntityOfUserData(bodies.GetUserData(id));
        const Impl::BodySlot *slot = _impl->SlotFor(entity);
        if (slot != nullptr && slot->kind == Impl::SlotKind::Body)
        {
            _impl->Follow(entity.index);
        }
    }

    for (const std::uint32_t index : _impl->awake)
    {
        Impl::BodySlot &slot = _impl->slots[index];
        const JPH::RVec3 pos = bodies.GetPosition(slot.body);
        const JPH::Quat rot = bodies.GetRotation(slot.body);

        // Retire the previous current, then record this step's pose as current.
        slot.snapshot.prevPosition = slot.snapshot.curPosition;
        slot.snapshot.prevRotation = slot.snapshot.curRotation;
        slot.snapshot.curPosition = glm::vec3(pos.GetX(), pos.GetY(), pos.GetZ());
        slot.snapshot.curRotation = glm::quat(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ());

        // Asleep and no longer moving: one more write puts its Transform exactly
        // at rest, and after that there is nothing to follow.
        slot.settled = !bodies.IsActive(slot.body) && slot.snapshot.prevPosition == slot.snapshot.curPosition &&
                       slot.snapshot.prevRotation == slot.snapshot.curRotation;
    }

    // Characters are swept rather than solved and are never asleep, so every
    // one is snapshotted every step.
    for (std::pair<const std::uint32_t, Impl::CharacterRecord> &entry : _impl->characters)
    {
        Impl::BodySlot &slot = _impl->slots[entry.first];
        const JPH::RVec3 pos = entry.second.character->GetPosition();
        const JPH::Quat rot = entry.second.character->GetRotation();

        slot.snapshot.prevPosition = slot.snapshot.curPosition;
        slot.snapshot.prevRotation = slot.snapshot.curRotation;
        slot.snapshot.curPosition = glm::vec3(pos.GetX(), pos.GetY(), pos.GetZ());
        slot.snapshot.curRotation = glm::quat(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ());
    }
}

namespace
{

// Below these per-physics-step deltas a pose is treated as at rest, so it is
// snapped to the current step instead of blended (see BlendSnapshot).
constexpr float kRestPositionDeltaSq = 1e-8f; // (0.1 mm)^2 of translation between steps
constexpr float kRestRotationDelta = 1e-7f;   // 1 - |dot(prev, cur)|; ~0.0009 rad between steps

} // namespace

Pose PhysicsWorld::Impl::BlendSnapshot(const MotionSnapshot &snapshot, float alpha)
{
    // A body settling toward sleep produces consecutive step poses that differ by
    // a hair; blending them with a per-frame-varying alpha makes the render pose
    // wobble (~0.001 rad). Below the rest deltas, snap to the current step so it
    // renders stable. Snapping still tracks a slow creep exactly — it writes the
    // current pose every frame — it only drops the blend.
    const glm::vec3 positionDelta = snapshot.curPosition - snapshot.prevPosition;

    Pose pose;
    pose.position = glm::dot(positionDelta, positionDelta) < kRestPositionDeltaSq
                        ? snapshot.curPosition
                        : glm::mix(snapshot.prevPosition, snapshot.curPosition, alpha);

    // 1 - |dot(prev, cur)| is ~0 for near-identical orientations; abs folds the
    // quaternion q/-q double cover. slerp keeps angular speed constant across the
    // blend and is renormalised since the result feeds the render matrix.
    const float rotationDelta = 1.f - glm::abs(glm::dot(snapshot.prevRotation, snapshot.curRotation));
    pose.rotation = rotationDelta < kRestRotationDelta
                        ? snapshot.curRotation
                        : glm::normalize(glm::slerp(snapshot.prevRotation, snapshot.curRotation, alpha));
    return pose;
}

void PhysicsWorld::Impl::WriteRenderPose(ECS::Entity entity, Pose pose, bool writeRotation)
{
    const BodySlot *slot = SlotFor(entity);
    if (slot == nullptr)
    {
        return;
    }

    // Held back while a field the reconcile still has to push was written from
    // outside: this write re-stamps the Transform, which would mark that field
    // as handled. A write to anything else — a look system turning a
    // character, which the reconcile does not push, or a mutable access that
    // changed nothing — holds nothing back, or the entity would render frozen
    // until the next step.
    if (scene.ChangeTick<ECS::Transform>(entity) != slot->stamp.tick)
    {
        const ECS::Transform *current = scene.Get<ECS::Transform>(entity);
        if (current == nullptr || current->position != slot->stamp.position ||
            current->scale != slot->stamp.scale || (writeRotation && current->rotation != slot->stamp.rotation))
        {
            return;
        }
    }
    WritePose(entity, pose, writeRotation);
}

void PhysicsWorld::Impl::WritePose(ECS::Entity entity, Pose pose, bool writeRotation)
{
    const ECS::Transform *current = scene.Get<ECS::Transform>(entity);
    if (current == nullptr)
    {
        return;
    }

    // Jolt reports world space; a Transform under a parent is an offset *from*
    // that parent. Writing one into the other and letting PropagateTransforms
    // multiply by the parent again applies the parent twice — silently, and once
    // more every frame. Convert instead.
    if (const glm::mat4 *parent = ECS::ParentWorldMatrix(scene, entity); parent != nullptr)
    {
        pose.position = glm::vec3(glm::inverse(*parent) * glm::vec4(pose.position, 1.f));
        pose.rotation = glm::normalize(glm::inverse(ECS::WorldRotationOf(*parent)) * pose.rotation);
    }

    // Nothing moved: skip the write rather than stamp a change tick for a pose
    // identical to the one already there. A resting body would otherwise read as
    // changed every frame — dirty-subtree work for PropagateTransforms, and
    // bandwidth for a visual-only mirror, which travels by Transform delta.
    //
    // Exact comparison rather than epsilon'd: a resting body's snapshot poses are
    // frozen, so the computed target is bit-identical frame to frame, and the
    // rest-snap branches above already absorbed the near-rest jitter. Anything
    // genuinely in motion differs in the low bits and is written.
    if (current->position == pose.position && (!writeRotation || current->rotation == pose.rotation))
    {
        return;
    }

    // GetMut, so the write stamps a change tick: PropagateTransforms's dirty-skip
    // and network delta replication both filter on it.
    ECS::Transform &transform = *scene.GetMut<ECS::Transform>(entity);
    transform.position = pose.position;
    if (writeRotation)
    {
        transform.rotation = pose.rotation;
    }
    StampTransform(entity);
}

void PhysicsWorld::InterpolateTransforms(float alpha)
{
    // Ordered removal is not needed — the writes are independent of one another —
    // so a body that settles is swapped out of the list.
    for (std::size_t i = 0; i < _impl->awake.size();)
    {
        const std::uint32_t index = _impl->awake[i];
        Impl::BodySlot &slot = _impl->slots[index];
        _impl->WriteRenderPose(ECS::Entity{index, slot.generation}, Impl::BlendSnapshot(slot.snapshot, alpha),
                               /*writeRotation=*/ true);

        if (slot.settled)
        {
            slot.followed = false;
            slot.settled = false;
            _impl->awake[i] = _impl->awake.back();
            _impl->awake.pop_back();
            continue;
        }
        ++i;
    }

    for (const std::pair<const std::uint32_t, Impl::CharacterRecord> &entry : _impl->characters)
    {
        const Impl::BodySlot &slot = _impl->slots[entry.first];
        _impl->WriteRenderPose(entry.second.entity, Impl::BlendSnapshot(slot.snapshot, alpha),
                               /*writeRotation=*/ false);
    }
}

} // namespace Assisi::Physics
