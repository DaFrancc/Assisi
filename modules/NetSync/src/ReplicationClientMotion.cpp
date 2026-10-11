/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/ReplicationClient.hpp>

#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/RenderOffset.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/NetComponents.hpp>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <vector>

#include "ReplicationInternal.hpp"

// ===========================================================================
// ReplicationClient: mirror bodies, smoothing, interpolation.
// ===========================================================================

namespace Assisi::NetSync
{
void ReplicationClient::DestroyMirrorBody(NetId netId)
{
    _bodies.erase(netId);
    _predictions.erase(netId);
}

void ReplicationClient::SyncMirrorBody(NetId netId, ECS::Entity entity)
{
    if (_physics == nullptr || !_scene.IsAlive(entity) || _bodies.contains(netId))
        return;

    const bool physical = _scene.Has<Physics::Collider>(entity) || _scene.Has<Physics::Character>(entity);
    if (!physical || !_scene.Has<ECS::Transform>(entity))
        return; // not a physical entity, or not fully described yet

    // It is a simulated mirror now, not an interpolated one. The physics world
    // builds its body; nothing sends body state for a static mirror, so that is
    // the only way one exists for the client's own bodies to rest on.
    _bodies.try_emplace(netId);
    _transformHistory.erase(netId);
}

void ReplicationClient::ApplyBodyState(const BodyState &state, std::uint64_t serverTick)
{
    if (_physics == nullptr)
        return;

    const auto it = _entityByNetId.find(state.netId);
    if (it == _entityByNetId.end() || !_scene.IsAlive(it->second))
        return; // a body record for an entity we do not have: benign under loss

    const ECS::Entity entity = it->second;
    if (!_physics->HasBody(entity))
    {
        // First state for this mirror: the body has to exist before it can be
        // corrected, and it is built from the components and Transform the server
        // sent. If either has not arrived, nothing is built and this record is
        // dropped; the delta path resends until acked.
        _physics->Reconcile();
        if (!_physics->HasBody(entity))
            return;
    }

    // It is a simulated mirror now, not an interpolated one.
    MirrorBody &record = _bodies[state.netId];
    _transformHistory.erase(state.netId);

    // How far the two simulations drifted apart since the last correction. Taken
    // before the snap and against the *physics* pose, not the rendered one:
    // folding in the cosmetic offset would flatter the number.
    const Physics::Pose simulated = _physics->GetBodyPose(entity);
    const glm::vec3 simulatedPosition = simulated.position;

    // A character's capsule never turns: where it faces is its Transform, which
    // its controls turn. The one this machine controls keeps the facing its own
    // controls gave it, since the host's is the same turn arriving a round trip
    // late; any other faces where the host says.
    const bool character = _scene.Has<Physics::Character>(entity);
    const ECS::Transform *transform = _scene.Get<ECS::Transform>(entity);
    const glm::quat simulatedRotation =
        character && transform != nullptr ? transform->rotation : simulated.rotation;
    const glm::quat targetRotation = character && ControlsEntity(entity) ? simulatedRotation : state.rotation;

    // Only what this client predicts is its own to move. A character it does
    // not, shoved here by the one it does, would be put back by the host's next
    // word, into the way of the character it was shoved by.
    if (character)
    {
        _physics->SetCharacterPushable(entity, PredictsCharacter(entity));
    }

    // A character this client predicts is compared with where it predicted it
    // at the same tick, not with where it is now: it is a round trip further on.
    PredictionHistory *history = nullptr;
    if (const std::unordered_map<NetId, PredictionHistory>::iterator found = _predictions.find(state.netId);
        found != _predictions.end() && !found->second.empty() && PredictsCharacter(entity))
    {
        history = &found->second;
    }
    const PredictionHistory::const_iterator predicted =
        history == nullptr ? PredictionHistory::const_iterator{}
                           : std::find_if(history->cbegin(), history->cend(),
                                          [serverTick](const PredictedFrame &frame) { return frame.tick == serverTick; });
    const bool comparable = history != nullptr && predicted != history->cend();
    const float divergence = glm::length((comparable ? predicted->frame.position : simulatedPosition) - state.position);

    ++_corrections.applied;
    _corrections.divergenceSum += static_cast<double>(divergence);
    _corrections.divergenceMax = std::max(_corrections.divergenceMax, divergence);

    // Near enough, and the client's own result stands: moving a character the
    // player is steering by a hair's breadth of rounding is a jitter they feel.
    if (comparable && divergence <= Smoothing().predictionTolerance)
    {
        ++_corrections.predictionsKept;
        std::erase_if(*history, [serverTick](const PredictedFrame &frame) { return frame.tick <= serverTick; });
        return;
    }

    // The rendered pose, which the correction must not change: the sim is
    // snapped and the offset absorbs the whole difference, so nothing happens on
    // screen this instant. Successive corrections accumulate into the same
    // offset, which keeps a stream of small ones smooth.
    const glm::vec3 renderedPosition = simulatedPosition + record.positionError;
    const glm::quat renderedRotation = record.rotationError * simulatedRotation;

    // Snapped hard, with no smoothing: extrapolation has to proceed from a valid
    // physics state, and a half-applied correction is not one. Hiding the jump
    // belongs to the view. A predicted character is snapped to the host's state
    // and then brought forward to now through the commands sent since.
    glm::vec3 correctedPosition = state.position;
    if (history != nullptr)
    {
        ReplayOwnCharacter(entity, *history, state, serverTick);
        ++_corrections.predictionsReplayed;
        correctedPosition = _physics->CaptureCharacter(entity).position;
    }
    else
    {
        _physics->ApplyCorrection(entity, Physics::Pose{state.rotation, state.position},
                                  Physics::BodyState{.linearVelocity = state.linearVelocity,
                                                     .angularVelocity = state.angularVelocity,
                                                     .asleep = state.asleep});
    }
    if (character && targetRotation != simulatedRotation)
    {
        _scene.GetMut<ECS::Transform>(entity)->rotation = targetRotation;
    }

    record.positionError = renderedPosition - correctedPosition;
    record.rotationError = glm::normalize(renderedRotation * glm::inverse(targetRotation));

    const ViewSmoothing &smoothing = Smoothing();
    const float carried   = glm::length(record.positionError);

    // Two ways an offset is not worth carrying: below the floor it is too small
    // to see, and past the ceiling a body sliding half a room to catch up reads
    // worse than admitting the teleport.
    if (carried < smoothing.snapBelowDistance || carried > smoothing.hardSnapDistance)
    {
        record.positionError   = glm::vec3{0.f};
        record.rotationError   = glm::quat{1.f, 0.f, 0.f, 0.f};
        record.smoothingWindow = 0.f;
    }
    else
    {
        // A bigger jump gets a shorter window: it is worth being over with
        // sooner. Lerped between the two rates in the band between them.
        record.smoothingWindow =
            carried <= smoothing.smallErrorDistance
                ? smoothing.positionCorrectionTime
                : (carried >= smoothing.largeErrorDistance
                       ? smoothing.positionCorrectionTimeFast
                       : glm::mix(smoothing.positionCorrectionTime, smoothing.positionCorrectionTimeFast,
                                  (carried - smoothing.smallErrorDistance) /
                                  (smoothing.largeErrorDistance - smoothing.smallErrorDistance)));

        // Restarted by every correction, from wherever the picture currently is,
        // so one arriving mid-convergence stays continuous.
        record.positionErrorStart = record.positionError;
        record.rotationErrorStart = record.rotationError;
        record.smoothingElapsed   = 0.f;
    }

    record.asleep       = state.asleep;
    record.restPosition = state.position;
    record.restRotation = state.rotation;
}

void ReplicationClient::ReplayOwnCharacter(ECS::Entity entity, PredictionHistory &history, const BodyState &state,
                                           std::uint64_t serverTick)
{
    // The wire carries where the host had it and how fast it went, not its jump
    // timers, stance or eye height; those come from the client's own frame for
    // that tick, or the nearest after it when that one is gone.
    const PredictionHistory::const_iterator from = std::find_if(
        history.cbegin(), history.cend(), [serverTick](const PredictedFrame &frame) { return frame.tick >= serverTick; });
    Physics::CharacterFrame start = from != history.cend() ? from->frame : history.back().frame;
    start.position = state.position;
    start.velocity = state.linearVelocity;
    _physics->RewindCharacter(entity, start);

    // Each frame is rewritten with where the replay put it, so the host's next
    // answer is compared with what the client now believes rather than with
    // what it believed before it was corrected.
    const float step = 1.f / static_cast<float>(std::max(_tickRateHz, 1u));
    for (PredictedFrame &later : history)
    {
        if (later.tick <= serverTick)
        {
            continue;
        }
        _physics->ReplayCharacter(entity, later.frame.intent, later.frame.facing, step);
        later.frame = _physics->CaptureCharacter(entity);
    }
    std::erase_if(history, [serverTick](const PredictedFrame &frame) { return frame.tick <= serverTick; });
}

bool ReplicationClient::PredictsCharacter(ECS::Entity entity) const
{
    if (!_scene.Has<Physics::Character>(entity) || !ControlsEntity(entity))
    {
        return false;
    }
    const Physics::CharacterState *state = _scene.Get<Physics::CharacterState>(entity);
    return state == nullptr || state->baseEntity == ECS::NullEntity;
}

void ReplicationClient::RecordPrediction(std::uint64_t commandTick)
{
    if (_physics == nullptr)
    {
        return;
    }
    for (const auto &[netId, body] : _bodies)
    {
        (void)body;
        const ECS::Entity entity = EntityOf(netId);
        if (!_scene.IsAlive(entity) || !PredictsCharacter(entity) || !_physics->HasBody(entity))
        {
            // Handed to someone else, riding, or not built yet: what it
            // predicted before no longer describes it.
            _predictions.erase(netId);
            continue;
        }
        PredictionHistory &history = _predictions[netId];

        // Command ticks only go forward. One that did not would pair the host's
        // answers with the wrong steps.
        if (!history.empty() && history.back().tick >= commandTick)
        {
            history.clear();
        }
        history.push_back(PredictedFrame{.frame = _physics->CaptureCharacter(entity), .tick = commandTick});
        while (history.size() > kPredictionHistoryTicks)
        {
            history.pop_front();
        }
    }
}

void ReplicationClient::SmoothView(double serverTimeTicks, float dt)
{
    // Non-bodied mirrors: interpolate between received samples.
    Interpolate(serverTimeTicks);

    if (_physics == nullptr || dt <= 0.f)
        return;

    const ViewSmoothing &smoothing = Smoothing();

    for (auto &[netId, record] : _bodies)
    {
        const auto entity = _entityByNetId.find(netId);
        if (entity == _entityByNetId.end() || !_scene.IsAlive(entity->second))
            continue;

        if (record.smoothingWindow <= 0.f)
        {
            // Nothing to hide. An offset a correction snapped away is cleared, or
            // the mirror would go on being drawn where the last one left it.
            if (ECS::RenderOffset *offset = _scene.Get<ECS::RenderOffset>(entity->second); offset != nullptr)
            {
                *offset = ECS::RenderOffset{};
            }
            continue;
        }

        // Linear over the window, so the offset is gone by the deadline at a
        // constant on-screen speed. Advancing in *time* rather than per frame is
        // what keeps the feel identical at 30 and at 144 Hz.
        record.smoothingElapsed += dt;
        const float remaining =
            1.f - std::min(1.f, record.smoothingElapsed / record.smoothingWindow);

        record.positionError = record.positionErrorStart * remaining;
        record.rotationError = glm::normalize(
            glm::slerp(glm::quat{1.f, 0.f, 0.f, 0.f}, record.rotationErrorStart,
                       // Orientation gets its own, shorter, window.
                       1.f - std::min(1.f, record.smoothingElapsed / smoothing.rotationCorrectionTime)));

        // Only how it is drawn: the Transform stays the corrected simulation
        // pose, and the offset is added in world space after the render blend,
        // which is right for a parented mirror too.
        ECS::RenderOffset *offset = _scene.Get<ECS::RenderOffset>(entity->second);
        if (offset == nullptr)
        {
            offset = _scene.Add<ECS::RenderOffset>(entity->second);
        }
        if (offset != nullptr)
        {
            *offset = ECS::RenderOffset{.rotation = record.rotationError, .position = record.positionError};
        }
    }
}

void ReplicationClient::RequestKeyframe()
{
    if (!_synchronized)
        return;

    Core::BitWriter writer;
    WriteMessageType(MessageType::RequestKeyframe, writer);
    // Reliable: asking already means something is wrong, and a dropped request
    // looks exactly like a button that does nothing.
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

        if (!_physics->IsBodyActive(it->second))
            continue;

        // Woken by something the server never saw. Put it back and hold it there
        // — the server's own correction is the only thing allowed to wake it.
        _physics->ApplyCorrection(it->second, Physics::Pose{record.restRotation, record.restPosition},
                                  Physics::BodyState{.asleep = true});
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
                return false;           // still waiting; keep it

            const Core::Reflect::ComponentMeta *meta = registry.ById(pending.component);
            if (meta == nullptr || !_scene.IsAlive(pending.entity))
                return true;           // the holder went away — the reference is moot

            void *component = const_cast<void *>(
                meta->getByEntity(&_scene, pending.entity.index, pending.entity.generation));
            if (component == nullptr)
                return true;

            // By-offset write, the same access the codec uses; the type
            // is fixed by FieldType::EntityRef.
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
        // A bodied mirror leaves the interpolation path entirely: it is stepped
        // by the local physics and corrected by the wire, so buffering poses for
        // it would also render it two snapshot intervals in the past — the delay
        // local simulation exists to remove.
        if (_bodies.contains(netId))
            continue;

        // Only a Transform the server sends is the server's to move.
        if (!_sentTransforms.contains(netId))
            continue;

        const ECS::Transform *transform = _scene.Get<ECS::Transform>(entity);
        if (transform == nullptr)
            continue;

        std::deque<TransformSample> &history = _transformHistory[netId];
        // A repeat of the tick we already hold means a snapshot was applied
        // twice. Overwrite rather than append: two samples on the same tick are
        // a zero span the interpolator would divide by.
        if (!history.empty() && history.back().serverTick == serverTick)
            history.pop_back();

        history.push_back(TransformSample{serverTick, transform->position, transform->rotation, transform->scale});
        while (history.size() > kMaxSamples)
            history.pop_front();
    }

