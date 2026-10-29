/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/Core/Reflect/PredictedCompare.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <utility>
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

/// Counts a correction the host's word for @p tick made to @p section, apart
/// when the host stepped that tick without this client's command.
void CountCorrection(ReplicationClient::PredictionCounts &section, const CommandEcho &echo, std::uint64_t tick)
{
    if (echo.Applied(tick))
    {
        ++section.corrections;
    }
    else
    {
        ++section.starvedCorrections;
    }
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
    Record(command.tick, RecordedPart::Everything);
    RecordReports(command.tick);
}

void ReplicationClient::RecordReports(std::uint64_t tick)
{
    BodyReport &report = _reports[tick % kPredictionHistoryTicks];
    report.tick = tick;
    report.bodies.clear();
    if (_physics == nullptr)
    {
        return;
    }
    for (const NetId netId : _simulated)
    {
        const ECS::Entity entity = EntityOf(netId);
        if (!_scene.IsAlive(entity) || !_physics->HasBody(entity) || report.bodies.size() >= kMaxReportedBodies)
        {
            continue;
        }
        const Physics::Pose pose = _physics->GetBodyPose(entity);
        const Physics::BodyState motion = _physics->GetBodyState(entity);
        report.bodies.push_back(BodyState{.netId = netId,
                                          .position = pose.position,
                                          .rotation = pose.rotation,
                                          .linearVelocity = motion.linearVelocity,
                                          .angularVelocity = motion.angularVelocity,
                                          .asleep = motion.asleep});
    }
}

void ReplicationClient::RecordHistory(std::uint64_t tick)
{
    ASSISI_ASSERT(_rollingBack, "ReplicationClient::RecordHistory outside a rollback");
    Record(tick, RecordedPart::Stepped);
}

void ReplicationClient::Record(std::uint64_t tick, RecordedPart part)
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
        // A replay steps only the Transform and the physics frame. The rest of
        // the record stays what the live step had, which is what the host's
        // answers about the tick are compared with.
        const bool whole = part == RecordedPart::Everything || record.tick != tick;
        record.tick = tick;
        if (whole)
        {
            record.present = 0;
            record.components.resize(_predictedBytes);
        }
        for (std::size_t i = 0; i < _predictedComponents.size(); ++i)
        {
            const Core::Reflect::ComponentMeta &meta = *_predictedComponents[i];
            if (!whole && meta.id != _transformComponentId)
            {
                continue;
            }
            const std::uint64_t bit = std::uint64_t{1} << i;
            record.present &= ~bit;
            const void *live = meta.getByEntity(&_scene, entity.index, entity.generation);
            if (live == nullptr)
            {
                continue;
            }
            std::memcpy(record.components.data() + _predictedOffsets[i], live, meta.size);
            record.present |= bit;
        }
        record.hasCharacter = _physics != nullptr && PredictsCharacter(netId, entity) && _physics->HasBody(entity);
        if (record.hasCharacter)
        {
            record.character = _physics->CaptureCharacter(entity);
        }
        record.hasBody = PredictsBody(netId, entity);
        if (record.hasBody)
        {
            record.body = CaptureBody(netId, entity);
        }
    }
    RecordFollowers(tick);
}

