/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file PhysicsWriteback.cpp
/// @brief Getting simulated poses back out: snapshots, render interpolation, and
///        the authoritative state replication reads.
///
/// Physics steps at a fixed rate and rendering does not, so a pose written
/// straight from the last step beats against the step rate on a high-refresh
/// display. Each step is snapshotted instead, and a render frame blends the last
/// two — which is why CaptureState and InterpolateTransforms are a pair, and why
/// anything that teleports an object has to collapse both halves of its snapshot
/// or the jump is smeared across a frame.

#include "PhysicsInternal.hpp"

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/TransformPose.hpp>

#include <Jolt/Physics/Body/BodyLock.h>

#include <vector>

namespace Assisi::Physics
{

void PhysicsWorld::CaptureState()
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();

    for (const JPH::BodyID &id : _impl->movingBodyIds)
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
        const JPH::Quat rot = bodies.GetRotation(id);

        // Retire the previous current, then record this step's pose as current.
        it->second.prevPosition = it->second.curPosition;
        it->second.prevRotation = it->second.curRotation;
        it->second.curPosition  = glm::vec3(pos.GetX(), pos.GetY(), pos.GetZ());
        it->second.curRotation  = glm::quat(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ());
    }

    // Characters are swept rather than solved, so they are not in the body set —
    // but they move every step they are not frozen, and want the same blend.
    for (auto &[id, record] : _impl->characters)
    {
        (void)id;
        const JPH::RVec3 pos = record.character->GetPosition();
        const JPH::Quat  rot = record.character->GetRotation();

        record.snapshot.prevPosition = record.snapshot.curPosition;
        record.snapshot.prevRotation = record.snapshot.curRotation;
        record.snapshot.curPosition  = glm::vec3(pos.GetX(), pos.GetY(), pos.GetZ());
        record.snapshot.curRotation  = glm::quat(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ());
    }
}

namespace
{

// Below these per-physics-step deltas a pose is treated as at rest, so it is
// snapped to the current step instead of blended (see BlendSnapshot).
constexpr float kRestPositionDeltaSq = 1e-8f; // (0.1 mm)^2 of translation between steps
constexpr float kRestRotationDelta   = 1e-7f; // 1 - |dot(prev, cur)|; ~0.0009 rad between steps

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

void PhysicsWorld::Impl::WriteRenderPose(ECS::Entity entity, ECS::Mut<ECS::Transform> transform,
                                         Pose pose, bool writeRotation,
                                         const ParentWorldFn &parentWorld)
{
    // Jolt reports world space; a Transform under a parent is an offset *from*
    // that parent. Writing one into the other and letting PropagateTransforms
    // multiply by the parent again applies the parent twice — silently, and once
    // more every frame. Convert instead.
    if (parentWorld)
    {
        if (const glm::mat4 *parent = parentWorld(entity); parent != nullptr)
        {
            pose.position = glm::vec3(glm::inverse(*parent) * glm::vec4(pose.position, 1.f));
            pose.rotation = glm::normalize(glm::inverse(ECS::WorldRotationOf(*parent)) * pose.rotation);
        }
    }

    // Nothing moved: skip the write rather than stamp a change tick for a pose
    // identical to the one already there. Every mutable access through the proxy
    // stamps, so a resting body would otherwise read as changed every frame for
    // the rest of the session — dirty-subtree work for PropagateTransforms, and
    // bandwidth for a visual-only mirror, which has no body channel and travels
    // by Transform delta.
    //
    // Exact comparison rather than epsilon'd: a resting body's snapshot poses are
    // frozen, so the computed target is bit-identical frame to frame, and the
    // rest-snap branches above already absorbed the near-rest jitter. Anything
    // genuinely in motion differs in the low bits and is written.
    const ECS::Transform &current = transform.Get();
    if (current.position == pose.position && (!writeRotation || current.rotation == pose.rotation))
    {
        return;
    }

    // Taken once, after the skip: binding the reference costs one tick per object
    // that actually moves rather than one per field written.
    ECS::Transform &t = transform.GetMut();
    t.position        = pose.position;
    if (writeRotation)
    {
        t.rotation = pose.rotation;
    }
}

void PhysicsWorld::InterpolateTransforms(Assisi::ECS::Scene &scene, float alpha, const ParentWorldFn &parentWorld)
{
    JPH::BodyInterface &bodies = _impl->physicsSystem.GetBodyInterface();

    // QueryMut, not Query: Transform is ACOMP(tracked) and this is the physics
    // writeback, so the new pose has to stamp a change tick. PropagateTransforms's
    // dirty-skip and network delta replication both filter on that tick, and a
    // write through a plain Query's `Transform&` stamps nothing — the body would
    // move with both consumers still reporting it unchanged. The proxy stamps
    // exactly like Scene::GetMut.
    //
    // RigidBody comes along as a Mut proxy because QueryMut wraps every type, but
    // it is only read — through the const Get(), which never stamps (and RigidBody
    // is ACOMP(transient) and untracked anyway, so there is no tick lane to touch).
    for (auto [entity, transform, rb] :
         scene.QueryMut<Assisi::ECS::Transform, RigidBody>())
    {
        // Only a body that moves has a pose worth writing back. A static one never
        // does, so its Transform is the authored truth and writing to it would
        // overwrite the author with a snapshot CaptureState never refreshes.
        //
        // A kinematic body *is* written back, because something can drive one
        // through the simulation (MoveBodyKinematic) — a moving platform whose
        // Transform never moved would be drawn where it was authored while the
        // thing standing on it rode away.
        const JPH::BodyID bodyId = ToJolt(rb.Get().bodyId);
        if (!bodies.IsAdded(bodyId) || bodies.GetMotionType(bodyId) == JPH::EMotionType::Static)
        {
            continue;
        }

        const auto it = _impl->snapshots.find(bodyId.GetIndexAndSequenceNumber());
        if (it == _impl->snapshots.end())
        {
            continue;
        }

        Impl::WriteRenderPose(entity, transform, Impl::BlendSnapshot(it->second, alpha),
                              /*writeRotation=*/ true, parentWorld);
    }

    // Characters, on the same two helpers. Their snapshots live on the character
    // record rather than in `snapshots`, because a character's inner body is
    // created and destroyed by Jolt and never enters this world's body set.
    for (auto [entity, transform, character] : scene.QueryMut<Assisi::ECS::Transform, Character>())
    {
        const Impl::CharacterRecord *record = _impl->FindCharacter(character.Get());
        if (record == nullptr)
        {
            continue;
        }

        Impl::WriteRenderPose(entity, transform, Impl::BlendSnapshot(record->snapshot, alpha),
                              /*writeRotation=*/ false, parentWorld);
    }
}

} // namespace Assisi::Physics