    // Entities that went away take their history with them.
    std::erase_if(_transformHistory,
                  [this](const auto &entry) { return !_entityByNetId.contains(entry.first); });
    std::erase_if(_sentTransforms, [this](NetId netId) { return !_entityByNetId.contains(netId); });
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
        // extrapolate. A wrong guess costs a visible snap when the real value
        // arrives; standing still reads better.
        if (serverTimeTicks >= static_cast<double>(history.back().serverTick) || history.size() == 1)
        {
            transform->position = history.back().position;
            transform->rotation = history.back().rotation;
            transform->scale    = history.back().scale;
            continue;
        }

        // Before the oldest: the buffer does not reach back that far — a client
        // that just joined, or a delay widened at runtime. Same answer.
        if (serverTimeTicks <= static_cast<double>(history.front().serverTick))
        {
            transform->position = history.front().position;
            transform->rotation = history.front().rotation;
            transform->scale    = history.front().scale;
            continue;
        }

        // Find the straddling pair. The buffer is three deep, so a scan beats
        // anything cleverer.
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
        const float t    = span > 0.0
                                ? static_cast<float>((serverTimeTicks - static_cast<double>(before->serverTick)) / span)
                                : 1.f;

        transform->position = glm::mix(before->position, after->position, t);
        transform->scale    = glm::mix(before->scale, after->scale, t);
        // slerp, not mix: a linear blend of quaternions is not a rotation, and
        // the error is worst where rotation is fastest.
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

