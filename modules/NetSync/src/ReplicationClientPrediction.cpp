/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/ReplicationClient.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/Core/Reflect/PredictedCompare.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <vector>

// ===========================================================================
// ReplicationClient: recording what was predicted, comparing it with the
// host's word, and rolling back.
// ===========================================================================

namespace Assisi::NetSync
{
namespace
{

/// The angle between two rotations (rad), whichever of q and -q each is.
float AngleBetween(const glm::quat &a, const glm::quat &b)
{
    const float cosine = std::min(1.f, std::abs(glm::dot(a, b)));
    return 2.f * std::acos(cosine);
}

/// "@p name: mine vs host's", for a quantity the tolerances say differs.
template <typename T> std::string Difference(std::string_view name, const T &mine, const T &host)
{
    return std::format("{}: {} vs {}", name, Core::Reflect::PredictedFieldText(mine),
                       Core::Reflect::PredictedFieldText(host));
}

} // namespace

void ReplicationClient::RecordStep(const InputCommand &command)
{
    // Command ticks only go forward. One that did not would pair the host's
    // answers with the wrong steps.
    if (command.tick <= _newestRecordedTick)
    {
        _history.clear();
        _recordedTicks.fill(0);
        _newestRecordedTick = 0;
    }
    _sentCommands[command.tick % kPredictionHistoryTicks] = command;
    Record(command.tick);
}

void ReplicationClient::RecordHistory(std::uint64_t tick)
{
    ASSISI_ASSERT(_rollingBack, "ReplicationClient::RecordHistory outside a rollback");
    Record(tick);
}

void ReplicationClient::Record(std::uint64_t tick)
{
    _recordedTicks[tick % kPredictionHistoryTicks] = tick;
    _newestRecordedTick = std::max(_newestRecordedTick, tick);
    for (const auto &[netId, confirmed] : _confirmed)
    {
        (void)confirmed;
        const ECS::Entity entity = EntityOf(netId);
        if (!_scene.IsAlive(entity))
        {
            continue;
        }
        PredictedRecord &record = _history[netId].records[tick % kPredictionHistoryTicks];
        record.tick = tick;
        record.present = 0;
        record.components.resize(_predictedBytes);
        for (std::size_t i = 0; i < _predictedComponents.size(); ++i)
        {
            const Core::Reflect::ComponentMeta &meta = *_predictedComponents[i];
            const void *live = meta.getByEntity(&_scene, entity.index, entity.generation);
            if (live == nullptr)
            {
                continue;
            }
            std::memcpy(record.components.data() + _predictedOffsets[i], live, meta.size);
            record.present |= std::uint64_t{1} << i;
        }
        record.hasCharacter = _physics != nullptr && PredictsCharacter(netId, entity) && _physics->HasBody(entity);
        if (record.hasCharacter)
        {
            record.character = _physics->CaptureCharacter(entity);
        }
    }
}

void ReplicationClient::ComparePredicted(std::uint64_t serverTick)
{
    // Every tick this client kept is newer: the host's word is about a moment
    // there is no record of, and so nothing to step again from.
    const bool olderThanKept = _newestRecordedTick >= serverTick + kPredictionHistoryTicks;

    std::vector<PredictionCounts *> differing;
    for (const auto &[netId, confirmed] : _confirmed)
    {
        if (confirmed.componentTick != serverTick && confirmed.physicsTick != serverTick)
        {
            continue;
        }
        const ECS::Entity entity = EntityOf(netId);
        const std::unordered_map<NetId, PredictionHistory>::iterator found = _history.find(netId);
        if (!_scene.IsAlive(entity) || found == _history.end())
        {
            continue;
        }
        PredictionHistory &history = found->second;
        if (olderThanKept)
        {
            HardSnap(netId, entity, confirmed);
            // What it recorded since stood on where it was, not where it is.
            for (PredictedRecord &record : history.records)
            {
                record.tick = 0;
            }
            history.mispredictedAt = 0;
            continue;
        }
        if (history.records[serverTick % kPredictionHistoryTicks].tick != serverTick)
        {
            continue;
        }

        differing.clear();
        CompareRecord(entity, confirmed, serverTick, history, differing);
        if (differing.empty())
        {
            ++_corrections.predictionsKept;
            continue;
        }

        // A rollback already waiting for it: this word is newer, so it is the
        // one to roll back to, and it is the same rollback.
        if (history.mispredictedAt != 0)
        {
            history.mispredictedAt = serverTick;
            continue;
        }
        if (_newestRecordedTick < history.cooldownUntil)
        {
            continue;
        }
        for (PredictionCounts *section : differing)
        {
            ++section->rollbacks;
        }
        history.mispredictedAt = serverTick;
        history.cooldownUntil = _newestRecordedTick + SnapshotIntervalTicks();
    }
}

void ReplicationClient::CompareRecord(ECS::Entity entity, const Confirmed &confirmed, std::uint64_t serverTick,
                                      PredictionHistory &history, std::vector<PredictionCounts *> &differing)
{
    const PredictedRecord &record = history.records[serverTick % kPredictionHistoryTicks];
    EntityPredictionCounts &counts = history.counts;

    if (confirmed.physicsTick == serverTick && record.hasCharacter && confirmed.body.character)
    {
        ++counts.physics.compares;
        const float divergence = glm::length(record.character.position - confirmed.body.position);
        _corrections.divergenceSum += static_cast<double>(divergence);
        _corrections.divergenceMax = std::max(_corrections.divergenceMax, divergence);
        if (std::string difference = PhysicsDifference(confirmed.body, record); !difference.empty())
        {
            counts.physics.firstDifference = std::move(difference);
            differing.push_back(&counts.physics);
        }
    }

    if (confirmed.componentTick != serverTick)
    {
        return;
    }
    // A character's Transform is its body's pose, which its physics frame
    // compares; the host sends that Transform only when the entity first
    // arrives, so the confirmed copy of it is that old.
    const bool character = _scene.Has<Physics::Character>(entity);
    for (std::size_t i = 0; i < _predictedComponents.size(); ++i)
    {
        const Core::Reflect::ComponentMeta &meta = *_predictedComponents[i];
        if ((character && meta.id == _transformComponentId) || (record.present & (std::uint64_t{1} << i)) == 0)
        {
            continue;
        }
        const Confirmed::Components::const_iterator host = confirmed.components.find(meta.id);
        if (host == confirmed.components.end() || host->second.size() != meta.size)
        {
            continue;
        }
        PredictionCounts &section = counts.components[meta.id];
        ++section.compares;
        const void *mine = record.components.data() + _predictedOffsets[i];
        if (meta.predictedEquals(mine, host->second.data()))
        {
            continue;
        }
        section.firstDifference = meta.predictedDifference(mine, host->second.data());
        differing.push_back(&section);
    }
}

std::string ReplicationClient::PhysicsDifference(const BodyState &host, const PredictedRecord &record) const
{
    const ViewSmoothing &smoothing = Smoothing();
    const Physics::CharacterFrame &mine = record.character;
    if (glm::length(mine.position - host.position) > smoothing.predictionTolerance)
    {
        return Difference("position", mine.position, host.position);
    }
    // A character faces the way its Transform turns, which the host sends as
    // the body's rotation.
    if (const std::optional<std::size_t> offset = TransformRecordOffset(); offset.has_value())
    {
        ECS::Transform transform;
        std::memcpy(&transform, record.components.data() + *offset, sizeof(ECS::Transform));
        if (AngleBetween(transform.rotation, host.rotation) > smoothing.predictionRotationTolerance)
        {
            return Difference("rotation", transform.rotation, host.rotation);
        }
    }
    if (glm::length(mine.velocity - host.linearVelocity) > smoothing.predictionVelocityTolerance)
    {
        return Difference("velocity", mine.velocity, host.linearVelocity);
    }
    if (mine.stance != host.stance)
    {
        return Difference("stance", mine.stance, host.stance);
    }
    if (mine.ground != host.ground)
    {
        return Difference("ground", mine.ground, host.ground);
    }
    return {};
}

std::optional<std::size_t> ReplicationClient::TransformRecordOffset() const
{
    for (std::size_t i = 0; i < _predictedComponents.size(); ++i)
    {
        if (_predictedComponents[i]->id == _transformComponentId)
        {
            return _predictedOffsets[i];
        }
    }
    return std::nullopt;
}

void ReplicationClient::HardSnap(NetId netId, ECS::Entity entity, const Confirmed &confirmed)
{
    ++_corrections.hardSnaps;
    const bool character = _scene.Has<Physics::Character>(entity);
    if (confirmed.componentTick != 0)
    {
        WriteHostComponents(entity, confirmed, character);
    }
    if (_physics != nullptr && confirmed.physicsTick != 0 && confirmed.body.character &&
        PredictsCharacter(netId, entity))
    {
        PlaceCharacter(entity, _physics->CaptureCharacter(entity), confirmed.body);
    }
    if (_scene.Has<ECS::Transform>(entity))
    {
        ECS::SnapTransform(_scene, entity);
    }
    // Shown as it is: there is nothing nearer the truth to ease from.
    if (const std::unordered_map<NetId, MirrorBody>::iterator body = _bodies.find(netId); body != _bodies.end())
    {
        body->second.positionError = glm::vec3{0.f};
        body->second.rotationError = glm::quat{1.f, 0.f, 0.f, 0.f};
        body->second.smoothingWindow = 0.f;
    }
}

void ReplicationClient::Restore(ECS::Entity entity, const PredictedRecord &record, const Confirmed *host)
{
    for (std::size_t i = 0; i < _predictedComponents.size(); ++i)
    {
        if ((record.present & (std::uint64_t{1} << i)) == 0)
        {
            continue;
        }
        const Core::Reflect::ComponentMeta &meta = *_predictedComponents[i];
        void *live = meta.getMutable(&_scene, entity.index, entity.generation);
        if (live != nullptr)
        {
            std::memcpy(live, record.components.data() + _predictedOffsets[i], meta.size);
        }
    }
    if (host != nullptr && host->componentTick == record.tick)
    {
        WriteHostComponents(entity, *host, _scene.Has<Physics::Character>(entity));
    }

    if (!record.hasCharacter)
    {
        return;
    }
    if (host != nullptr && host->physicsTick == record.tick && host->body.character)
    {
        PlaceCharacter(entity, record.character, host->body);
    }
    else
    {
        _physics->RewindCharacter(entity, record.character);
    }
}

void ReplicationClient::WriteHostComponents(ECS::Entity entity, const Confirmed &confirmed, bool skipTransform)
{
    const Core::Reflect::ComponentRegistry &registry = Core::Reflect::ComponentRegistry::Instance();
    for (const auto &[componentId, bytes] : confirmed.components)
    {
        if (skipTransform && componentId == _transformComponentId)
        {
            continue;
        }
        const Core::Reflect::ComponentMeta *meta = registry.ById(componentId);
        void *live = meta != nullptr ? meta->getMutable(&_scene, entity.index, entity.generation) : nullptr;
        if (live != nullptr && bytes.size() == meta->size)
        {
            std::memcpy(live, bytes.data(), meta->size);
        }
    }
}

void ReplicationClient::PlaceCharacter(ECS::Entity entity, Physics::CharacterFrame frame, const BodyState &host)
{
    // The wire carries where the host had it, how fast it went, how it stood
    // and which way it faced, not its jump timers or eye height: those stay
    // the client's own.
    frame.position = host.position;
    frame.velocity = host.linearVelocity;
    frame.stance = host.stance;
    _physics->RewindCharacter(entity, frame);
    if (ECS::Transform *transform = _scene.GetMut<ECS::Transform>(entity); transform != nullptr)
    {
        transform->rotation = host.rotation;
    }
}

std::optional<ReplicationClient::Replay> ReplicationClient::BeginRollback()
{
    ASSISI_ASSERT(!_rollingBack, "ReplicationClient::BeginRollback while a rollback is running");
    std::uint64_t from = 0;
    for (const auto &[netId, history] : _history)
    {
        (void)netId;
        from = std::max(from, history.mispredictedAt);
    }
    if (from == 0)
    {
        return std::nullopt;
    }
    if (_recordedTicks[from % kPredictionHistoryTicks] != from)
    {
        for (auto &[netId, history] : _history)
        {
            (void)netId;
            history.mispredictedAt = 0;
        }
        return std::nullopt;
    }

    Replay replay;
    replay.from = from;
    for (std::uint64_t tick = from + 1u; tick <= _newestRecordedTick; ++tick)
    {
        if (_recordedTicks[tick % kPredictionHistoryTicks] == tick)
        {
            replay.ticks.push_back(tick);
        }
    }

    // Every predicted entity goes back to the same tick: the one the host
    // disagreed with to the host's word, the rest to their own records. Put
    // back to the host's, an entity it agreed with would move by the host's
    // rounding on every rollback of its neighbour.
    _renderedBeforeReplay.clear();
    std::vector<ECS::Entity> replayed;
    for (auto &[netId, history] : _history)
    {
        const bool mispredicted = history.mispredictedAt == from;
        history.mispredictedAt = 0;
        const ECS::Entity entity = EntityOf(netId);
        const PredictedRecord &record = history.records[from % kPredictionHistoryTicks];
        if (!_scene.IsAlive(entity) || record.tick != from)
        {
            continue;
        }
        if (record.hasCharacter)
        {
            const ECS::Transform *transform = _scene.Get<ECS::Transform>(entity);
            const Physics::Pose simulated{transform != nullptr ? transform->rotation : glm::quat{1.f, 0.f, 0.f, 0.f},
                                          _physics->CaptureCharacter(entity).position};
            const std::unordered_map<NetId, MirrorBody>::const_iterator body = _bodies.find(netId);
            _renderedBeforeReplay.push_back(RenderedBeforeReplay{
                    .pose = body == _bodies.end()
                            ? simulated
                            : Physics::Pose{body->second.rotationError * simulated.rotation,
                                            simulated.position + body->second.positionError},
                    .netId = netId});
            replay.characters.push_back(entity);
        }
        const std::unordered_map<NetId, Confirmed>::const_iterator host = _confirmed.find(netId);
        Restore(entity, record, mispredicted && host != _confirmed.end() ? &host->second : nullptr);
        replayed.push_back(entity);
    }

    _scene.BeginReplay(replayed);
    _rollingBack = true;
    ++_corrections.predictionsReplayed;
    return replay;
}

ReplicationClient::AppliedCommand ReplicationClient::CommandAppliedAt(std::uint64_t tick) const
{
    const InputCommand &sent = _sentCommands[tick % kPredictionHistoryTicks];
    const AppliedCommand own{.command = sent.tick == tick ? &sent : nullptr, .starvedTicks = 0};
    if (tick > _echo.serverTick || _echo.serverTick - tick >= kStarvedMaskTicks)
    {
        return own;
    }

    // The host's count of steps in a row without a command, as it stood at
    // the tick: every starved bit from it back to the last one it had.
    std::uint32_t starved = 0;
    for (std::uint64_t back = _echo.serverTick - tick; back < kStarvedMaskTicks; ++back)
    {
        if ((_echo.starvedMask & (std::uint64_t{1} << back)) == 0)
        {
            break;
        }
        ++starved;
    }
    return starved == 0 ? own : AppliedCommand{.command = nullptr, .starvedTicks = starved};
}

void ReplicationClient::EndRollback()
{
    ASSISI_ASSERT(_rollingBack, "ReplicationClient::EndRollback without BeginRollback");
    _rollingBack = false;
    _scene.EndReplay();

    // Only a live step blends; the replay's last pose is where each predicted
    // entity is, not a step to be drawn moving into.
    for (const auto &[netId, confirmed] : _confirmed)
    {
        (void)confirmed;
        const ECS::Entity entity = EntityOf(netId);
        if (_scene.IsAlive(entity) && _scene.Has<ECS::Transform>(entity))
        {
            ECS::SnapTransform(_scene, entity);
        }
    }

    for (const RenderedBeforeReplay &before : _renderedBeforeReplay)
    {
        const ECS::Entity entity = EntityOf(before.netId);
        const std::unordered_map<NetId, MirrorBody>::iterator body = _bodies.find(before.netId);
        const ECS::Transform *transform = _scene.Get<ECS::Transform>(entity);
        if (!_scene.IsAlive(entity) || body == _bodies.end() || transform == nullptr)
        {
            continue;
        }
        AbsorbCorrection(body->second, before.pose,
                         Physics::Pose{transform->rotation, _physics->CaptureCharacter(entity).position});
    }
    _renderedBeforeReplay.clear();
}

const ReplicationClient::EntityPredictionCounts *ReplicationClient::PredictionCountsOf(NetId netId) const
{
    const std::unordered_map<NetId, PredictionHistory>::const_iterator found = _history.find(netId);
    return found == _history.end() ? nullptr : &found->second.counts;
}

std::uint64_t ReplicationClient::SnapshotIntervalTicks() const
{
    return std::max<std::uint64_t>(1u, _tickRateHz / std::max(_snapshotHz, 1u));
}

} // namespace Assisi::NetSync
