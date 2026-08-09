/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/Replication.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/DistanceRelevancy.hpp>
#include <Assisi/NetSync/NetComponents.hpp>

#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <expected>
#include <string>
#include <utility>

#include "ReplicationInternal.hpp"

// ===========================================================================
// ReplicationClient: mirror bodies, smoothing, interpolation.
// ===========================================================================

namespace Assisi::NetSync
{
void ReplicationClient::DestroyMirrorBody(NetId netId)
{
    const auto found = _bodies.find(netId);
    if (found == _bodies.end())
        return;

    // Without this the Jolt body outlives its entity and keeps colliding — an
    // invisible obstacle in the middle of the world, which is a worse bug than
    // the one that produced it.
    if (_physics != nullptr)
        _physics->RemoveBody(found->second.body);
    _bodies.erase(found);
}

void ReplicationClient::SyncMirrorBody(NetId netId, ECS::Entity entity)
{
    if (_physics == nullptr || !_scene.IsAlive(entity))
        return;

    const Physics::RigidBodyDescriptor *descriptor = _scene.Get<Physics::RigidBodyDescriptor>(entity);
    const ECS::Transform               *transform  = _scene.Get<ECS::Transform>(entity);
    if (descriptor == nullptr || transform == nullptr)
        return; // not a physical entity, or not fully described yet

    if (_scene.Get<Physics::RigidBody>(entity) == nullptr)
    {
        (void)_physics->AddBodyFromDescriptor(_scene, entity, *transform, *descriptor);
        const Physics::RigidBody *created = _scene.Get<Physics::RigidBody>(entity);
        if (created == nullptr)
            return;

        _bodies[netId].body = *created;
        // It is a simulated mirror now, not an interpolated one.
        _transformHistory.erase(netId);
        return;
    }

    // A dynamic body is owned by the correction stream from here on; touching it
    // from the component path would fight it every snapshot.
    if (!descriptor->isStatic)
        return;

    // Authored-static geometry moves by being *authored*, so its Transform is
    // the truth and the collider has to follow it. Without this the client's
    // visual moves and its collision does not — the worse half of a desync,
    // because it is invisible until something falls through it.
    const Physics::RigidBody *body = _scene.Get<Physics::RigidBody>(entity);
    _physics->SetBodyTransform(*body, transform->position, transform->rotation);
}

void ReplicationClient::ApplyBodyState(const BodyState &state)
{
    if (_physics == nullptr)
        return;

    const auto it = _entityByNetId.find(state.netId);
    if (it == _entityByNetId.end() || !_scene.IsAlive(it->second))
        return; // a body record for an entity we do not have: benign under loss

    const ECS::Entity entity = it->second;
    if (_scene.Get<Physics::RigidBody>(entity) == nullptr)
    {
        // First state for this mirror: build the body the server described.
        // Both halves have to have arrived — the descriptor says what to build,
        // the Transform says where — and if either has not, this record is
        // dropped and the next one (the delta path resends until acked) does it.
        const ECS::Transform               *transform  = _scene.Get<ECS::Transform>(entity);
        const Physics::RigidBodyDescriptor *descriptor = _scene.Get<Physics::RigidBodyDescriptor>(entity);
        if (transform == nullptr || descriptor == nullptr)
            return;

        (void)_physics->AddBodyFromDescriptor(_scene, entity, *transform, *descriptor);
        const Physics::RigidBody *created = _scene.Get<Physics::RigidBody>(entity);
        if (created == nullptr)
            return;
        _bodies[state.netId].body = *created;

        // It is a simulated mirror now, not an interpolated one.
        _transformHistory.erase(state.netId);
    }

    const Physics::RigidBody *body   = _scene.Get<Physics::RigidBody>(entity);
    MirrorBody               &record = _bodies[state.netId];

    // How far the two simulations drifted apart since the last correction. Taken
    // before the snap, against the *physics* pose rather than the rendered one:
    // this is the number the correction cadence has to be justified by, and
    // folding the previous frame's cosmetic offset into it would flatter it.
    const auto [simulatedPosition, simulatedRotation] = _physics->GetBodyTransform(*body);
    const float divergence                            = glm::length(simulatedPosition - state.position);

    ++_corrections.applied;
    _corrections.divergenceSum += static_cast<double>(divergence);
    _corrections.divergenceMax = std::max(_corrections.divergenceMax, divergence);

    // The rendered pose right now, which the correction must not change: the
    // sim is snapped, and the offset absorbs the whole difference so the screen
    // sees nothing happen at this instant. Successive corrections accumulate
    // into the same offset, which is what keeps a stream of small ones smooth
    // rather than each one fighting the last.
    const glm::vec3 renderedPosition = simulatedPosition + record.positionError;
    const glm::quat renderedRotation = record.rotationError * simulatedRotation;

    // The simulation is snapped hard, with no smoothing: extrapolation has to
    // proceed from a valid physics state, and a half-applied correction is not
    // one. Hiding the jump belongs to the view.
    _physics->ApplyBodyState(*body, state.position, state.rotation, state.linearVelocity, state.angularVelocity,
                             /*activate=*/!state.asleep);

    record.positionError = renderedPosition - state.position;
    record.rotationError = glm::normalize(renderedRotation * glm::inverse(state.rotation));

    const ViewSmoothing &smoothing = Smoothing();
    const float          carried   = glm::length(record.positionError);

    // Two ways an offset is not worth carrying. Below the floor the correction
    // is too small to see, so smoothing it buys nothing and only delays
    // convergence. Past the ceiling, smoothing reads worse than admitting the
    // jump: a body sliding half a room to catch up looks like a bug, where a
    // teleport looks like a teleport.
    if (carried < smoothing.snapBelowDistance || carried > smoothing.hardSnapDistance)
    {
        record.positionError   = glm::vec3{0.f};
        record.rotationError   = glm::quat{1.f, 0.f, 0.f, 0.f};
        record.smoothingWindow = 0.f;
    }
    else
    {
        // A bigger jump gets a shorter window: it is worth being over with
        // sooner. The time-domain form of the published two-rate split.
        record.smoothingWindow =
            carried <= smoothing.smallErrorDistance
                ? smoothing.positionCorrectionTime
                : (carried >= smoothing.largeErrorDistance
                       ? smoothing.positionCorrectionTimeFast
                       : glm::mix(smoothing.positionCorrectionTime, smoothing.positionCorrectionTimeFast,
                                  (carried - smoothing.smallErrorDistance) /
                                      (smoothing.largeErrorDistance - smoothing.smallErrorDistance)));

        // Restarted by every correction, from wherever the picture currently is,
        // which is what keeps a correction arriving mid-convergence continuous.
        record.positionErrorStart = record.positionError;
        record.rotationErrorStart = record.rotationError;
        record.smoothingElapsed   = 0.f;
    }

    record.asleep       = state.asleep;
    record.restPosition = state.position;
    record.restRotation = state.rotation;
}

void ReplicationClient::SmoothView(double serverTimeTicks, float dt)
{
    // Non-bodied mirrors: interpolate between received samples, unchanged.
    Interpolate(serverTimeTicks);

    if (_physics == nullptr || dt <= 0.f)
        return;

    const ViewSmoothing &smoothing = Smoothing();

    for (auto &[netId, record] : _bodies)
    {
        const auto entity = _entityByNetId.find(netId);
        if (entity == _entityByNetId.end() || !_scene.IsAlive(entity->second))
            continue;

        ECS::Transform *transform = _scene.GetMut<ECS::Transform>(entity->second);
        if (transform == nullptr)
            continue;

        if (record.smoothingWindow <= 0.f)
            continue; // nothing to hide

        // Linear over the window, so the offset is gone by the deadline and
        // moves at a constant on-screen speed until it is. Advancing in *time*
        // rather than per frame is what keeps the feel identical at 30 and at
        // 144 Hz.
        record.smoothingElapsed += dt;
        const float remaining =
            1.f - std::min(1.f, record.smoothingElapsed / record.smoothingWindow);

        record.positionError = record.positionErrorStart * remaining;
        record.rotationError = glm::normalize(
            glm::slerp(glm::quat{1.f, 0.f, 0.f, 0.f}, record.rotationErrorStart,
                       // Orientation gets its own, shorter, window.
                       1.f - std::min(1.f, record.smoothingElapsed / smoothing.rotationCorrectionTime)));

        // On top of the physics writeback's pose, which ran just before this.
        transform->position += record.positionError;
        transform->rotation = record.rotationError * transform->rotation;
    }
}

void ReplicationClient::RequestKeyframe()
{
    if (!_synchronized)
        return;

    Core::BitWriter writer;
    WriteMessageType(MessageType::RequestKeyframe, writer);
    // Reliable: the point of asking is that something is already wrong, and an
    // unreliable request that gets dropped looks exactly like a button that does
    // nothing.
    _transport.Send(_connection, writer.Data(), Net::SendMode::Reliable, Net::Lane::Control);
}

void ReplicationClient::EnforceSleep()
{
    if (_physics == nullptr)
        return;

    for (const auto &[netId, record] : _bodies)
    {
        if (!record.asleep)
            continue;

        const auto it = _entityByNetId.find(netId);
        if (it == _entityByNetId.end() || !_scene.IsAlive(it->second))
            continue;

        const Physics::RigidBody *body = _scene.Get<Physics::RigidBody>(it->second);
        if (body == nullptr || !_physics->IsBodyActive(*body))
            continue;

        // Woken by something the server never saw. Put it back and hold it there
        // — the server's own correction is the only thing allowed to wake it.
        _physics->ApplyBodyState(*body, record.restPosition, record.restRotation, glm::vec3{0.f}, glm::vec3{0.f},
                                 /*activate=*/false);
    }
}

void ReplicationClient::ResolvePendingRefs()
{
    if (_pendingRefs.empty())
        return;

    const Core::Reflect::ComponentRegistry &registry = Core::Reflect::ComponentRegistry::Instance();

    std::erase_if(_pendingRefs,
                  [&](const PendingRef &pending)
                  {
                      const auto target = _entityByNetId.find(pending.target);
                      if (target == _entityByNetId.end())
                          return false; // still waiting; keep it

                      const Core::Reflect::ComponentMeta *meta = registry.ById(pending.component);
                      if (meta == nullptr || !_scene.IsAlive(pending.entity))
                          return true; // the holder went away — the reference is moot

                      void *component = const_cast<void *>(
                          meta->getByEntity(&_scene, pending.entity.index, pending.entity.generation));
                      if (component == nullptr)
                          return true;

                      // Write the resolved handle directly at the field's offset:
                      // this is the same by-offset access the codec itself uses,
                      // and the type is fixed by FieldType::EntityRef.
                      auto *slot = reinterpret_cast<ECS::Entity *>(static_cast<std::byte *>(component) +
                                                                   pending.fieldOffset);
                      *slot      = target->second;
                      _scene.MarkChanged(pending.entity, pending.component);
                      return true;
                  });
}

void ReplicationClient::CaptureTransforms(std::uint64_t serverTick)
{
    for (const auto &[netId, entity] : _entityByNetId)
    {
        // A bodied mirror leaves the interpolation path entirely: it has a real
        // dynamic body stepped by the local physics and corrected by the wire,
        // and buffering poses for it would mean rendering it two snapshot
        // intervals in the past *as well* — the delay local simulation exists to
        // remove.
        if (_bodies.contains(netId))
            continue;

        const ECS::Transform *transform = _scene.Get<ECS::Transform>(entity);
        if (transform == nullptr)
            continue;

        std::deque<TransformSample> &history = _transformHistory[netId];
        // A repeat of the tick we already hold means a snapshot was applied
        // twice; overwrite rather than append, so the buffer never holds two
        // samples the interpolator would divide by zero between.
        if (!history.empty() && history.back().serverTick == serverTick)
            history.pop_back();

        history.push_back(TransformSample{serverTick, transform->position, transform->rotation, transform->scale});
        while (history.size() > kMaxSamples)
            history.pop_front();
    }

    // Entities that went away take their history with them.
    std::erase_if(_transformHistory,
                  [this](const auto &entry) { return !_entityByNetId.contains(entry.first); });
}

void ReplicationClient::Interpolate(double serverTimeTicks)
{
    for (const auto &[netId, history] : _transformHistory)
    {
        if (history.empty())
            continue;

        const auto entity = _entityByNetId.find(netId);
        if (entity == _entityByNetId.end() || !_scene.IsAlive(entity->second))
            continue;

        ECS::Transform *transform = _scene.GetMut<ECS::Transform>(entity->second);
        if (transform == nullptr)
            continue;

        // Past the newest sample: hold the last known pose rather than
        // extrapolate. A guess that turns out wrong costs a visible snap when
        // the real value arrives, and standing still reads better than that.
        if (serverTimeTicks >= static_cast<double>(history.back().serverTick) || history.size() == 1)
        {
            transform->position = history.back().position;
            transform->rotation = history.back().rotation;
            transform->scale    = history.back().scale;
            continue;
        }

        // Before the oldest: the buffer does not reach back that far (a client
        // that just joined, or a delay someone widened at runtime). Same
        // answer — show what we have.
        if (serverTimeTicks <= static_cast<double>(history.front().serverTick))
        {
            transform->position = history.front().position;
            transform->rotation = history.front().rotation;
            transform->scale    = history.front().scale;
            continue;
        }

        // Find the straddling pair. The buffer is three deep, so a scan is
        // both simpler and faster than anything cleverer.
        const TransformSample *before = &history.front();
        const TransformSample *after  = &history.back();
        for (std::size_t i = 1; i < history.size(); ++i)
        {
            if (static_cast<double>(history[i].serverTick) >= serverTimeTicks)
            {
                before = &history[i - 1];
                after  = &history[i];
                break;
            }
        }

        const double span = static_cast<double>(after->serverTick) - static_cast<double>(before->serverTick);
        const float  t    = span > 0.0
                                ? static_cast<float>((serverTimeTicks - static_cast<double>(before->serverTick)) / span)
                                : 1.f;

        transform->position = glm::mix(before->position, after->position, t);
        transform->scale    = glm::mix(before->scale, after->scale, t);
        // slerp, not mix: a linear blend of quaternions is not a rotation, and
        // the error is worst exactly where rotation is fastest.
        transform->rotation = glm::slerp(before->rotation, after->rotation, t);
    }
}

ECS::Entity ReplicationClient::EntityOf(NetId netId) const
{
    const auto it = _entityByNetId.find(netId);
    return it == _entityByNetId.end() ? ECS::NullEntity : it->second;
}

bool ReplicationClient::ControlsEntity(ECS::Entity entity) const
{
    if (!_handshake.clientId.IsValid())
        return false;
    const ControlledBy *claim = _scene.Get<ControlledBy>(entity);
    return claim != nullptr && claim->client == _handshake.clientId.value;
}

NetId ReplicationClient::NetIdOf(ECS::Entity entity) const
{
    for (const auto &[netId, mirror] : _entityByNetId)
    {
        if (mirror == entity)
            return netId;
    }
    return InvalidNetId;
}

void ReplicationClient::Reset()
{
    for (const auto &[netId, entity] : _entityByNetId)
    {
        if (_scene.IsAlive(entity))
            _scene.Destroy(entity);
    }
    if (!_entityByNetId.empty())
        ++_structureRevision;
    _entityByNetId.clear();

    if (_physics != nullptr)
    {
        for (const auto &[netId, record] : _bodies)
            _physics->RemoveBody(record.body);
    }
    _bodies.clear();
    _transformHistory.clear();
    _pendingRefs.clear();
    _feedback          = ClockFeedback{};
    _handshake         = ServerHello{};
    _corrections       = CorrectionStats{};
    _lastAppliedTick   = 0;
    _snapshotsApplied  = 0;
    _snapshotsRejected = 0;
    _synchronized      = false;
    _worldComplete     = false;
    _awaitingLevel     = false;
    _rejectMessage.clear();
    // _structureRevision deliberately survives: it is a monotonic "something
    // changed" counter a consumer compares against its own last-acted-on value,
    // and resetting it to 0 would make a rejoin look like no change at all.
}

} // namespace Assisi::NetSync