    // The mirrors' bodies go with the entities destroyed above.
    _bodies.clear();
    _predictions.clear();
    _transformHistory.clear();
    _sentTransforms.clear();
    _pendingRefs.clear();

    // Every instance retires at once — the wholesale form of what the despawn
    // path does one record at a time. The members are already destroyed above,
    // which is the order Collapse requires; what it drops is whatever the
    // expansion put somewhere only the expander can reach.
    //
    // Ids first, maps cleared, expander called last: Collapse is App code
    // reaching into a world this class knows nothing about, so it runs with no
    // container of ours being iterated.
    std::vector<ECS::InstanceId> collapsed;
    collapsed.reserve(_instanceIdByBase.size());
    for (const auto &[base, localInstance] : _instanceIdByBase)
    {
        (void)base;
        collapsed.push_back(localInstance);
    }
    _instanceRecords.clear();
    _instanceIdByBase.clear();
    _baseByInstanceId.clear();
    _deferredAnnouncements.clear();
    if (_instanceExpander != nullptr)
    {
        for (const ECS::InstanceId localInstance : collapsed)
            _instanceExpander->Collapse(localInstance);
    }

    _feedback          = ClockFeedback{};
    _handshake         = ServerHello{};
    _corrections       = CorrectionStats{};
    _lastAppliedTick   = 0;
    _snapshotsApplied  = 0;
    _snapshotsRejected = 0;
    _eventsDispatched  = 0;
    _eventsUnhandled   = 0;
    _synchronized      = false;
    _worldComplete     = false;
    _awaitingLevel     = false;
    // The join gate, and load-bearing: SetContentSetHash completes a join the
    // moment both halves are in, so this must go back to "the application has
    // not said its world is built" or a hash arriving first would send the next
    // hello on its own.
    _levelReady = false;
    _rejectMessage.clear();
    // _structureRevision deliberately survives: consumers compare it against
    // their own last-acted-on value, so resetting it to 0 would make a rejoin
    // look like no change at all.
    //
    // So does everything the *application* installed rather than the session: the
    // expander, `_deferHandshake`, and the content-set hash. Those describe this
    // build and this machine, not the connection that just ended, and the next
    // join needs them already in place — its hello goes out exactly once, with
    // nothing to retry if a half is missing.
}

} // namespace Assisi::NetSync