void ReplicationClient::RecordFollowers(std::uint64_t tick)
{
    std::vector<FollowerRecord> &followers = _followers[tick % kPredictionHistoryTicks];
    _replacedFollowers.swap(followers);
    followers.clear();
    if (_physics == nullptr)
    {
        return;
    }
    _followerEntities.clear();
    _physics->Followers(_followerEntities);
    // A body this client simulates was where its own steps put it, so a step
    // stepped again meets it there, in a rollback as in the sync test: the
    // host has it in the same place, from this client's reports.
    for (const NetId netId : _simulated)
    {
        const ECS::Entity entity = EntityOf(netId);
        if (_scene.IsAlive(entity) && _physics->HasBody(entity))
        {
            _followerEntities.push_back(entity);
        }
    }
    for (const ECS::Entity entity : _followerEntities)
    {
        // A replay only puts back a follower it does not step, where the live
        // record of the tick has it. That record's start may hold a correction
        // made since, which a record taken again would lose, and the end of
        // the replay would undo.
        if (_rollingBack && !_scene.IsReplayed(entity))
        {
            const std::vector<FollowerRecord>::const_iterator live =
                std::find_if(_replacedFollowers.begin(), _replacedFollowers.end(),
                             [entity](const FollowerRecord &record) { return record.entity == entity; });
            if (live != _replacedFollowers.end())
            {
                followers.push_back(*live);
                continue;
            }
        }
        const Physics::FollowerFrame frame = _physics->CaptureFollower(entity);
        followers.push_back(FollowerRecord{.frame = frame, .start = frame, .entity = entity});
    }
}

void ReplicationClient::RecordFollowerNow(ECS::Entity entity)
{
    // Left out, the correction would be undone by the next rollback, which
    // leaves each follower where the newest step left it, and a later one would
    // step the next tick again from where the live step did not.
    const std::uint64_t tick = _newestRecordedTick;
    if (_physics == nullptr || tick == 0 || _recordedTicks[tick % kPredictionHistoryTicks] != tick)
    {
        return;
    }
    for (FollowerRecord &follower : _followers[tick % kPredictionHistoryTicks])
    {
        if (follower.entity == entity)
        {
            follower.start = _physics->CaptureFollower(entity);
            return;
        }
    }
}

void ReplicationClient::PlaceFollowersNow()
{
    const std::uint64_t tick = _newestRecordedTick;
    if (_physics == nullptr || tick == 0 || _recordedTicks[tick % kPredictionHistoryTicks] != tick)
    {
        return;
    }
    for (const FollowerRecord &follower : _followers[tick % kPredictionHistoryTicks])
    {
        _physics->PlaceFollower(follower.entity, follower.start);
    }
}

std::vector<Physics::FollowerStep> ReplicationClient::FollowersAt(std::uint64_t tick) const
{
    std::vector<Physics::FollowerStep> steps;
    const std::uint64_t previous = tick - 1u;
    if (_recordedTicks[tick % kPredictionHistoryTicks] != tick ||
        _recordedTicks[previous % kPredictionHistoryTicks] != previous)
    {
        return steps;
    }
    // Both in the order the physics world lists them, so one pass pairs them.
    const std::vector<FollowerRecord> &from = _followers[previous % kPredictionHistoryTicks];
    const std::vector<FollowerRecord> &to = _followers[tick % kPredictionHistoryTicks];
    std::vector<FollowerRecord>::const_iterator start = from.begin();
    for (const FollowerRecord &end : to)
    {
        // Other players follow only in the sync test, which no real rollback
        // looks like. A rollback starts from the host's word, and the snapshot
        // that brought it has just put every other player where the host had
        // them then, so they stand where they are. The sync test steps a tick
        // again from this client's own record instead, to check that a replay
        // puts back everything it steps and steps it the same way twice; for
        // that, everything the replay meets has to be what the live step met,
        // other players included. Met where they are now, they would show as
        // differences that are no fault of the replay and bury one that is.
        if (!_syncTesting && _scene.Has<Physics::Character>(end.entity))
        {
            continue;
        }
        const std::vector<FollowerRecord>::const_iterator found = std::find_if(
            start, from.end(), [&end](const FollowerRecord &record) { return record.entity == end.entity; });
        if (found == from.end())
        {
            continue;
        }
        steps.push_back(Physics::FollowerStep{.from = found->start, .to = end.frame, .entity = end.entity});
        start = found + 1;
    }
    return steps;
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
        const bool componentsCorrected = CompareRecord(entity, confirmed, serverTick, history, differing);
        if (differing.empty())
        {
            if (!componentsCorrected)
            {
                ++_corrections.predictionsKept;
            }
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
            CountCorrection(*section, _echo, serverTick);
        }
        history.mispredictedAt = serverTick;
        history.cooldownUntil = _newestRecordedTick + SnapshotIntervalTicks();
    }
}

