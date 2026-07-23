/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/Replication.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/NetSync/NetComponents.hpp>

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

ReplicationServer::ReplicationServer(Net::NetTransport &transport, ECS::Scene &scene, ReplicationConfig config)
    : _transport(transport), _scene(scene), _config(config)
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

    // Resolve the replicated component set once. Everything serializable
    // replicates except the marker itself, which only says *that* an entity
    // replicates and would be pure overhead on the wire — the client learns it
    // from the spawn.
    const Core::Reflect::ComponentId markerId =
        Core::Reflect::ComponentRegistry::Instance().IdOf(typeid(Replicated));
    for (const Core::Reflect::ComponentMeta *meta : Core::Reflect::ComponentRegistry::Instance().SerializableComponents())
    {
        if (meta->id != markerId)
            _replicatedComponents.push_back(meta->id);
    }
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
    hello.protocolHash    = Core::Reflect::ProtocolHash();
    hello.protocolSummary = Core::Reflect::ProtocolSummary();
    hello.tickRateHz      = _config.tickRateHz;
    hello.snapshotHz      = _config.snapshotHz;
    hello.serverTick      = _simTick;
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
    writer.WriteString(Core::Reflect::ProtocolSummary());
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

    if (hello.protocolHash != Core::Reflect::ProtocolHash())
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
    connection.ackedTick       = record->serverTick;
    connection.ackedChangeTick = record->sceneChangeTick;
    ++connection.diagnostics.acksReceived;

    // Everything at or before the acked tick is settled.
    connection.inFlight.erase(connection.inFlight.begin(), record + 1);
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

std::uint32_t ReplicationServer::WriteEntityComponents(ECS::Entity entity, std::uint64_t sinceChangeTick,
                                                       Core::BitWriter &writer)
{
    Core::Reflect::CodecContext context;
    context.entityToWire = [this](std::uint64_t packed) -> std::uint64_t
    {
        // Entity references cross the wire as NetIds. A reference to something
        // that does not replicate resolves to zero rather than to a local handle
        // the peer would misread as one of its own.
        const NetId netId = NetIdOf(UnpackEntity(packed));
        return static_cast<std::uint64_t>(netId);
    };

    const Core::Reflect::ComponentRegistry &registry = Core::Reflect::ComponentRegistry::Instance();

    std::uint32_t written = 0;
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

        writer.WriteBool(true);
        Core::Reflect::WriteComponent(*meta, component, writer, Core::Reflect::kAllFields, &context);
        ++written;
    }
    writer.WriteBool(false);
    return written;
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
    record.serverTick      = _simTick;
    record.sceneChangeTick = _scene.CurrentChangeTick();
    record.netIds.reserve(_liveNetIds.size());

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
                record.netIds.push_back(netId);
            continue;
        }

        writer.WriteBool(true);
        writer.WriteVarUInt32(netId);
        writer.WriteBool(!known); // isSpawn

        WriteEntityComponents(entity, known ? connection.ackedChangeTick : 0, writer);
        record.netIds.push_back(netId);
    }
    writer.WriteBool(false);

    // record.netIds is built in _liveNetIds order, which is sorted — keep that
    // invariant explicit, since the ack path binary-searches it.
    std::sort(record.netIds.begin(), record.netIds.end());

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

    if (!IsSnapshotTick(simTick))
        return;

    for (auto &[id, connection] : _connections)
    {
        if (connection.ready)
            SendSnapshot(connection);
    }
}

// ===========================================================================
// ReplicationClient
// ===========================================================================

ReplicationClient::ReplicationClient(Net::NetTransport &transport, ECS::Scene &scene, Net::ConnectionId connection)
    : _transport(transport), _scene(scene), _connection(connection)
{
}

void ReplicationClient::SendHello()
{
    Core::BitWriter writer;
    WriteMessageType(MessageType::ClientHello, writer);

    ClientHello hello;
    hello.protocolHash = Core::Reflect::ProtocolHash();
    WriteClientHello(hello, writer);

    _transport.Send(_connection, writer.Data(), Net::SendMode::Reliable, Net::Lane::Control);
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
        if (hello.protocolHash != Core::Reflect::ProtocolHash())
        {
            // Say so locally too. The server also refuses, but a client that
            // only ever sees "disconnected" cannot tell a version mismatch from
            // a network fault.
            _rejectMessage = "protocol mismatch: server and client disagree on component layout";
            Core::Log::Error("NetSync: {}\n  server: {}", _rejectMessage, hello.protocolSummary);
            return;
        }
        SendHello();
        _synchronized = true;
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
        if (it == _entityByNetId.end())
        {
            // Treat an unknown id as a spawn even when the server called it a
            // delta. That happens after we drop a snapshot the server had
            // already counted as delivered; refusing here would strand the
            // entity permanently, while creating it costs one extra full state.
            const ECS::Entity entity = _scene.Create();
            (void)_scene.Add<Replicated>(entity, Replicated{});
            it = _entityByNetId.emplace(netId, entity).first;
        }
        else if (isSpawn)
        {
            // The server thinks this is new but we already have it: our despawn
            // was lost. Nothing to do — the full state that follows overwrites
            // whatever we were holding.
        }

        const ECS::Entity entity = it->second;

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

    ResolvePendingRefs();

    _lastAppliedTick = header.serverTick;
    ++_snapshotsApplied;
    _feedback = ClockFeedback{header.serverTick, header.inputBufferDepth, header.starvedTicks};

    SendAck(header.serverTick);
    return true;
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
    _entityByNetId.clear();
    _pendingRefs.clear();
    _feedback          = ClockFeedback{};
    _lastAppliedTick   = 0;
    _snapshotsApplied  = 0;
    _snapshotsRejected = 0;
    _synchronized      = false;
    _rejectMessage.clear();
}

} // namespace Assisi::NetSync
