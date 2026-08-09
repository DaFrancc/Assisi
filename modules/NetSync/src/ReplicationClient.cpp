/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/InstanceRecord.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/NetSync/NetComponents.hpp>

#include <algorithm>
#include <string>
#include <utility>

#include "ReplicationInternal.hpp"

// ===========================================================================
// ReplicationClient: lifecycle, messages, applying a snapshot.
// ===========================================================================

namespace Assisi::NetSync
{
namespace
{

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

ReplicationClient::ReplicationClient(Net::NetTransport &transport, ECS::Scene &scene, Net::ConnectionId connection,
                                     Physics::PhysicsWorld *physics)
    : _transport(transport), _scene(scene), _physics(physics), _connection(connection)
{
    const Core::Reflect::ComponentRegistry &registry = Core::Reflect::ComponentRegistry::Instance();
    _descriptorComponentId                          = registry.IdOf(typeid(Physics::RigidBodyDescriptor));
    _rigidBodyComponentId                           = registry.IdOf(typeid(Physics::RigidBody));
}

void ReplicationClient::SendHello()
{
    Core::BitWriter writer;
    WriteMessageType(MessageType::ClientHello, writer);

    ClientHello hello;
    hello.protocolHash   = NetProtocolHash();
    hello.contentSetHash = _contentSetHash;
    WriteClientHello(hello, writer);

    _transport.Send(_connection, writer.Data(), Net::SendMode::Reliable, Net::Lane::Control);
}

void ReplicationClient::SetContentSetHash(std::uint64_t hash)
{
    _contentSetHash      = hash;
    _contentSetHashReady = true;

    // The hash was the last thing missing: a client whose level finished loading
    // while the scan was still running completes its join here.
    if (_levelReady && !_synchronized)
        ConfirmLevelReady();
}

void ReplicationClient::ConfirmLevelReady(std::uint64_t contentSetHash)
{
    _contentSetHash      = contentSetHash;
    _contentSetHashReady = true;
    ConfirmLevelReady();
}

void ReplicationClient::ConfirmLevelReady()
{
    if (_synchronized)
        return;

    _levelReady    = true;
    _awaitingLevel = false;

    // Both, or nothing. A hello with a placeholder hash is a refused join no retry
    // can fix, because the hello is sent exactly once.
    if (!_contentSetHashReady)
        return;

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

bool ReplicationClient::SendIntentBytes(const void *intent, std::type_index type, std::uint64_t clientTick,
                                        bool reliable)
{
    if (!_synchronized)
        return false;

    const Core::Reflect::MessageRegistry &registry = Core::Reflect::MessageRegistry::Instance();
    const Core::Reflect::MessageMeta     *meta     = registry.ById(registry.IdOf(type));
    if (meta == nullptr)
    {
        // Unreachable through SendIntent, whose static_assert needs a
        // MessageTraits specialization that only a registered message has. Kept
        // because the type-erased entry point is reachable from elsewhere, and
        // silently sending nothing is the worst possible answer.
        Core::Log::Error("NetSync: refusing to send an intent of an unregistered type — is it AMSG?");
        return false;
    }

    Core::BitWriter writer;
    WriteMessageType(MessageType::Intent, writer);
    writer.WriteVarUInt64(clientTick);

    // Local entity handles mean nothing on the server; references travel as
    // NetIds, exactly as they do inside a component block.
    Core::Reflect::CodecContext codec;
    codec.entityToWire = [this](std::uint64_t packed) -> std::uint64_t
    // .value: the codec's entity-ref slot is a bare uint64_t — the wire boundary.
    { return NetIdOf(UnpackEntity(packed)).value; };

    if (!Core::Reflect::WriteMessage(*meta, intent, writer, &codec))
        return false;

    // Reliability is the type's, never the call site's. Both forms ride
    // Lane::Control: an intent is a statement about the world's future, and it
    // must not be reordered behind a snapshot the way the input window is.
    _transport.Send(_connection, writer.Data(),
                    reliable ? Net::SendMode::Reliable : Net::SendMode::Unreliable, Net::Lane::Control);
    return true;
}

void ReplicationClient::DispatchEvent(const Core::Reflect::MessageMeta &meta, Core::BitReader &reader)
{
    Core::Reflect::CodecContext codec;
    codec.entityFromWire = [this](std::uint64_t wire) -> std::uint64_t
    {
        // Wire boundary: the codec's entity-ref slot is a bare uint64_t.
        const ECS::Entity mirror = EntityOf(NetId{static_cast<std::uint32_t>(wire)});
        return (static_cast<std::uint64_t>(mirror.index)) | (static_cast<std::uint64_t>(mirror.generation) << 32);
    };

    NetContext context{InvalidClientId, _session, &_scene};
    if (!MessageDispatch::Instance().Dispatch(meta, context, reader, &codec))
    {
        // Normal: the server's build may care about something this one does
        // not. Counted so a handler somebody meant to write is a number rather
        // than a silence.
        ++_eventsUnhandled;
        (void)Core::Reflect::SkipMessageBody(reader);
        return;
    }
    ++_eventsDispatched;
}

bool ReplicationClient::ApplyEventSection(Core::BitReader &reader)
{
    while (reader.Ok() && reader.ReadBool())
    {
        const Core::Reflect::MessageId id = Core::Reflect::ReadMessageId(reader);
        if (!reader.Ok() || id == Core::Reflect::kInvalidMessageId)
            return false;

        const Core::Reflect::MessageMeta *meta = Core::Reflect::MessageRegistry::Instance().ById(id);
        if (meta == nullptr)
        {
            // An id this build does not know. The length prefix is exactly what
            // makes stepping over it possible instead of losing the rest of the
            // packet — and the handshake means it should never happen between a
            // matched pair.
            if (!Core::Reflect::SkipMessageBody(reader))
                return false;
            ++_eventsUnhandled;
            continue;
        }

        DispatchEvent(*meta, reader);
    }
    return reader.Ok();
}

void ReplicationClient::HandleAnnouncement(Core::BitReader &reader)
{
    DeferredAnnouncement pending;
    pending.serverTick = reader.ReadVarUInt64();
    pending.subject    = NetId{reader.ReadVarUInt32()}; // wire read
    pending.messageId  = Core::Reflect::ReadMessageId(reader);
    if (!reader.Ok() || pending.messageId == Core::Reflect::kInvalidMessageId)
        return;

    // The body is copied whole — id prefix and all — so the deferred path and
    // the immediate one decode identically rather than through two readers with
    // two chances to disagree.
    const std::size_t bodyStart = (reader.BitsRead() - 0) / 8;
    (void)bodyStart;

    // Re-encode the id in front of the body so DispatchEvent sees the same shape
    // it sees in the snapshot section.
    Core::BitWriter body;
    body.WriteVarUInt32(pending.messageId.value); // wire write
    const std::uint32_t bodyBits = reader.ReadVarUInt32();
    body.WriteVarUInt32(bodyBits);
    std::size_t remaining = bodyBits;
    while (remaining > 0 && reader.Ok())
    {
        const std::uint32_t chunk = static_cast<std::uint32_t>(std::min<std::size_t>(remaining, 64));
        body.WriteBits64(reader.ReadBits64(chunk), chunk);
        remaining -= chunk;
    }
    if (!reader.Ok())
        return;

    const std::span<const std::byte> encoded = body.Data();
    pending.bytes.assign(encoded.begin(), encoded.end());
    _deferredAnnouncements.push_back(std::move(pending));

    // Try immediately: an announcement about a world we have already caught up
    // to has nothing to wait for.
    DrainAnnouncements();
}

void ReplicationClient::DrainAnnouncements()
{
    for (auto it = _deferredAnnouncements.begin(); it != _deferredAnnouncements.end();)
    {
        // Two conditions, and both are about the same thing: does the world this
        // message describes exist here yet. The tick stamp covers state the
        // message implies; the entity check covers the one it names.
        const bool tickReached = _lastAppliedTick >= it->serverTick;
        const bool subjectHere = it->subject == InvalidNetId || EntityOf(it->subject) != ECS::NullEntity;
        if (!tickReached || !subjectHere)
        {
            ++it;
            continue;
        }

        const Core::Reflect::MessageMeta *meta = Core::Reflect::MessageRegistry::Instance().ById(it->messageId);
        if (meta != nullptr)
        {
            Core::BitReader reader(it->bytes);
            (void)Core::Reflect::ReadMessageId(reader);
            DispatchEvent(*meta, reader);
        }
        it = _deferredAnnouncements.erase(it);
    }
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

    case MessageType::Announcement:
        if (_synchronized)
            HandleAnnouncement(reader);
        break;

    case MessageType::Reject:
    {
        const std::uint32_t reason = reader.ReadBits(8);
        const std::string   detail = reader.ReadString();
        if (!reader.Ok())
            return;
        switch (static_cast<RejectReason>(reason))
        {
        case RejectReason::ProtocolMismatch:
            _rejectMessage = "server rejected the connection: protocol mismatch";
            break;
        case RejectReason::ContentMismatch:
            _rejectMessage = "server rejected the connection: content sets differ — remove stray .alvl/.abp "
                             "files or sync assets";
            break;
        case RejectReason::ServerFull:
        default:
            _rejectMessage = "server rejected the connection: full";
            break;
        }
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

    // Instance records, first in the packet and read before anything else can
    // reference them. Recording only — turning a record into expanded members is
    // 7c. Until then a member block still arrives as an ordinary entity, so
    // reading this wrong is a desync of the whole stream rather than a missing
    // car, which is why it is bounded like every other count here.
    const std::uint32_t recordCount = reader.ReadBool() ? reader.ReadVarUInt32() : 0u;
    if (!reader.Ok() || recordCount > 65536u)
    {
        reader.Invalidate();
        return false;
    }
    for (std::uint32_t i = 0; i < recordCount; ++i)
    {
        InstanceRecord entry;
        entry.blueprintIndex = reader.ReadVarUInt32();
        entry.base           = NetId{reader.ReadVarUInt32()}; // wire read
        entry.memberCount    = reader.ReadVarUInt32();

        // Bounded before it is used as a loop count, not merely as a sanity
        // check: the presence bits below are `memberCount` reads, so an
        // unbounded count read off the wire is a loop the packet dictates the
        // length of. No blueprint has 65536 members; one claiming to has
        // already failed the content-set hash.
        if (!reader.Ok() || entry.memberCount == 0 || entry.memberCount > 65536u)
        {
            reader.Invalidate();
            return false;
        }

        // Which members exist over there. All of them, unless the host says
        // otherwise — see InstanceRecord::memberPresent.
        if (!reader.ReadBool())
        {
            entry.memberPresent.resize(entry.memberCount);
            for (std::uint32_t member = 0; member < entry.memberCount; ++member)
                entry.memberPresent[member] = reader.ReadBool() ? 1u : 0u;
        }

        entry.placement.position.x = reader.ReadFloat();
        entry.placement.position.y = reader.ReadFloat();
        entry.placement.position.z = reader.ReadFloat();
        entry.placement.rotation.w = reader.ReadFloat();
        entry.placement.rotation.x = reader.ReadFloat();
        entry.placement.rotation.y = reader.ReadFloat();
        entry.placement.rotation.z = reader.ReadFloat();
        entry.placement.scale.x    = reader.ReadFloat();
        entry.placement.scale.y    = reader.ReadFloat();
        entry.placement.scale.z    = reader.ReadFloat();

        if (!reader.Ok() || !entry.base.IsValid())
        {
            reader.Invalidate();
            return false;
        }

        // Idempotent: a record is resent until acked, so the same instance
        // arrives repeatedly and only the first one expands.
        const auto [slot, inserted] = _instanceRecords.insert_or_assign(entry.base, entry);
        (void)slot;
        if (!inserted || _instanceExpander == nullptr)
            continue;

        std::vector<ECS::Entity> members;
        ECS::InstanceId          localInstance;
        if (!_instanceExpander->Expand(entry, members, localInstance) || members.size() != entry.memberCount)
        {
            // Fatal, not survivable. Binding a short or failed expansion would
            // attach member ids to the wrong entities, and every delta after
            // this one would land on the wrong member — a mirror that is wrong
            // rather than incomplete.
            Core::Log::Error("NetSync: could not expand instance blueprint {} ({} members expected, {} "
                             "produced) — refusing the snapshot",
                             entry.blueprintIndex, entry.memberCount, members.size());
            _instanceRecords.erase(entry.base);
            reader.Invalidate();
            return false;
        }

        for (std::uint32_t member = 0; member < entry.memberCount; ++member)
        {
            if (!entry.HasMember(member))
            {
                // The blueprint built it here and the host has no such entity —
                // pruned since it was placed, or removed from that instance by
                // the level. Expanding the whole definition and then dropping
                // the holes is deliberate: the alternative is an expander that
                // takes a member filter, and every caller of it would have to
                // keep the file's member order anyway, which is the one thing
                // `base + i` cannot survive being wrong about.
                if (members[member] != ECS::NullEntity && _scene.IsAlive(members[member]))
                    _scene.Destroy(members[member]);
                continue;
            }
            // A hole the *client's* expansion left where the host has a member:
            // the two disagree about the file, which the content-set hash was
            // supposed to make impossible. Leaving it unbound costs one member;
            // binding NullEntity would cost every delta that names it.
            if (members[member] == ECS::NullEntity)
                continue;

            // The binding the whole scheme rests on: member i *is* base + i, so
            // the server never sends a member list.
            _entityByNetId.insert_or_assign(NetId{entry.base.value + member}, members[member]);
        }
        if (localInstance.IsValid())
            _instanceIdByBase.insert_or_assign(entry.base, localInstance);
        ++_structureRevision;
    }

    // Runs, not ids — see the encoder. The count bounds the number of runs; each
    // run's length is bounded separately below.
    const std::uint32_t despawnCount = reader.ReadVarUInt32();
    if (!reader.Ok() || despawnCount > 65536u)
    {
        reader.Invalidate();
        return false;
    }
    // Collected before any of it is acted on, because which records to throw away
    // is a question about *all* the runs together and not about each one in turn
    // — see below.
    std::vector<std::pair<NetId, std::uint32_t>> despawnRuns;
    despawnRuns.reserve(despawnCount);
    for (std::uint32_t i = 0; i < despawnCount; ++i)
    {
        const NetId         start  = NetId{reader.ReadVarUInt32()}; // wire read
        const std::uint32_t length = reader.ReadVarUInt32();
        // Bounded like every other count on this path: the run is attacker-
        // controlled, and an unchecked length here is a loop the packet dictates
        // the size of.
        if (!reader.Ok() || length == 0 || length > 65536u)
        {
            reader.Invalidate();
            return false;
        }
        despawnRuns.emplace_back(start, length);
    }

    for (const auto &[start, length] : despawnRuns)
    {
        for (std::uint32_t offset = 0; offset < length; ++offset)
        {
            const NetId netId{start.value + offset};
            const auto  it = _entityByNetId.find(netId);
            if (it == _entityByNetId.end())
                continue; // a run may name ids this client never had

            _scene.Destroy(it->second);
            _entityByNetId.erase(it);
            DestroyMirrorBody(netId);
            ++_structureRevision;
        }
    }

    // A record dies with its last member, and that is the *same* predicate the
    // server resends on — an instance leaves `knownInstances` exactly when no
    // member of it is left to be relevant. The two used to disagree (B7): the
    // client erased only when a run started exactly at a record's base and was
    // exactly its width, so two adjacent instances leaving together — one run of
    // six over two blocks of three — kept both records while destroying all six
    // entities. The resend then found the records already present, skipped the
    // expansion, and the members came back as bare mirrors attributed to
    // nothing, permanently, because the authored-value elision suppresses
    // everything still equal to the file on an empty baseline.
    //
    // Only records a run actually touched are considered: one whose members have
    // not arrived yet — held back by the byte budget, or waiting on an expander
    // — has no bindings either, and must not be mistaken for one that has lost
    // them all.
    if (!despawnRuns.empty())
    {
        std::erase_if(_instanceRecords,
                      [this, &despawnRuns](const std::pair<const NetId, InstanceRecord> &row)
                      {
                          const NetId base = row.second.base;
                          // 64-bit, so a block or a run running off the top of
                          // the id space compares as the range it is rather than
                          // wrapping into a low one.
                          const std::uint64_t end =
                              static_cast<std::uint64_t>(base.value) + row.second.memberCount;

                          const bool touched =
                              std::any_of(despawnRuns.begin(), despawnRuns.end(),
                                          [base, end](const auto &run)
                                          {
                                              return run.first.value < end &&
                                                     static_cast<std::uint64_t>(run.first.value) + run.second >
                                                         base.value;
                                          });
                          if (!touched)
                              return false;

                          for (std::uint32_t member = 0; member < row.second.memberCount; ++member)
                          {
                              if (_entityByNetId.contains(NetId{base.value + member}))
                                  return false;
                          }

                          // The local instance goes with it: `instanceFromWire`
                          // would otherwise keep resolving this base to an
                          // instance whose members are all destroyed.
                          _instanceIdByBase.erase(base);
                          return true;
                      });
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
        // Wire boundary: the codec's entity-ref slot is a bare uint64_t.
        const NetId netId = NetId{static_cast<std::uint32_t>(wire)};
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

    // The other half of the tag's translation: what arrives is the instance's
    // base NetId, and what this machine stores is its own instance id. Zero when
    // the instance was never expanded here, which leaves the tag invalid rather
    // than pointing at an unrelated local instance.
    context.instanceFromWire = [this](std::uint32_t base) -> std::uint32_t
    {
        const auto local = _instanceIdByBase.find(NetId{base});
        return local == _instanceIdByBase.end() ? 0u : local->second.value;
    };

    while (reader.Ok() && reader.ReadBool())
    {
        const NetId netId   = NetId{reader.ReadVarUInt32()}; // wire read
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
            const Core::Reflect::ComponentId componentId{reader.ReadVarUInt32()}; // wire read
            if (!reader.Ok())
                return false;

            // Losing the descriptor is not an ordinary component removal: this
            // mirror stops being body-corrected and becomes an interpolated
            // visual, so its Jolt body has to go with it. Without this the body
            // outlives its authority and keeps colliding — an invisible obstacle
            // in the middle of the world, which is a worse bug than the one that
            // produced it. The stale transient RigidBody would also block any
            // future rebuild, since SyncMirrorBody keys off its presence.
            if (_physics != nullptr && componentId == _descriptorComponentId &&
                _scene.Get<Physics::RigidBody>(entity) != nullptr)
            {
                DestroyMirrorBody(netId);
                _scene.RemoveById(entity, _rigidBodyComponentId);
            }

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

        // Everything this entity was told about has landed, so its physics body
        // can be brought in line with it.
        SyncMirrorBody(netId, entity);
    }

    if (!reader.Ok())
        return false;

    // Motion, for everything the server's physics world owns. After the entity
    // blocks by construction: the entity and its descriptor exist by now, so a
    // body built here starts at the authoritative state rather than re-settling
    // from the level file's pose.
    const std::size_t bodySectionStart = reader.BitsRead();
    while (reader.Ok() && reader.ReadBool())
    {
        BodyState state;
        if (!ReadBodyState(reader, state))
            return false;
        ApplyBodyState(state);
    }
    if (!reader.Ok())
        return false;
    _corrections.bytesApplied += (reader.BitsRead() - bodySectionStart + 7u) / 8u;

    ResolvePendingRefs();

    CaptureTransforms(header.serverTick);

    _lastAppliedTick = header.serverTick;
    ++_snapshotsApplied;
    _worldComplete = header.worldComplete;
    _feedback      = ClockFeedback{header.serverTick, header.inputBufferDepth, header.starvedTicks};

    // After the state, never before. A handler for an event about an entity
    // spawned in this same packet must find that entity already there, and the
    // wire order plus this call site are the entire mechanism — no per-message
    // sequencing, no waiting.
    if (!ApplyEventSection(reader))
        return false;

    // ...and the tick just moved, so an announcement that was waiting for it may
    // now have its world.
    DrainAnnouncements();

    SendAck(header.serverTick);
    return true;
}

} // namespace Assisi::NetSync