bool ReplicationClient::CompareRecord(ECS::Entity entity, const Confirmed &confirmed, std::uint64_t serverTick,
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
    if (confirmed.physicsTick == serverTick && record.hasBody && !confirmed.body.character)
    {
        ++counts.physics.compares;
        if (std::string difference = PredictedBodyDifference(record.body, confirmed.body, Smoothing());
            !difference.empty())
        {
            counts.physics.firstDifference = std::move(difference);
            differing.push_back(&counts.physics);
        }
    }

    if (confirmed.componentTick != serverTick)
    {
        return false;
    }
    // A character's or a body's Transform is its body's pose, which its
    // physics frame compares; the host sends that Transform only when the
    // entity first arrives, so the confirmed copy of it is that old.
    const bool framed = _scene.Has<Physics::Character>(entity) || record.hasBody;
    bool corrected = false;
    for (std::size_t i = 0; i < _predictedComponents.size(); ++i)
    {
        const Core::Reflect::ComponentMeta &meta = *_predictedComponents[i];
        if ((framed && meta.id == _transformComponentId) || (record.present & (std::uint64_t{1} << i)) == 0)
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
        // A replay steps the Transform again, from controls whose turning
        // adds up; every other component no replay steps takes the host's
        // value now.
        if (meta.id == _transformComponentId)
        {
            differing.push_back(&section);
            continue;
        }
        if (void *live = meta.getMutable(&_scene, entity.index, entity.generation); live != nullptr)
        {
            std::memcpy(live, host->second.data(), meta.size);
            CountCorrection(section, _echo, serverTick);
            corrected = true;
        }
    }
    return corrected;
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
    // In the world, as the host sends it: a rider's own velocity is its
    // base's frame's.
    if (glm::length(mine.worldVelocity - host.linearVelocity) > smoothing.predictionVelocityTolerance)
    {
        return Difference("velocity", mine.worldVelocity, host.linearVelocity);
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
    const bool predictedBody = PredictsBody(netId, entity);
    if (confirmed.componentTick != 0)
    {
        WriteHostComponents(entity, confirmed, _scene.Has<Physics::Character>(entity) || predictedBody);
    }
    if (_physics != nullptr && confirmed.physicsTick != 0 && confirmed.body.character &&
        PredictsCharacter(netId, entity))
    {
        PlaceCharacter(entity, _physics->CaptureCharacter(entity), confirmed.body);
    }
    if (predictedBody && confirmed.physicsTick != 0 && !confirmed.body.character)
    {
        PlaceBody(entity, confirmed.body);
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
    // A body's Transform is its pose.
    if (record.hasBody)
    {
        PlaceBody(entity, host != nullptr && host->physicsTick == record.tick ? host->body : record.body);
        return;
    }

    // Only what a replay steps again goes back: the Transform here and the
    // physics frame below. Every other predicted component stays as it is.
    for (std::size_t i = 0; i < _predictedComponents.size(); ++i)
    {
        const Core::Reflect::ComponentMeta &meta = *_predictedComponents[i];
        if (meta.id != _transformComponentId || (record.present & (std::uint64_t{1} << i)) == 0)
        {
            continue;
        }
        void *live = meta.getMutable(&_scene, entity.index, entity.generation);
        if (live != nullptr)
        {
            std::memcpy(live, record.components.data() + _predictedOffsets[i], meta.size);
        }
    }
    if (host != nullptr && host->componentTick == record.tick && !_scene.Has<Physics::Character>(entity))
    {
        WriteHostComponent(entity, *host, _transformComponentId);
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
    for (const auto &[componentId, bytes] : confirmed.components)
    {
        (void)bytes;
        if (skipTransform && componentId == _transformComponentId)
        {
            continue;
        }
        WriteHostComponent(entity, confirmed, componentId);
    }
}

void ReplicationClient::WriteHostComponent(ECS::Entity entity, const Confirmed &confirmed,
                                           Core::Reflect::ComponentId componentId)
{
    const Confirmed::Components::const_iterator bytes = confirmed.components.find(componentId);
    const Core::Reflect::ComponentMeta *meta = Core::Reflect::ComponentRegistry::Instance().ById(componentId);
    if (bytes == confirmed.components.end() || meta == nullptr || bytes->second.size() != meta->size)
    {
        return;
    }
    if (void *live = meta->getMutable(&_scene, entity.index, entity.generation); live != nullptr)
    {
        std::memcpy(live, bytes->second.data(), meta->size);
    }
}

void ReplicationClient::PlaceCharacter(ECS::Entity entity, Physics::CharacterFrame frame, const BodyState &host)
{
    // The wire carries where the host had it, how fast it went, how it stood
    // and which way it faced, not its jump timers or eye height: those stay
    // the client's own.
    // The host's velocity is in the world; a rider's own is relative to its
    // base, which moved as fast then as the frame records.
    frame.position = host.position;
    frame.velocity = host.linearVelocity - (frame.worldVelocity - frame.velocity);
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
    std::optional<Replay> replay = RollBackTo(from);
    if (replay.has_value())
    {
        ++_corrections.predictionsReplayed;
    }
    return replay;
}

std::optional<ReplicationClient::Replay> ReplicationClient::BeginSyncTest()
{
    const std::uint64_t now = _newestRecordedTick;
    if (!_syncTest || _rollingBack || now == 0 || _recordedTicks[(now - 1u) % kPredictionHistoryTicks] != now - 1u)
    {
        return std::nullopt;
    }
    _syncLive.clear();
    for (const auto &[netId, history] : _history)
    {
        const PredictedRecord &record = history.records[now % kPredictionHistoryTicks];
        if (record.tick == now)
        {
            _syncLive.emplace_back(netId, record);
        }
    }
    _syncTick = now;
    std::optional<Replay> replay = RollBackTo(now - 1u);
    _syncTesting = replay.has_value();
    return replay;
}

void ReplicationClient::EndSyncTest()
{
    _syncTesting = false;
    ++_syncTestCounts.replays;
    for (const auto &[netId, live] : _syncLive)
    {
        const std::unordered_map<NetId, PredictionHistory>::const_iterator found = _history.find(netId);
        if (found == _history.end())
        {
            continue;
        }
        const PredictedRecord &replayed = found->second.records[_syncTick % kPredictionHistoryTicks];
        for (const std::string &difference : SyncDifferences(live, replayed))
        {
            ++_syncTestCounts.differences;
            Core::Log::Warn("Sync test: tick {}, entity {}: stepped again, {} (live, then replayed).", _syncTick,
                            netId.value, difference);
        }
    }
    _syncLive.clear();
}

std::vector<std::string> ReplicationClient::SyncDifferences(const PredictedRecord &live,
                                                            const PredictedRecord &replayed) const
{
    std::vector<std::string> differences;
    if (replayed.tick != live.tick)
    {
        differences.push_back("the record: missing");
        return differences;
    }
    if (live.hasCharacter && replayed.hasCharacter)
    {
        const Physics::CharacterFrame &a = live.character;
        const Physics::CharacterFrame &b = replayed.character;
        if (!Core::Reflect::PredictedFieldEqual(a.position, b.position))
        {
            differences.push_back(Difference("position", a.position, b.position));
        }
        if (!Core::Reflect::PredictedFieldEqual(a.worldVelocity, b.worldVelocity))
        {
            differences.push_back(Difference("velocity", a.worldVelocity, b.worldVelocity));
        }
        if (a.stance != b.stance || a.ground != b.ground || a.base != b.base)
        {
            differences.push_back(Difference("stance, ground and base",
                                             glm::uvec3(static_cast<std::uint32_t>(a.stance),
                                                        static_cast<std::uint32_t>(a.ground), a.base.index),
                                             glm::uvec3(static_cast<std::uint32_t>(b.stance),
                                                        static_cast<std::uint32_t>(b.ground), b.base.index)));
        }
    }
    for (std::size_t i = 0; i < _predictedComponents.size(); ++i)
    {
        const std::uint64_t bit = std::uint64_t{1} << i;
        if ((live.present & bit) == 0 || (replayed.present & bit) == 0)
        {
            continue;
        }
        const Core::Reflect::ComponentMeta &meta = *_predictedComponents[i];
        const void *a = live.components.data() + _predictedOffsets[i];
        const void *b = replayed.components.data() + _predictedOffsets[i];
        if (!meta.predictedEquals(a, b))
        {
            differences.push_back(std::format("{} {}", meta.name, meta.predictedDifference(a, b)));
        }
    }
    return differences;
}

std::optional<ReplicationClient::Replay> ReplicationClient::RollBackTo(std::uint64_t from)
{
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
        if (record.hasCharacter || record.hasBody)
        {
            const Physics::Pose simulated = SimulatedPose(entity);
            const std::unordered_map<NetId, MirrorBody>::const_iterator body = _bodies.find(netId);
            _renderedBeforeReplay.push_back(RenderedBeforeReplay{
                    .pose = body == _bodies.end()
                            ? simulated
                            : Physics::Pose{body->second.rotationError * simulated.rotation,
                                            simulated.position + body->second.positionError},
                    .netId = netId});
        }
        if (record.hasCharacter)
        {
            replay.characters.push_back(entity);
        }
        if (record.hasBody)
        {
            replay.bodies.push_back(entity);
        }
        const std::unordered_map<NetId, Confirmed>::const_iterator host = _confirmed.find(netId);
        Restore(entity, record, mispredicted && host != _confirmed.end() ? &host->second : nullptr);
        replayed.push_back(entity);
    }

    _scene.BeginReplay(replayed);
    _rollingBack = true;
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

void ReplicationClient::RestoreIntentsAt(std::uint64_t tick)
{
    ASSISI_ASSERT(_rollingBack, "ReplicationClient::RestoreIntentsAt outside a rollback");
    for (const auto &[netId, history] : _history)
    {
        const PredictedRecord &record = history.records[tick % kPredictionHistoryTicks];
        if (record.tick != tick || !record.hasCharacter)
        {
            continue;
        }
        Physics::CharacterIntent *intent = _scene.GetMut<Physics::CharacterIntent>(EntityOf(netId));
        if (intent != nullptr)
        {
            *intent = record.character.intent;
        }
    }
}

void ReplicationClient::EndRollback()
{
    ASSISI_ASSERT(_rollingBack, "ReplicationClient::EndRollback without BeginRollback");
    _rollingBack = false;
    _scene.EndReplay();
    PlaceFollowersNow();

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
        if (!_scene.IsAlive(entity) || body == _bodies.end() || !_scene.Has<ECS::Transform>(entity))
        {
            continue;
        }
        AbsorbCorrection(body->second, before.pose, SimulatedPose(entity));
    }
    _renderedBeforeReplay.clear();
}

const ReplicationClient::EntityPredictionCounts *ReplicationClient::PredictionCountsOf(NetId netId) const
{
    const std::unordered_map<NetId, PredictionHistory>::const_iterator found = _history.find(netId);
    return found == _history.end() ? nullptr : &found->second.counts;
}

std::vector<NetId> ReplicationClient::PredictedEntities() const
{
    std::vector<NetId> predicted;
    predicted.reserve(_history.size());
    for (const auto &[netId, history] : _history)
    {
        (void)history;
        predicted.push_back(netId);
    }
    std::ranges::sort(predicted);
    return predicted;
}

std::uint64_t ReplicationClient::SnapshotIntervalTicks() const
{
    return std::max<std::uint64_t>(1u, _tickRateHz / std::max(_snapshotHz, 1u));
}

} // namespace Assisi::NetSync
