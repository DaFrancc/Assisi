/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>

#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/RenderOffset.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/NetComponents.hpp>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "ReplicationInternal.hpp"

// ===========================================================================
// ReplicationClient: mirror bodies, smoothing, interpolation.
// ===========================================================================

namespace Assisi::NetSync
{
namespace
{

/// @p received, with each component that lies within one wire step of @p local
/// replaced by @p local's. The wire cannot tell those two apart, so taking the
/// received one is only rounding: a body at rest on the floor would be pulled a
/// millimetre into it every snapshot and stood back up between them, a tremble
/// with nothing behind it.
glm::vec3 WithinRoundingKept(glm::vec3 received, glm::vec3 local, float step)
{
    for (glm::length_t axis = 0; axis < 3; ++axis)
    {
        if (std::abs(received[axis] - local[axis]) <= step)
        {
            received[axis] = local[axis];
        }
    }
    return received;
}

} // namespace

void ReplicationClient::DestroyMirrorBody(NetId netId)
{
    _bodies.erase(netId);
    _history.erase(netId);
    _simulated.erase(netId);
    _touchedAt.erase(netId);
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

void ReplicationClient::ApplyBodyState(const BodyState &state)
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

    // This client's to simulate: what the host says of it is this client's
    // own report coming back, older than the body is now.
    if (_simulated.contains(state.netId))
    {
        return;
    }

    // How far the two simulations drifted apart since the last correction. Taken
    // before the snap and against the *physics* pose, not the rendered one:
    // folding in the cosmetic offset would flatter the number.
    //
    // A character is measured at its feet, which is what the host sends, and
    // not at its inner body: Jolt moves that copy of the capsule on its own
    // schedule, centimetres off the feet, and every correction would read the
    // gap as an error to smooth away, drawn as a bob.
    const bool character = _scene.Has<Physics::Character>(entity);
    const Physics::Pose simulated = _physics->GetBodyPose(entity);
    const glm::vec3 simulatedPosition = character ? _physics->CaptureCharacter(entity).position : simulated.position;

    // A character's capsule never turns: where it faces is its Transform, which
    // its controls turn. The one this machine controls keeps the facing its own
    // controls gave it, since the host's is the same turn arriving a round trip
    // late; any other faces where the host says.
    const ECS::Transform *transform = _scene.Get<ECS::Transform>(entity);
    const glm::quat simulatedRotation =
        character && transform != nullptr ? transform->rotation : simulated.rotation;
    const glm::quat targetRotation = character && ControlsEntity(entity) ? simulatedRotation : state.rotation;

    // Only what this client predicts is its own to move. A character it does
    // not, shoved here by the one it does, would be put back by the host's next
    // word, into the way of the character it was shoved by.
    const bool predicted = character ? PredictsCharacter(state.netId, entity) : PredictsBody(state.netId, entity);
    if (character)
    {
        _physics->SetCharacterPushable(entity, predicted);
    }

    ++_corrections.applied;

    // Compared with where the client had it at the same tick, not where it is
    // now, a round trip further on: ComparePredicted does that, after the
    // whole snapshot is in. The rest pose is kept, for when it stops being
    // predicted and is a mirror EnforceSleep holds again.
    if (predicted)
    {
        record.asleep       = state.asleep;
        record.restPosition = state.position;
        record.restRotation = state.rotation;
        return;
    }

    const float divergence = glm::length(simulatedPosition - state.position);
    _corrections.divergenceSum += static_cast<double>(divergence);
    _corrections.divergenceMax = std::max(_corrections.divergenceMax, divergence);

    // The rendered pose, which the correction must not change: the sim is
    // snapped and the offset absorbs the whole difference, so nothing happens on
    // screen this instant. Successive corrections accumulate into the same
    // offset, which keeps a stream of small ones smooth.
    const Physics::Pose rendered{record.rotationError * simulatedRotation, simulatedPosition + record.positionError};

    // Snapped hard, with no smoothing: extrapolation has to proceed from a valid
    // physics state, and a half-applied correction is not one. Hiding the jump
    // belongs to the view.
    const glm::vec3 correctedPosition =
        WithinRoundingKept(state.position, simulatedPosition, Quantization().PositionStep());
    _physics->ApplyCorrection(entity, Physics::Pose{state.rotation, correctedPosition},
                              Physics::BodyState{.linearVelocity = state.linearVelocity,
                                                 .angularVelocity = state.angularVelocity,
                                                 .asleep = state.asleep});
    RecordFollowerNow(entity);
    if (character && targetRotation != simulatedRotation)
    {
        _scene.GetMut<ECS::Transform>(entity)->rotation = targetRotation;
    }

    AbsorbCorrection(record, rendered, Physics::Pose{targetRotation, correctedPosition});

    record.asleep       = state.asleep;
    record.restPosition = correctedPosition;
    record.restRotation = state.rotation;
}

void ReplicationClient::AbsorbCorrection(MirrorBody &record, const Physics::Pose &rendered,
                                         const Physics::Pose &corrected)
{
    record.positionError = rendered.position - corrected.position;
    record.rotationError = glm::normalize(rendered.rotation * glm::inverse(corrected.rotation));

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
}

bool ReplicationClient::PredictsCharacter(NetId netId, ECS::Entity entity) const
{
    return _confirmed.contains(netId) && _scene.Has<Physics::Character>(entity);
}

bool ReplicationClient::PredictsBody(NetId netId, ECS::Entity entity) const
{
    return _physics != nullptr && _confirmed.contains(netId) && !_scene.Has<Physics::Character>(entity) &&
           _physics->HasBody(entity);
}

Physics::Pose ReplicationClient::SimulatedPose(ECS::Entity entity) const
{
    if (!_scene.Has<Physics::Character>(entity))
    {
        return _physics->GetBodyPose(entity);
    }
    const ECS::Transform *transform = _scene.Get<ECS::Transform>(entity);
    return Physics::Pose{transform != nullptr ? transform->rotation : glm::quat{1.f, 0.f, 0.f, 0.f},
                         _physics->CaptureCharacter(entity).position};
}

BodyState ReplicationClient::CaptureBody(NetId netId, ECS::Entity entity) const
{
    const Physics::Pose pose = _physics->GetBodyPose(entity);
    const Physics::BodyState motion = _physics->GetBodyState(entity);
    return BodyState{.netId = netId,
                     .position = pose.position,
                     .rotation = pose.rotation,
                     .linearVelocity = motion.linearVelocity,
                     .angularVelocity = motion.angularVelocity,
                     .asleep = motion.asleep};
}

void ReplicationClient::PlaceBody(ECS::Entity entity, const BodyState &host)
{
    _physics->ApplyCorrection(entity, Physics::Pose{host.rotation, host.position},
                              Physics::BodyState{.linearVelocity = host.linearVelocity,
                                                 .angularVelocity = host.angularVelocity,
                                                 .asleep = host.asleep});
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
        // A predicted body is this client's to wake: the host hears of the
        // push that woke it a round trip later.
        if (!record.asleep || _confirmed.contains(netId))
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

        // A predicted entity's pose is this client's own, ahead of every
        // sample the host could send.
        if (_confirmed.contains(netId))
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
    _history.clear();
    _sentCommands.fill(InputCommand{});
    _recordedTicks.fill(0);
    for (std::vector<FollowerRecord> &followers : _followers)
    {
        followers.clear();
    }
    _reports.fill(BodyReport{});
    _simulated.clear();
    _touchedAt.clear();
    _newestRecordedTick = 0;
    _confirmed.clear();
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
    _echo              = CommandEcho{};
    _handshake         = ServerHello{};
    _corrections       = CorrectionStats{};
    _lastAppliedTick   = 0;
    _snapshotsApplied  = 0;
    _snapshotsRejected = 0;
    _eventsReceived    = 0;
    _inbox             = Core::Reflect::MessageInbox{};
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
