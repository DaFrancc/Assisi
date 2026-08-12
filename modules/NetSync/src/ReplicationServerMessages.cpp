/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/ReplicationProviders.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>

#include <algorithm>
#include <string>
#include <utility>

#include "ReplicationInternal.hpp"

// ===========================================================================
// ReplicationServer: inbound messages, input, intents, events.
// ===========================================================================

namespace Assisi::NetSync
{
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
    hello.clientId        = connection.clientId;
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
    case MessageType::Intent:
        // Only from a client that has proved it speaks our protocol: before the
        // handshake we do not know the bytes mean what they appear to.
        if (it->second.ready)
            HandleIntent(it->second, reader);
        break;
    case MessageType::RequestKeyframe:
        // No payload to validate, and nothing a hostile client gains: the worst
        // it can do is ask for its own full state, on its own bandwidth.
        if (it->second.ready)
        {
            Core::Log::Info("NetSync: connection {} asked for a full re-anchor.", connection);
            ResetBaselines(it->second);
        }
        break;
    default:
        // A server-to-client message arriving from a client is a client that is
        // confused or lying, not a case to handle.
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

    if (hello.contentSetHash != _contentSetHash)
    {
        // A hash cannot name what differs; accepted, because the point is only
        // that after a join both machines expand every blueprint identically.
        Core::Log::Warn("NetSync: rejecting connection {} — content set mismatch (ours {:016x}, theirs "
                        "{:016x}).",
                        connection.id, _contentSetHash, hello.contentSetHash);
        SendReject(connection, RejectReason::ContentMismatch);
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
    connection.knownInstances  = std::move(record->instances);
    connection.ackedTick       = record->serverTick;
    ++connection.diagnostics.acksReceived;

    // Fold in exactly the entities this snapshot *wrote*: one skipped for byte
    // budget has no entry and keeps the baseline it had. Being named in the
    // record and having its state delivered are different facts.
    for (const WrittenEntity &entity : record->written)
    {
        EntityBaseline &baseline = connection.baselines[entity.netId];
        baseline.componentTick   = std::max(baseline.componentTick, entity.ticks.componentTick);
        baseline.bodyTick        = std::max(baseline.bodyTick, entity.ticks.bodyTick);
    }

    // A NetId that has left the acked set is gone for good — ids are never
    // reused, so no straggler ack can resurrect one. Without this the map grows
    // with every entity that has ever replicated.
    const auto retired = [&connection](const auto &entry)
    { return !std::binary_search(connection.acked.begin(), connection.acked.end(), entry.first); };
    std::erase_if(connection.baselines, retired);
    std::erase_if(connection.priority, retired);
    connection.diagnostics.baselineEntries = static_cast<std::uint32_t>(connection.baselines.size());

    // Everything at or before the acked tick is settled.
    connection.inFlight.erase(connection.inFlight.begin(), record + 1);
}

void ReplicationServer::ResetBaselines(Connection &connection)
{
    connection.baselines.clear();
    connection.inFlight.clear();
    // Priorities are deliberately kept: they describe who is owed a turn, which
    // a re-anchor does not change.
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
        // The client picks these numbers; clamp before the simulation sees them.
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

namespace
{

/// What the validation gate needs to say no, and to say which rule said it.
struct IntentGate
{
    ReplicationServer    *server      = nullptr;
    ECS::Scene           *scene       = nullptr;
    ClientId              sender;
    ConnectionDiagnostics *diagnostics = nullptr;
};

/// Steps 6 and 7 of the dispatch order, on the decoded value.
///
/// One site rather than one per handler: hand-written validation spread across
/// receive sites is the shape most documented RPC exploits come out of.
bool ValidateIntent(const Core::Reflect::MessageMeta &meta, const void *message, void *userData)
{
    IntentGate &gate = *static_cast<IntentGate *>(userData);

    // Step 6 — range. Reject, never clamp: unlike input, where a stick can
    // legitimately saturate, an out-of-range intent field means a lying client
    // or disagreeing builds, and clamping would accept the attack silently.
    std::string offending;
    if (!Core::Reflect::FieldsWithinBounds(meta.fields, message, &offending))
    {
        ++gate.diagnostics->intentsOutOfRange;
        Core::Log::Warn("NetSync: dropping '{}' from client {} — field '{}' is outside its declared range.",
                        meta.name, gate.sender.value, offending);
        return false;
    }

    // Step 7 — control. Only fields the author marked as the intent's *subject*:
    // checking every entity reference would forbid a client from ever naming an
    // entity it does not own, which is most of them.
    for (const Core::Reflect::FieldMeta &field : meta.fields)
    {
        if (!field.controlled || field.type != Core::Reflect::FieldType::EntityRef)
            continue;

        const std::uint64_t packed =
            *reinterpret_cast<const std::uint64_t *>(static_cast<const std::byte *>(message) + field.offset);
        const ECS::Entity entity = UnpackEntity(packed);
        if (entity == ECS::NullEntity)
            continue; // names nothing, so it claims nothing

        if (gate.server->ControllerOf(entity) != gate.sender)
        {
            // Counted, not treated as an attack: control transfer has a
            // propagation delay, so an honest client can send one of these.
            ++gate.diagnostics->intentsNotYours;
            return false;
        }
    }

    return true;
}

} // namespace

void ReplicationServer::HandleIntent(Connection &connection, Core::BitReader &reader)
{
    // Step 1 — envelope: bounded, and read before anything decides to care.
    const std::uint64_t             clientTick = reader.ReadVarUInt64();
    const Core::Reflect::MessageId  messageId  = Core::Reflect::ReadMessageId(reader);
    if (!reader.Ok() || messageId == Core::Reflect::kInvalidMessageId)
    {
        ++connection.diagnostics.intentsMalformed;
        return;
    }

    const Core::Reflect::MessageMeta *meta = Core::Reflect::MessageRegistry::Instance().ById(messageId);
    if (meta == nullptr)
    {
        // An id this build does not know. Nothing follows it in an intent
        // packet, so counting it is the whole response.
        ++connection.diagnostics.intentsMalformed;
        return;
    }

    // Step 2 — direction. Free, so it leads the semantic checks: clients do not
    // speak events.
    if (meta->direction != Core::Reflect::MessageDirection::Intent)
    {
        ++connection.diagnostics.intentsWrongWay;
        Core::Log::Warn("NetSync: client {} sent '{}', which is an event. Clients do not speak events.",
                        connection.clientId.value, meta->name);
        return;
    }

    // Step 3 — rate, per connection per type, *before any payload work*, so a
    // flood costs a comparison rather than a parse.
    const std::uint64_t window = _simTick / std::max<std::uint64_t>(1, _config.tickRateHz);
    if (window != connection.intentWindowTick)
    {
        connection.intentWindowTick = window;
        connection.intentsInWindow.clear();
    }
    if (++connection.intentsInWindow[messageId] > _config.maxIntentsPerTypePerSecond)
    {
        ++connection.diagnostics.intentsRateLimited;
        return;
    }

    // Step 4 — staleness. Load-bearing for unreliable intents, where
    // out-of-order arrival is normal: a late map ping must not land in a world
    // that has moved past it.
    const std::uint64_t oldest = _simTick > _config.intentStaleWindowTicks
                                     ? _simTick - _config.intentStaleWindowTicks
                                     : 0;
    if (clientTick < oldest || clientTick > _simTick + _config.intentLeadWindowTicks)
    {
        ++connection.diagnostics.intentsStale;
        return;
    }

    // Steps 5 through 8.
    DispatchIntent(connection.clientId, connection.diagnostics, *meta, reader);
}

void ReplicationServer::DispatchIntent(ClientId sender, ConnectionDiagnostics &diagnostics,
                                       const Core::Reflect::MessageMeta &meta, Core::BitReader &reader)
{
    IntentGate gate{this, &_scene, sender, &diagnostics};
    NetContext context{sender, _session, &_scene};

    // References arrive naming the sender's world — entity handles as NetIds,
    // instance ids as their block's base — and have to become local before a
    // handler, or the control check, can do anything with them.
    const Core::Reflect::CodecContext codec = DecodeContext();

    const std::uint64_t before = diagnostics.intentsOutOfRange + diagnostics.intentsNotYours;

    // Step 5 (decode), 6 and 7 (the gate), 8 (the handler) all happen inside,
    // because the gate needs the decoded value and the handler needs it after.
    if (!MessageDispatch::Instance().Dispatch(meta, context, reader, &codec, &ValidateIntent, &gate))
    {
        ++diagnostics.intentsUnhandled;
        return;
    }

    if (diagnostics.intentsOutOfRange + diagnostics.intentsNotYours == before)
        ++diagnostics.intentsAccepted;
}

std::size_t ReplicationServer::EventFloorBytes(const Connection &connection) const
{
    // Nothing waiting, nothing allowed: a game that sends no events pays no
    // bytes for the feature.
    return connection.pendingEvents.empty() ? 0 : _config.reservedEventBytes;
}

bool ReplicationServer::EventReaches(const Connection &connection, NetId subject) const
{
    if (!connection.ready)
        return false;
    if (subject == InvalidNetId)
        return true; // independent: nothing to scope it by, so everyone ready
    // The relevancy boundary reused rather than re-derived, so the zero-bytes
    // guarantee covers messages and not only state.
    return IsRelevant(connection.id, subject);
}

void ReplicationServer::QueueEvent(Connection &connection, NetId subject, std::vector<std::byte> bytes)
{
    while (connection.pendingEvents.size() >= _config.maxHeldEventsPerConnection)
    {
        // Oldest first: a queue at its cap means events about entities this
        // connection will never hold, and the newest is likeliest to still be
        // about something.
        connection.pendingEvents.pop_front();
        ++connection.diagnostics.eventsOverflowed;
    }
    connection.pendingEvents.push_back(Connection::PendingEvent{subject, std::move(bytes)});
    connection.diagnostics.eventsHeld = static_cast<std::uint32_t>(connection.pendingEvents.size());
}

void ReplicationServer::SendEvent(const void *event, std::type_index type, Recipients recipients, ClientId who)
{
    const Core::Reflect::MessageRegistry &registry = Core::Reflect::MessageRegistry::Instance();
    const Core::Reflect::MessageId        id       = registry.IdOf(type);
    const Core::Reflect::MessageMeta     *meta     = registry.ById(id);
    if (meta == nullptr)
    {
        Core::Log::Error("NetSync: refusing to send an event of an unregistered type — is it AMSG?");
        return;
    }

    // Encoded once: references translate identically for every recipient, so
    // nothing about the bytes is per-connection.
    //
    // Ids are assigned on demand, not looked up. Spawning something and
    // announcing it in the same frame is the common case, and NetIds are
    // otherwise handed out at the next tick — a lookup would encode "nothing"
    // for exactly the entity the event is about.
    const Core::Reflect::CodecContext codec = EncodeContext(IdAssignment::OnDemand);

    Core::BitWriter writer;
    if (!Core::Reflect::WriteMessage(*meta, event, writer, &codec))
        return;
    const std::span<const std::byte> encoded = writer.Data();
    std::vector<std::byte>           bytes(encoded.begin(), encoded.end());

    // What relevancy scopes this by: the first entity the message names. An
    // `independent` event names none by declaration, and reflectgen refuses an
    // event that names none without declaring it.
    NetId subject = InvalidNetId;
    if (!meta->independent)
    {
        for (const Core::Reflect::FieldMeta &field : meta->fields)
        {
            if (field.type != Core::Reflect::FieldType::EntityRef)
                continue;
            const std::uint64_t packed =
                *reinterpret_cast<const std::uint64_t *>(static_cast<const std::byte *>(event) + field.offset);
            subject = EnsureNetId(UnpackEntity(packed));
            break;
        }
    }

    const bool reliable = meta->reliability == Core::Reflect::MessageReliability::Reliable;

    const auto deliver = [&](Connection &connection)
    {
        if (reliable)
        {
            // Immediately, on the control lane, stamped with the tick the client
            // must have applied before it may act on this. Rare by design — see
            // MessageType::Announcement.
            Core::BitWriter announcement;
            WriteMessageType(MessageType::Announcement, announcement);
            announcement.WriteVarUInt64(_simTick);
            announcement.WriteVarId(subject); // wire write
            announcement.WriteBytes(bytes);
            _transport.Send(connection.id, announcement.Data(), Net::SendMode::Reliable, Net::Lane::Control);
            ++connection.diagnostics.announcementsSent;
            return;
        }
        QueueEvent(connection, subject, bytes);
    };

    const auto deliverToHost = [&]()
    {
        // No transport, so no reliability distinction: the host's delivery is a
        // queue drained at the end of its tick, which gives it the same "the
        // world is at least as new as the message" property that packet
        // ordering gives a remote client.
        _hostEvents.emplace_back(id, bytes);
    };

    switch (recipients)
    {
    case Recipients::AllRelevant:
    case Recipients::ExceptInstigator:
    {
        const bool exclude = recipients == Recipients::ExceptInstigator;
        for (auto &[connectionId, connection] : _connections)
        {
            (void)connectionId;
            if (exclude && connection.clientId == who)
                continue;
            if (EventReaches(connection, subject))
                deliver(connection);
        }
        // The authority sees everything, so the host is always in the
        // all-relevant class — but can be the excluded instigator like anyone else.
        if (!exclude || who != HostClientId)
            deliverToHost();
        break;
    }

    case Recipients::Directed:
    {
        if (!who.IsValid())
        {
            // Nobody to address. Honestly reachable — an uncontrolled entity
            // has no controller — so it is counted, not treated as an error,
            // and on the host's counters since no connection owns the failure.
            ++_hostDiagnostics.eventsUndeliverable;
            return;
        }
        if (who == HostClientId)
        {
            deliverToHost();
            return;
        }
        const auto connectionId = _connectionByClient.find(who.value);
        if (connectionId == _connectionByClient.end())
        {
            ++_hostDiagnostics.eventsUndeliverable;
            return;
        }
        const auto connection = _connections.find(connectionId->second);
        if (connection == _connections.end() || !connection->second.ready)
        {
            ++_hostDiagnostics.eventsUndeliverable;
            return;
        }
        // Addressed to a person, not to a viewpoint, so it deliberately does
        // *not* consult relevancy: "you died" must reach you whether or not
        // your corpse is in your own set.
        deliver(connection->second);
        break;
    }
    }
}

void ReplicationServer::WriteEventSection(Connection &connection, Core::BitWriter &writer,
                                          const SentSnapshot &record)
{
    // The section may run *past* the snapshot's soft cap by the configured
    // floor, rather than those bytes being held back from the entity and body
    // passes. Reserving up front does not work: those passes stop only once
    // already at the budget, and the last entity written can overshoot by more
    // than the reservation — the floor would be gone exactly when the world is
    // busiest, which is when events most need it.
    const std::size_t limit = _config.maxSnapshotBytes + EventFloorBytes(connection);

    // Bool-chained like the entity and body sections: a count would have to be
    // known before the first record is written, which means predicting the
    // budget cut instead of discovering it.
    std::size_t index = 0;
    while (index < connection.pendingEvents.size())
    {
        Connection::PendingEvent &pending = connection.pendingEvents[index];

        // The subject despawned before this connection ever heard of it, so the
        // event is now about nothing.
        if (pending.subject != InvalidNetId &&
            !std::binary_search(_liveNetIds.begin(), _liveNetIds.end(), pending.subject))
        {
            connection.pendingEvents.erase(connection.pendingEvents.begin() +
                                           static_cast<std::ptrdiff_t>(index));
            ++connection.diagnostics.eventsEvicted;
            continue;
        }

        // Acked, or written into this very packet — the same test the body-state
        // gate uses. It is what makes a message about an entity spawned in this
        // snapshot arrive *after* the spawn, with no ordering machinery.
        const bool known = pending.subject == InvalidNetId ||
                           std::binary_search(connection.acked.begin(), connection.acked.end(), pending.subject) ||
                           std::binary_search(record.netIds.begin(), record.netIds.end(), pending.subject);
        if (!known)
        {
            ++index; // held: the entity may arrive in a later snapshot
            continue;
        }

        if (writer.BytesWritten() + pending.bytes.size() > limit)
            break; // out of room even with the allowance; try next tick

        writer.WriteBool(true);
        writer.WriteBytes(pending.bytes);
        connection.pendingEvents.erase(connection.pendingEvents.begin() + static_cast<std::ptrdiff_t>(index));
        ++connection.diagnostics.eventsSent;
    }

    writer.WriteBool(false);
    connection.diagnostics.eventsHeld = static_cast<std::uint32_t>(connection.pendingEvents.size());
}

void ReplicationServer::DispatchHostEvents()
{
    if (_hostEvents.empty())
        return;

    // Swapped out first: a handler may send further events, and appending to
    // the vector being iterated would reallocate under it, or run this tick's
    // consequences inside this tick's drain.
    std::vector<std::pair<Core::Reflect::MessageId, std::vector<std::byte>>> events;
    events.swap(_hostEvents);

    // The same decode a remote event gets: the host reads its own copy back
    // through the wire's translation rather than around it, so a handler cannot
    // come to depend on ids only the host would ever see.
    const Core::Reflect::CodecContext codec = DecodeContext();

    NetContext context{HostClientId, _session, &_scene};

    for (const auto &[id, bytes] : events)
    {
        const Core::Reflect::MessageMeta *meta = Core::Reflect::MessageRegistry::Instance().ById(id);
        if (meta == nullptr)
            continue;

        Core::BitReader reader(bytes);
        (void)Core::Reflect::ReadMessageId(reader);
        if (!MessageDispatch::Instance().Dispatch(*meta, context, reader, &codec))
            ++_hostDiagnostics.intentsUnhandled;
    }
}

void ReplicationServer::DispatchLocalIntent(const void *intent, std::type_index type)
{
    const Core::Reflect::MessageRegistry &registry = Core::Reflect::MessageRegistry::Instance();
    const Core::Reflect::MessageMeta     *meta     = registry.ById(registry.IdOf(type));
    if (meta == nullptr)
        return;

    // The host has no connection, hence no ConnectionDiagnostics; _hostDiagnostics
    // stands in so its intents are counted like everyone else's.
    Core::BitWriter                   writer;
    const Core::Reflect::CodecContext codec = EncodeContext(IdAssignment::Existing);
    if (!Core::Reflect::WriteMessage(*meta, intent, writer, &codec))
        return;

    Core::BitReader reader(writer.Data());
    (void)Core::Reflect::ReadMessageId(reader); // the id we already have

    // Entering at step 5: there is no envelope to read, no transport to
    // rate-limit, and no clock skew to be stale against — the host *is* the
    // clock. Decoding onwards is identical, range and control checks included:
    // the host being trusted is not the same as the host being correct.
    DispatchIntent(HostClientId, _hostDiagnostics, *meta, reader);
}

} // namespace Assisi::NetSync
