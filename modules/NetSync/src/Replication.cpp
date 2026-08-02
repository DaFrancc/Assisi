/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/Replication.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/NetComponents.hpp>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <utility>

namespace Assisi::NetSync
{
namespace
{

/// Entity handles are (index, generation); the maps here want one integer key.
/// Packing is local-only — this value never reaches the wire, where NetId is the
/// sole identity.
std::uint64_t PackEntity(ECS::Entity entity)
{
    return (static_cast<std::uint64_t>(entity.index) << 32) | static_cast<std::uint64_t>(entity.generation);
}

/// `(netId, componentId)` as one sortable integer. The component-set diff that
/// finds removals is a set_difference over these, so they must order by entity
/// first and component second — which the shift gives for free.
std::uint64_t PackComponentRef(NetId netId, Core::Reflect::ComponentId componentId)
{
    return (static_cast<std::uint64_t>(netId) << 32) | static_cast<std::uint64_t>(componentId);
}

NetId NetIdOfRef(std::uint64_t packed) { return static_cast<NetId>(packed >> 32); }

Core::Reflect::ComponentId ComponentIdOfRef(std::uint64_t packed)
{
    return static_cast<Core::Reflect::ComponentId>(packed & 0xFFFFFFFFull);
}

ECS::Entity UnpackEntity(std::uint64_t packed)
{
    return ECS::Entity{static_cast<std::uint32_t>(packed >> 32), static_cast<std::uint32_t>(packed & 0xFFFFFFFFull)};
}

/// Ensure @p entity has the component @p meta describes, and hand back a
/// writable pointer to it.
///
/// Both hooks stamp the change tick, which is what a client applying a snapshot
/// wants: its own systems watch change ticks too, and an applied snapshot is a
/// change by any definition.
void *EnsureComponent(ECS::Scene &scene, ECS::Entity entity, const Core::Reflect::ComponentMeta &meta)
{
    if (void *existing = meta.getMutable(&scene, entity.index, entity.generation))
        return existing;
    return meta.construct(&scene, entity.index, entity.generation);
}

} // namespace

// ===========================================================================
// ReplicationServer
// ===========================================================================

ReplicationServer::ReplicationServer(Net::NetTransport &transport, ECS::Scene &scene,
                                     Physics::PhysicsWorld *physics, ReplicationConfig config)
    : _transport(transport), _scene(scene), _physics(physics), _config(config)
{
    // Clamp the snapshot rate to a divisor of the tick rate. A rate that does
    // not divide evenly makes the send interval alternate between two tick
    // counts, which reaches the player as interpolation judder and reaches the
    // developer as nothing at all — no error, no log. Better to quietly land on
    // the nearest workable rate and say so.
    if (_config.tickRateHz == 0)
        _config.tickRateHz = 60;
    if (_config.snapshotHz == 0)
        _config.snapshotHz = _config.tickRateHz;

    _snapshotDiv = std::max<std::uint64_t>(1, _config.tickRateHz / std::min(_config.snapshotHz, _config.tickRateHz));
    const std::uint32_t effectiveHz = static_cast<std::uint32_t>(_config.tickRateHz / _snapshotDiv);
    if (effectiveHz != _config.snapshotHz)
    {
        Core::Log::Info("NetSync: snapshot rate {} Hz is not a divisor of the {} Hz tick rate — using {} Hz.",
                        _config.snapshotHz, _config.tickRateHz, effectiveHz);
        _config.snapshotHz = effectiveHz;
    }

    // Resolve the replicated component set once: exactly the types annotated
    // ACOMP(replicated). Opt-in, and the opt-in is the point — "everything
    // serializable travels" shipped a `Camera` with every marked entity, whose
    // isActive could take over the receiving client's view, and would have put
    // every future gameplay-local component on the wire by default. The
    // Replicated marker is not in the set for a different reason: it says only
    // *that* an entity replicates, which the client learns from the spawn.
    for (const Core::Reflect::ComponentMeta *meta : Core::Reflect::ComponentRegistry::Instance().SerializableComponents())
    {
        if (meta->replicated)
            _replicatedComponents.push_back(meta->id);
    }

    _transformComponentId = Core::Reflect::ComponentRegistry::Instance().IdOf(typeid(ECS::Transform));
}

bool ReplicationServer::IsSnapshotTick(std::uint64_t simTick) const { return simTick % _snapshotDiv == 0; }

void ReplicationServer::AddConnection(Net::ConnectionId connection)
{
    Connection &entry = _connections[connection];
    entry.id          = connection;
    entry.ready       = false;
    SendHello(entry);
}

void ReplicationServer::RemoveConnection(Net::ConnectionId connection) { _connections.erase(connection); }

bool ReplicationServer::IsReady(Net::ConnectionId connection) const
{
    const auto it = _connections.find(connection);
    return it != _connections.end() && it->second.ready;
}

const ConnectionDiagnostics *ReplicationServer::Diagnostics(Net::ConnectionId connection) const
{
    const auto it = _connections.find(connection);
    return it == _connections.end() ? nullptr : &it->second.diagnostics;
}

NetId ReplicationServer::NetIdOf(ECS::Entity entity) const
{
    const auto it = _netIdByEntity.find(PackEntity(entity));
    return it == _netIdByEntity.end() ? InvalidNetId : it->second;
}

ECS::Entity ReplicationServer::EntityOf(NetId netId) const
{
    const auto it = _entityByNetId.find(netId);
    return it == _entityByNetId.end() ? ECS::NullEntity : it->second;
}

void ReplicationServer::SendHello(Connection &connection)
{
    Core::BitWriter writer;
    WriteMessageType(MessageType::ServerHello, writer);

    ServerHello hello;
    hello.protocolHash    = NetProtocolHash();
    hello.protocolSummary = NetProtocolSummary();
    hello.tickRateHz      = _config.tickRateHz;
    hello.snapshotHz      = _config.snapshotHz;
    hello.serverTick      = _simTick;
    hello.level           = _level;
    WriteServerHello(hello, writer);

    // Reliable: a lost handshake would leave the client waiting forever with
    // nothing to retry against.
    _transport.Send(connection.id, writer.Data(), Net::SendMode::Reliable, Net::Lane::Control);
}

void ReplicationServer::SendReject(Connection &connection, RejectReason reason)
{
    Core::BitWriter writer;
    WriteMessageType(MessageType::Reject, writer);
    writer.WriteBits(static_cast<std::uint32_t>(reason), 8);
    writer.WriteString(NetProtocolSummary());
    _transport.Send(connection.id, writer.Data(), Net::SendMode::Reliable, Net::Lane::Control);
}

void ReplicationServer::HandleMessage(Net::ConnectionId connection, std::span<const std::byte> payload)
{
    const auto it = _connections.find(connection);
    if (it == _connections.end())
        return;

    Core::BitReader   reader(payload);
    const MessageType type = ReadMessageType(reader);
    if (!reader.Ok())
        return;

    switch (type)
    {
    case MessageType::ClientHello: HandleClientHello(it->second, reader); break;
    case MessageType::Ack:         HandleAck(it->second, reader); break;
    case MessageType::Input:       HandleInput(it->second, reader); break;
    default:
        // Server-to-client messages arriving from a client are not a case to
        // handle; they are a client that is confused or lying.
        break;
    }
}

void ReplicationServer::HandleClientHello(Connection &connection, Core::BitReader &reader)
{
    ClientHello hello;
    if (!ReadClientHello(reader, hello))
        return;

    if (hello.protocolHash != NetProtocolHash())
    {
        // Two builds that disagree on component layout would corrupt each
        // other's state silently, which is far worse than not connecting.
        Core::Log::Warn("NetSync: rejecting connection {} — protocol hash mismatch.", connection.id);
        SendReject(connection, RejectReason::ProtocolMismatch);
        return;
    }

    connection.ready = true;
}

void ReplicationServer::HandleAck(Connection &connection, Core::BitReader &reader)
{
    const std::uint64_t ackedTick = reader.ReadVarUInt64();
    if (!reader.Ok())
        return;

    // Acks can arrive out of order; an older one carries no information we do
    // not already have.
    if (ackedTick <= connection.ackedTick)
        return;

    const auto record = std::find_if(connection.inFlight.begin(), connection.inFlight.end(),
                                     [ackedTick](const SentSnapshot &sent) { return sent.serverTick == ackedTick; });
    if (record == connection.inFlight.end())
        return; // acking something we never sent, or evicted long ago

    connection.acked           = std::move(record->netIds);
    connection.ackedComponents = std::move(record->components);
    connection.ackedTick       = record->serverTick;
    ++connection.diagnostics.acksReceived;

    // Fold in exactly the entities this snapshot *wrote*. One skipped for byte
    // budget has no entry here and keeps whatever baseline it had, which is the
    // whole point: "we mentioned it in the record" and "we delivered its state"
    // are different facts.
    for (const WrittenEntity &entity : record->written)
    {
        EntityBaseline &baseline = connection.baselines[entity.netId];
        baseline.componentTick   = std::max(baseline.componentTick, entity.ticks.componentTick);
        baseline.bodyTick        = std::max(baseline.bodyTick, entity.ticks.bodyTick);
    }

    // A NetId that has left the acked set is gone for good — they are never
    // reused, so a straggler ack cannot resurrect one. Without this the map
    // grows with every entity that has ever replicated.
    std::erase_if(connection.baselines,
                  [&connection](const auto &entry) {
                      return !std::binary_search(connection.acked.begin(), connection.acked.end(), entry.first);
                  });
    connection.diagnostics.baselineEntries = static_cast<std::uint32_t>(connection.baselines.size());

    // Everything at or before the acked tick is settled.
    connection.inFlight.erase(connection.inFlight.begin(), record + 1);
}

void ReplicationServer::ResetBaselines(Connection &connection)
{
    connection.baselines.clear();
    connection.inFlight.clear();
    connection.diagnostics.baselineEntries = 0;
    ++connection.diagnostics.keyframeSweeps;
}

void ReplicationServer::HandleInput(Connection &connection, Core::BitReader &reader)
{
    // Rate limit before the codec runs: a flood should cost us a comparison,
    // not a parse.
    const std::uint64_t window = _simTick / std::max<std::uint64_t>(1, _config.tickRateHz);
    if (window != connection.rateWindowTick)
    {
        connection.rateWindowTick  = window;
        connection.packetsInWindow = 0;
    }
    if (++connection.packetsInWindow > _config.maxInputPacketsPerSecond)
    {
        ++connection.diagnostics.inputPacketsDropped;
        return;
    }

    std::vector<InputCommand> commands;
    if (!InputCommandBuffer::ReadPacket(reader, commands))
    {
        ++connection.diagnostics.inputPacketsDropped;
        return;
    }

    for (InputCommand &command : commands)
    {
        // Server authority is not enough on its own: the client picks these
        // numbers. Clamp before the simulation ever sees them.
        if (!ClampInputCommand(command, _config.inputLimits))
            ++connection.diagnostics.commandsClamped;
        connection.input.Accept(command);
    }
}

const InputCommand *ReplicationServer::ConsumeInput(Net::ConnectionId connection, std::uint64_t tick)
{
    const auto it = _connections.find(connection);
    return it == _connections.end() ? nullptr : it->second.input.Consume(tick);
}

void ReplicationServer::ReconcileNetIds()
{
    _liveNetIds.clear();

    for (auto [entity, replicated] : _scene.Query<Replicated>())
    {
        (void)replicated;
        const std::uint64_t key = PackEntity(entity);
        const auto          it  = _netIdByEntity.find(key);
        NetId               netId;
        if (it == _netIdByEntity.end())
        {
            netId = _nextNetId++;
            _netIdByEntity.emplace(key, netId);
            _entityByNetId.emplace(netId, entity);
        }
        else
        {
            netId = it->second;
        }
        _liveNetIds.push_back(netId);
    }

    std::sort(_liveNetIds.begin(), _liveNetIds.end());

    // Drop mappings whose entity is gone or has stopped replicating. The NetId
    // itself is retired with it and never reused — a stale reference must fail
    // to resolve rather than quietly address whoever took the slot.
    for (auto it = _entityByNetId.begin(); it != _entityByNetId.end();)
    {
        const ECS::Entity entity = it->second;
        if (!_scene.IsAlive(entity) || !_scene.Has<Replicated>(entity))
        {
            _netIdByEntity.erase(PackEntity(entity));
            it = _entityByNetId.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void ReplicationServer::CaptureBodyStates()
{
    if (_physics == nullptr)
        return;

    ++_bodyStateTick;
    _physics->GetActiveBodyStates(_activeBodies);

    for (const NetId netId : _liveNetIds)
    {
        const ECS::Entity entity = EntityOf(netId);
        if (entity == ECS::NullEntity)
            continue;

        const Physics::RigidBody *body = _scene.Get<Physics::RigidBody>(entity);
        if (body == nullptr)
            continue; // not a simulated entity; its Transform replicates normally

        const auto active = std::find_if(_activeBodies.begin(), _activeBodies.end(),
                                         [entity](const Physics::PhysicsWorld::ActiveBodyState &candidate)
                                         { return candidate.entity == entity; });

        BodyRecord &record = _bodyStates[netId];
        if (active != _activeBodies.end())
        {
            record.state = BodyState{netId,   active->position,        active->rotation,
                                     active->linearVelocity, active->angularVelocity, /*asleep=*/false};
            record.tick  = _bodyStateTick;
            continue;
        }

        // Not active. Two reasons to record it anyway, and neither is "every
        // tick": it has just gone to sleep (the transition is the change, and
        // the one whose loss used to be permanent), or this is the first time we
        // have ever seen it — which is what makes joining an already-settled
        // world produce sleeping mirrors at the server's rest poses instead of a
        // client-side re-settle. A body that was already asleep last tick
        // records nothing, which is where the idle-bandwidth property comes from.
        if (record.tick == 0 || !record.state.asleep)
        {
            const auto [position, rotation] = _physics->GetBodyTransform(*body);
            record.state = BodyState{netId, position, rotation, glm::vec3{0.f}, glm::vec3{0.f}, /*asleep=*/true};
            record.tick  = _bodyStateTick;
        }
    }

    // Retired NetIds take their records with them.
    std::erase_if(_bodyStates,
                  [this](const auto &entry)
                  { return !std::binary_search(_liveNetIds.begin(), _liveNetIds.end(), entry.first); });
}

void ReplicationServer::WriteEntityComponents(NetId netId, ECS::Entity entity, std::uint64_t sinceChangeTick,
                                              const Connection &connection, Core::BitWriter &writer,
                                              std::vector<std::uint64_t> &outComponents)
{
    Core::Reflect::CodecContext context;
    context.entityToWire = [this](std::uint64_t packed) -> std::uint64_t
    {
        // Entity references cross the wire as NetIds. A reference to something
        // that does not replicate resolves to zero rather than to a local handle
        // the peer would misread as one of its own.
        const NetId referenced = NetIdOf(UnpackEntity(packed));
        return static_cast<std::uint64_t>(referenced);
    };

    const Core::Reflect::ComponentRegistry &registry = Core::Reflect::ComponentRegistry::Instance();

    // What this entity has right now, in the same packed, sorted form as the
    // acked baseline — _replicatedComponents is registry order, which is
    // ascending by id, so this comes out sorted without a sort.
    const std::size_t componentsBegin = outComponents.size();
    for (const Core::Reflect::ComponentId id : _replicatedComponents)
    {
        const Core::Reflect::ComponentMeta *meta = registry.ById(id);
        if (meta != nullptr && meta->getByEntity(&_scene, entity.index, entity.generation) != nullptr)
            outComponents.push_back(PackComponentRef(netId, id));
    }

    // Removals. Change detection stamps writes, not removals — there is no tick
    // to consult for "this component is gone" — so it is found by diffing this
    // entity's slice of the acked baseline against what it has now. Exactly the
    // shape of the despawn comparison, one level down.
    const auto ackedLow  = std::lower_bound(connection.ackedComponents.begin(), connection.ackedComponents.end(),
                                            PackComponentRef(netId, 0));
    const auto ackedHigh = std::lower_bound(connection.ackedComponents.begin(), connection.ackedComponents.end(),
                                            PackComponentRef(netId + 1, 0));

    std::vector<Core::Reflect::ComponentId> removed;
    for (auto it = ackedLow; it != ackedHigh; ++it)
    {
        const bool stillPresent = std::binary_search(outComponents.begin() + static_cast<std::ptrdiff_t>(componentsBegin),
                                                     outComponents.end(), *it);
        if (!stillPresent)
            removed.push_back(ComponentIdOfRef(*it));
    }

    writer.WriteVarUInt32(static_cast<std::uint32_t>(removed.size()));
    for (const Core::Reflect::ComponentId id : removed)
        writer.WriteVarUInt32(id);

    // For an entity the physics world owns, motion travels as body state, not as
    // a Transform. Sending both would double the cost of every moving object and
    // hand the client two disagreeing answers about where it is.
    const bool bodied = _physics != nullptr && _scene.Get<Physics::RigidBody>(entity) != nullptr;

    for (const Core::Reflect::ComponentId id : _replicatedComponents)
    {
        const Core::Reflect::ComponentMeta *meta = registry.ById(id);
        if (meta == nullptr)
            continue;

        const void *component = meta->getByEntity(&_scene, entity.index, entity.generation);
        if (component == nullptr)
            continue;

        // sinceChangeTick == 0 is the empty baseline: spawn, late join, and a
        // client that has acked nothing all take this path, which is the whole
        // point of not having a separate full-state message.
        if (sinceChangeTick != 0 && !_scene.ChangedById(entity, id, sinceChangeTick))
            continue;

        // ...and on that empty baseline a bodied entity's Transform still goes,
        // because it carries scale and the initial placement the client builds
        // its body at. Afterwards it is suppressed. The honest cost: a *non-pose*
        // Transform edit on a live body — a runtime scale change — reaches
        // clients only at the next keyframe sweep. Accepted for v1, because a
        // scale change on a live body needs a collider reshape the engine does
        // not do yet either (docs/replication-plan-v4.md §5).
        if (bodied && sinceChangeTick != 0 && id == _transformComponentId)
            continue;

        writer.WriteBool(true);
        Core::Reflect::WriteComponent(*meta, component, writer, Core::Reflect::kAllFields, &context);
    }
    writer.WriteBool(false);
}

void ReplicationServer::WriteBodyStates(Connection &connection, Core::BitWriter &writer, SentSnapshot &record,
                                        std::size_t writtenFromComponents)
{
    // Bool-chained like the entity blocks above, rather than the count-prefixed
    // form the design sketch shows. A count has to be known before the first
    // record is written, which means predicting the budget cut instead of
    // discovering it — and the bool chain is cheaper anyway (one bit per record
    // plus a terminator, against a varint).
    if (_physics == nullptr)
    {
        writer.WriteBool(false);
        return;
    }

    const auto prefixEnd = record.written.begin() + static_cast<std::ptrdiff_t>(writtenFromComponents);

    for (const NetId netId : _liveNetIds)
    {
        if (writer.BytesWritten() >= _config.maxSnapshotBytes)
            break; // the rest keep their baselines and go next snapshot

        const auto found = _bodyStates.find(netId);
        if (found == _bodyStates.end() || found->second.tick == 0)
            continue;

        const auto baseline = connection.baselines.find(netId);
        if (baseline != connection.baselines.end() && found->second.tick <= baseline->second.bodyTick)
            continue; // already delivered

        // The gate: a body state is only useful to a client that has something
        // to apply it to. Without it, an entity whose spawn block was cut for
        // budget would ship a body state the client has no mirror — and no
        // descriptor to build a body from — to receive it.
        //
        // `record.netIds` is this snapshot's entity set in ascending order, and
        // holds both the entities written here and the known ones the budget
        // skipped; the skipped ones are acked by definition, so testing against
        // it is exactly "acked, or written into this same snapshot".
        if (!std::binary_search(connection.acked.begin(), connection.acked.end(), netId) &&
            !std::binary_search(record.netIds.begin(), record.netIds.end(), netId))
        {
            continue;
        }

        writer.WriteBool(true);
        WriteBodyState(found->second.state, writer);

        // Record the delivery against this entity. It may already have an entry
        // from the component pass; if not, componentTick stays 0 and the ack's
        // max() leaves whatever component baseline it had untouched.
        const auto slot = std::lower_bound(record.written.begin(), prefixEnd, netId,
                                           [](const WrittenEntity &entry, NetId value)
                                           { return entry.netId < value; });
        if (slot != prefixEnd && slot->netId == netId)
            slot->ticks.bodyTick = found->second.tick;
        else
            record.written.push_back(WrittenEntity{netId, EntityBaseline{0, found->second.tick}});
    }

    writer.WriteBool(false);
}

void ReplicationServer::SendSnapshot(Connection &connection)
{
    Core::BitWriter writer;
    WriteMessageType(MessageType::Snapshot, writer);

    SnapshotHeader header;
    header.serverTick       = _simTick;
    header.baselineTick     = connection.ackedTick;
    header.inputBufferDepth = static_cast<std::uint32_t>(connection.input.Depth());
    header.starvedTicks     = static_cast<std::uint32_t>(connection.input.StarvedTicks());
    // Complete when the acked set already covers every live entity. Computed
    // against the *previous* ack rather than this snapshot, because that is the
    // only thing the client has actually confirmed receiving.
    header.worldComplete = std::includes(connection.acked.begin(), connection.acked.end(), _liveNetIds.begin(),
                                         _liveNetIds.end());
    WriteSnapshotHeader(header, writer);

    // Despawns: everything the client is known to have that no longer exists.
    // Falls straight out of comparing the acked entity set against the live one
    // — no separate bookkeeping to get out of step with reality.
    std::vector<NetId> despawns;
    std::set_difference(connection.acked.begin(), connection.acked.end(), _liveNetIds.begin(), _liveNetIds.end(),
                        std::back_inserter(despawns));
    writer.WriteVarUInt32(static_cast<std::uint32_t>(despawns.size()));
    for (const NetId netId : despawns)
        writer.WriteVarUInt32(netId);

    // The send loop is deliberately shaped as "collect, optionally order, drain
    // to a budget" even though v1 orders nothing and sends everything that fits.
    // A per-connection priority accumulator slots into the sort step later with
    // no change to anything on the wire.
    SentSnapshot record;
    record.serverTick = _simTick;
    record.netIds.reserve(_liveNetIds.size());
    record.written.reserve(_liveNetIds.size());

    // Sampled once, before anything is written, and stamped onto every entity
    // this snapshot writes. Nothing mutates the scene while a snapshot is being
    // built, so one reading is honest for all of them — and taking it up front
    // means a change made after this point cannot be mistaken for delivered.
    const std::uint64_t captureTick = _scene.CurrentChangeTick();

    for (const NetId netId : _liveNetIds)
    {
        const ECS::Entity entity = EntityOf(netId);
        if (entity == ECS::NullEntity)
            continue;

        const bool known = std::binary_search(connection.acked.begin(), connection.acked.end(), netId);

        if (writer.BytesWritten() >= _config.maxSnapshotBytes)
        {
            // Out of room. An entity the client already has simply misses this
            // update and stays in the record — its baseline is unaffected. One
            // it does not have must be left out of the record entirely, so the
            // next snapshot still treats it as a spawn.
            if (known)
            {
                record.netIds.push_back(netId);
                // Carry its component baseline forward untouched: we told the
                // client nothing about this entity, so nothing about it changed
                // from the client's point of view.
                const auto low  = std::lower_bound(connection.ackedComponents.begin(),
                                                   connection.ackedComponents.end(), PackComponentRef(netId, 0));
                const auto high = std::lower_bound(connection.ackedComponents.begin(),
                                                   connection.ackedComponents.end(), PackComponentRef(netId + 1, 0));
                record.components.insert(record.components.end(), low, high);
            }
            continue;
        }

        // The delta baseline is this entity's own. A missing entry reads as 0,
        // which is the empty baseline — spawn, late join, and a post-sweep
        // re-anchor all arrive here, and all take the one full-state path.
        std::uint64_t sinceChangeTick = 0;
        if (known)
        {
            if (const auto baseline = connection.baselines.find(netId); baseline != connection.baselines.end())
                sinceChangeTick = baseline->second.componentTick;
        }

        writer.WriteBool(true);
        writer.WriteVarUInt32(netId);
        writer.WriteBool(!known); // isSpawn

        WriteEntityComponents(netId, entity, sinceChangeTick, connection, writer, record.components);
        record.netIds.push_back(netId);
        record.written.push_back(WrittenEntity{netId, EntityBaseline{captureTick, 0}});
    }
    writer.WriteBool(false);

    // netIds is built in _liveNetIds order, which is sorted; the body pass below
    // binary-searches it, so make that explicit before it runs.
    std::sort(record.netIds.begin(), record.netIds.end());

    // Motion, for everything the physics world owns. After the entity blocks so
    // ordering with a spawn in the same packet is free: the entity and its
    // descriptor exist by the time the body state lands, and the client's body
    // starts at the authoritative state rather than re-settling from the level
    // file's pose.
    const std::size_t writtenFromComponents = record.written.size();
    WriteBodyStates(connection, writer, record, writtenFromComponents);

    // All three are built in ascending order — except `written`, which the body
    // pass may have appended to — so keep the invariant explicit, since the ack
    // path binary-searches them.
    std::sort(record.components.begin(), record.components.end());
    std::sort(record.written.begin(), record.written.end(),
              [](const WrittenEntity &lhs, const WrittenEntity &rhs) { return lhs.netId < rhs.netId; });

    connection.inFlight.push_back(std::move(record));
    while (connection.inFlight.size() > _config.maxInFlightSnapshots)
    {
        // The client has stopped acking. Dropping the oldest bounds our memory;
        // its ack, if it ever arrives, will simply find nothing and be ignored.
        connection.inFlight.pop_front();
    }

    // Unreliable: a lost snapshot must not be retransmitted. It would arrive
    // after the state it describes had already been superseded, costing latency
    // to deliver data that is worse than the data behind it.
    _transport.Send(connection.id, writer.Data(), Net::SendMode::Unreliable, Net::Lane::Snapshot);

    ++connection.diagnostics.snapshotsSent;
    connection.diagnostics.bytesSent += writer.BytesWritten();
    connection.diagnostics.inFlightSnapshots = static_cast<std::uint32_t>(connection.inFlight.size());
}

void ReplicationServer::Tick(std::uint64_t simTick)
{
    _simTick = simTick;

    // Once per tick, before any connection is served, so every client sees the
    // same world rather than a per-connection view of it.
    ReconcileNetIds();
    CaptureBodyStates();

    if (!IsSnapshotTick(simTick))
        return;

    // The keyframe sweep, which is not a third mechanism: it is the delta path
    // with its filter reset. Every entity's baseline goes back to zero, the
    // existing empty-baseline code path sends full state, and the byte budget
    // paginates it over as many snapshots as it needs — no new machinery, and
    // nothing on the wire that spawn and late-join do not already exercise.
    if (_config.keyframeIntervalTicks != 0 && simTick != 0 && simTick % _config.keyframeIntervalTicks == 0)
    {
        for (auto &[id, connection] : _connections)
        {
            if (connection.ready)
                ResetBaselines(connection);
        }
    }

    for (auto &[id, connection] : _connections)
    {
        if (connection.ready)
            SendSnapshot(connection);
    }
}

// ===========================================================================
// ReplicationClient
// ===========================================================================

ReplicationClient::ReplicationClient(Net::NetTransport &transport, ECS::Scene &scene, Net::ConnectionId connection,
                                     Physics::PhysicsWorld *physics)
    : _transport(transport), _scene(scene), _physics(physics), _connection(connection)
{
}

void ReplicationClient::SendHello()
{
    Core::BitWriter writer;
    WriteMessageType(MessageType::ClientHello, writer);

    ClientHello hello;
    hello.protocolHash = NetProtocolHash();
    WriteClientHello(hello, writer);

    _transport.Send(_connection, writer.Data(), Net::SendMode::Reliable, Net::Lane::Control);
}

void ReplicationClient::ConfirmLevelReady()
{
    if (_synchronized)
        return;

    _awaitingLevel = false;
    SendHello();
    _synchronized = true;
}

void ReplicationClient::AbortJoin(std::string reason)
{
    _awaitingLevel = false;
    _rejectMessage = std::move(reason);
    Core::Log::Error("NetSync: join aborted — {}", _rejectMessage);
}

void ReplicationClient::SendAck(std::uint64_t serverTick)
{
    Core::BitWriter writer;
    WriteMessageType(MessageType::Ack, writer);
    writer.WriteVarUInt64(serverTick);
    // Unreliable: an ack is a statement about a moment, and the next one
    // supersedes it. Retransmitting a stale ack would only make the server
    // delta against an older baseline than it needs to.
    _transport.Send(_connection, writer.Data(), Net::SendMode::Unreliable, Net::Lane::Control);
}

void ReplicationClient::SendInput(const InputCommandBuffer &buffer)
{
    if (!_synchronized)
        return;

    Core::BitWriter writer;
    WriteMessageType(MessageType::Input, writer);
    buffer.WritePacket(writer);
    _transport.Send(_connection, writer.Data(), Net::SendMode::Unreliable, Net::Lane::Snapshot);
}

void ReplicationClient::HandleMessage(std::span<const std::byte> payload)
{
    Core::BitReader   reader(payload);
    const MessageType type = ReadMessageType(reader);
    if (!reader.Ok())
        return;

    switch (type)
    {
    case MessageType::ServerHello:
    {
        ServerHello hello;
        if (!ReadServerHello(reader, hello))
            return;

        _tickRateHz = hello.tickRateHz;
        _snapshotHz = hello.snapshotHz;
        // Two snapshot intervals: enough that one lost or late snapshot does
        // not empty the buffer, and no more, since every tick of this is
        // latency the player sees on everyone else's position.
        _interpolationDelayTicks =
            hello.snapshotHz > 0 ? 2.0 * static_cast<double>(hello.tickRateHz) / hello.snapshotHz : 6.0;
        if (hello.protocolHash != NetProtocolHash())
        {
            // Say so locally too. The server also refuses, but a client that
            // only ever sees "disconnected" cannot tell a version mismatch from
            // a network fault.
            _rejectMessage = "protocol mismatch: server and client disagree on component layout";
            Core::Log::Error("NetSync: {}\n  server: {}", _rejectMessage, hello.protocolSummary);
            return;
        }

        _handshake = std::move(hello);
        if (_deferHandshake)
        {
            // Answer later. Until the local world exists there is nothing for a
            // NetId to map onto, and a snapshot applied against the wrong world
            // is silently wrong rather than loudly broken.
            _awaitingLevel = true;
            break;
        }
        ConfirmLevelReady();
        break;
    }

    case MessageType::Reject:
    {
        const std::uint32_t reason = reader.ReadBits(8);
        const std::string   detail = reader.ReadString();
        if (!reader.Ok())
            return;
        _rejectMessage = reason == static_cast<std::uint32_t>(RejectReason::ProtocolMismatch)
                             ? "server rejected the connection: protocol mismatch"
                             : "server rejected the connection: full";
        Core::Log::Error("NetSync: {}\n  server: {}", _rejectMessage, detail);
        _synchronized = false;
        break;
    }

    case MessageType::Snapshot:
        if (!ApplySnapshot(reader))
            ++_snapshotsRejected;
        break;

    default:
        break;
    }
}

bool ReplicationClient::ApplySnapshot(Core::BitReader &reader)
{
    SnapshotHeader header;
    if (!ReadSnapshotHeader(reader, header))
        return false;

    // Snapshots can overtake each other. An older one describes a world we have
    // already moved past, and applying it would visibly rewind everything.
    if (_snapshotsApplied > 0 && header.serverTick <= _lastAppliedTick)
        return true;

    const std::uint32_t despawnCount = reader.ReadVarUInt32();
    if (!reader.Ok() || despawnCount > 65536u)
    {
        reader.Invalidate();
        return false;
    }
    for (std::uint32_t i = 0; i < despawnCount; ++i)
    {
        const NetId netId = reader.ReadVarUInt32();
        if (!reader.Ok())
            return false;

        const auto it = _entityByNetId.find(netId);
        if (it != _entityByNetId.end())
        {
            _scene.Destroy(it->second);
            _entityByNetId.erase(it);
            DestroyMirrorBody(netId);
            ++_structureRevision;
        }
    }

    const Core::Reflect::ComponentRegistry &registry = Core::Reflect::ComponentRegistry::Instance();

    // Records, in field order, every entity reference this component carried and
    // whether it resolved. Paired afterwards with the component's EntityRef
    // fields so an unresolved one can be patched when its target arrives —
    // component data may legitimately name an entity whose spawn is in a later
    // block or a later snapshot.
    struct RefSite
    {
        NetId target   = InvalidNetId;
        bool  resolved = false;
    };
    std::vector<RefSite> refSites;

    Core::Reflect::CodecContext context;
    context.entityFromWire = [this, &refSites](std::uint64_t wire) -> std::uint64_t
    {
        const NetId netId = static_cast<NetId>(wire);
        if (netId == InvalidNetId)
        {
            refSites.push_back(RefSite{InvalidNetId, true}); // a genuine null reference
            return PackEntity(ECS::NullEntity);
        }
        const auto it = _entityByNetId.find(netId);
        if (it == _entityByNetId.end())
        {
            refSites.push_back(RefSite{netId, false});
            return PackEntity(ECS::NullEntity);
        }
        refSites.push_back(RefSite{netId, true});
        return PackEntity(it->second);
    };

    while (reader.Ok() && reader.ReadBool())
    {
        const NetId netId   = reader.ReadVarUInt32();
        const bool  isSpawn = reader.ReadBool();
        if (!reader.Ok() || netId == InvalidNetId)
        {
            reader.Invalidate();
            return false;
        }

        auto it = _entityByNetId.find(netId);
        if (it != _entityByNetId.end() && !_scene.IsAlive(it->second))
        {
            // A client system destroyed this mirror — gameplay runs over the
            // play world, mirrors included, and that is the decision taken
            // seriously rather than fenced off. Drop the dead mapping and let
            // the unknown-NetId path below build a fresh one, rather than
            // dereferencing a handle whose slot may since have been reused.
            _entityByNetId.erase(it);
            DestroyMirrorBody(netId);
            it = _entityByNetId.end();
            if (_mirrorsResurrected == 0)
            {
                Core::Log::Warn("NetSync: a mirrored entity was destroyed locally and has been recreated. "
                                "Client-side systems may push mirrors, but destroying one only lasts until "
                                "the server next mentions it.");
            }
            ++_mirrorsResurrected;
        }

        if (it == _entityByNetId.end())
        {
            // Treat an unknown id as a spawn even when the server called it a
            // delta. That happens after we drop a snapshot the server had
            // already counted as delivered; refusing here would strand the
            // entity permanently, while creating it costs one extra full state.
            const ECS::Entity entity = _scene.Create();
            (void)_scene.Add<Replicated>(entity, Replicated{});
            // Mirrored is what everything downstream keys off to know this
            // entity is not ours to write: the editor's read-only guard, the
            // inspector's replication-path line, and any gameplay system that
            // needs to tell "the server's crate" from "our crate".
            (void)_scene.Add<Mirrored>(entity, Mirrored{});
            it = _entityByNetId.emplace(netId, entity).first;
            ++_structureRevision;
        }
        else if (isSpawn)
        {
            // The server thinks this is new but we already have it: our despawn
            // was lost. Nothing to do — the full state that follows overwrites
            // whatever we were holding.
        }

        const ECS::Entity entity = it->second;

        // Removals come first: a component the server has dropped must go
        // before whatever else this block says about the entity, so a component
        // both removed and re-added in one snapshot ends up added.
        const std::uint32_t removedCount = reader.ReadVarUInt32();
        if (!reader.Ok() || removedCount > 4096u)
        {
            reader.Invalidate();
            return false;
        }
        for (std::uint32_t i = 0; i < removedCount; ++i)
        {
            const auto componentId = static_cast<Core::Reflect::ComponentId>(reader.ReadVarUInt32());
            if (!reader.Ok())
                return false;
            _scene.RemoveById(entity, componentId);
            ++_structureRevision;
        }

        while (reader.Ok() && reader.ReadBool())
        {
            const Core::Reflect::ComponentId componentId = Core::Reflect::ReadComponentId(reader);
            if (!reader.Ok())
                return false;

            const Core::Reflect::ComponentMeta *meta = registry.ById(componentId);
            if (meta == nullptr || !meta->serializable)
            {
                // The protocol hash is supposed to make this impossible; if it
                // happens anyway, the rest of the packet is unparseable.
                reader.Invalidate();
                return false;
            }

            refSites.clear();
            void *component = EnsureComponent(_scene, entity, *meta);
            if (component == nullptr || !Core::Reflect::ReadComponent(*meta, component, reader, nullptr, &context))
                return false;
            // Component data landed. A presentation layer has to re-resolve
            // whatever it derives from that data — MeshRenderer's GPU pointers
            // above all — and this counter is the only signal it gets.
            ++_structureRevision;

            // Pair the recorded references with the component's EntityRef
            // fields, in the same order the codec visited them.
            std::size_t site = 0;
            for (const Core::Reflect::FieldMeta &field : meta->fields)
            {
                if (field.transient || field.type != Core::Reflect::FieldType::EntityRef)
                    continue;
                if (site >= refSites.size())
                    break;
                if (!refSites[site].resolved)
                {
                    _pendingRefs.push_back(
                        PendingRef{entity, componentId, field.offset, refSites[site].target});
                }
                ++site;
            }
        }
    }

    if (!reader.Ok())
        return false;

    // Motion, for everything the server's physics world owns. After the entity
    // blocks by construction: the entity and its descriptor exist by now, so a
    // body built here starts at the authoritative state rather than re-settling
    // from the level file's pose.
    while (reader.Ok() && reader.ReadBool())
    {
        BodyState state;
        if (!ReadBodyState(reader, state))
            return false;
        ApplyBodyState(state);
    }
    if (!reader.Ok())
        return false;

    ResolvePendingRefs();

    CaptureTransforms(header.serverTick);

    _lastAppliedTick = header.serverTick;
    ++_snapshotsApplied;
    _worldComplete = header.worldComplete;
    _feedback      = ClockFeedback{header.serverTick, header.inputBufferDepth, header.starvedTicks};

    SendAck(header.serverTick);
    return true;
}

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

    const Physics::RigidBody *body = _scene.Get<Physics::RigidBody>(entity);

    // The simulation is snapped hard, with no smoothing: extrapolation has to
    // proceed from a valid physics state, and a half-applied correction is not
    // one. Hiding the jump is the *view's* job (R6), not the simulation's.
    _physics->ApplyBodyState(*body, state.position, state.rotation, state.linearVelocity, state.angularVelocity,
                             /*activate=*/!state.asleep);

    MirrorBody &record  = _bodies[state.netId];
    record.asleep       = state.asleep;
    record.restPosition = state.position;
    record.restRotation = state.rotation;
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
