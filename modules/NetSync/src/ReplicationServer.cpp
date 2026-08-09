/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/ReplicationConfig.hpp>
#include <Assisi/NetSync/ReplicationProviders.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/DistanceRelevancy.hpp>
#include <Assisi/NetSync/NetComponents.hpp>

#include <algorithm>
#include <string>
#include <utility>

#include "ReplicationInternal.hpp"

// ===========================================================================
// ReplicationServer: lifecycle, connections, control, ids, relevancy.
// ===========================================================================

namespace Assisi::NetSync
{
ReplicationServer::ReplicationServer(Net::NetTransport &transport, ECS::Scene &scene,
                                     Physics::PhysicsWorld *physics, ReplicationConfig config)
    : _transport(transport), _scene(scene), _physics(physics), _config(config)
{
    // Clamp the snapshot rate to a divisor of the tick rate. A rate that does
    // not divide makes the send interval alternate between two tick counts,
    // which the player sees as interpolation judder and nothing reports.
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

    // Resolve the *capable* component set once: exactly the types annotated
    // ACOMP(replicable). Opt-in is the point — "everything serializable
    // travels" put a `Camera` on the wire with every marked entity, whose
    // isActive could take over the receiving client's view.
    //
    // Capability, not policy: what a given entity actually sends is narrowed
    // later by the game's neverReplicate list and by its own exclusion mask.
    const Core::Reflect::ComponentRegistry &registry = Core::Reflect::ComponentRegistry::Instance();

    // The game's veto, resolved once. An unregistered name is a typo or a
    // renamed type, and warns rather than being ignored: an author who believes
    // something is off the wire when it is not has no way to notice.
    std::vector<Core::Reflect::ComponentId> vetoed;
    for (const std::string &name : _config.neverReplicate)
    {
        const Core::Reflect::ComponentMeta *meta = registry.Find(name);
        if (meta == nullptr)
        {
            Core::Log::Warn("NetSync: 'neverReplicate' names '{}', which no registered component matches — "
                            "ignoring it. Was the type renamed?",
                            name);
            continue;
        }
        if (!meta->replicable)
        {
            Core::Log::Info("NetSync: 'neverReplicate' names '{}', which is not ACOMP(replicable) anyway — it was "
                            "never going to be sent.",
                            name);
            continue;
        }
        vetoed.push_back(meta->id);
    }
    std::sort(vetoed.begin(), vetoed.end());

    for (const Core::Reflect::ComponentMeta *meta : registry.SerializableComponents())
    {
        if (!meta->replicable)
            continue;
        if (std::binary_search(vetoed.begin(), vetoed.end(), meta->id))
            continue; // vetoed by the game, so not even a candidate
        _replicatedComponents.push_back(meta->id);
        // Ordinals stay the *registry's*, never this list's index: exclusion
        // masks authored in level files are indexed by the registry's
        // numbering, and the game filter must not silently renumber them.
        _replicatedOrdinals.push_back(registry.ReplicableOrdinalOf(meta->id));
    }

    // Say what this session can send, once, where someone will see it — so that
    // a future module marking a new type replicable does not quietly put it on
    // every marked entity's wire unnoticed.
    {
        std::string names;
        for (const Core::Reflect::ComponentId id : _replicatedComponents)
        {
            if (const Core::Reflect::ComponentMeta *meta = registry.ById(id))
            {
                if (!names.empty())
                    names += ", ";
                names += meta->name;
            }
        }
        Core::Log::Info("NetSync: replicable components ({}): {}", _replicatedComponents.size(),
                        names.empty() ? "none" : names);
    }

    _transformComponentId = registry.IdOf(typeid(ECS::Transform));
    _transformOrdinal     = registry.ReplicableOrdinalOf(_transformComponentId);
    _descriptorOrdinal    = registry.ReplicableOrdinalOf(registry.IdOf(typeid(Physics::RigidBodyDescriptor)));

    // Whatever control claims this scene arrived carrying were made in a session
    // that is over.
    StripAuthoredControl();

    if (_config.relevancy.provider == RelevancyConfig::Provider::Distance)
    {
        _relevancy = std::make_unique<DistanceRelevancy>(_config.relevancy);
        Core::Log::Info("NetSync: relevancy is distance-based — enter {} m, exit {} m, dwell {} ticks.",
                        static_cast<double>(_config.relevancy.radius),
                        static_cast<double>(_config.relevancy.exitRadius), _config.relevancy.dwellTicks);
    }
}

void ReplicationServer::StripAuthoredControl()
{
    // Collected first, then removed. Mutating a component pool while a query
    // over it runs is the hazard Scene.hpp warns about.
    std::vector<ECS::Entity> authored;
    for (auto [entity, controlled] : _scene.Query<ControlledBy>())
    {
        (void)controlled;
        authored.push_back(entity);
    }

    for (const ECS::Entity entity : authored)
        _scene.Remove<ControlledBy>(entity);

    if (!authored.empty())
    {
        Core::Log::Info("NetSync: stripped {} authored ControlledBy component{} at session start — control is "
                        "assigned at runtime, never saved.",
                        authored.size(), authored.size() == 1 ? "" : "s");
    }
}

bool ReplicationServer::IsSnapshotTick(std::uint64_t simTick) const { return simTick % _snapshotDiv == 0; }

void ReplicationServer::AddConnection(Net::ConnectionId connection)
{
    Connection &entry = _connections[connection];
    entry.id          = connection;
    entry.ready       = false;
    // Only for a genuinely new connection. Re-registering a live one must not
    // renumber it — every ControlledBy already on the wire names the old id.
    if (!entry.clientId.IsValid())
    {
        entry.clientId = ClientId{_nextClientId++};
        _connectionByClient.emplace(entry.clientId.value, connection);
    }

    // Registered, but silent until this host knows its own content set. A
    // client answers a hello exactly once and never again, so a hello sent
    // before the server can check the answer is a join with no correct outcome.
    if (_contentSetHashReady)
        SendHello(entry);
}

void ReplicationServer::SetContentSetHash(std::uint64_t hash)
{
    _contentSetHash      = hash;
    _contentSetHashReady = true;

    // Everyone who connected while the scan was running.
    for (auto &[id, connection] : _connections)
    {
        if (!connection.ready)
            SendHello(connection);
    }
}

void ReplicationServer::RemoveConnection(Net::ConnectionId connection)
{
    const auto it = _connections.find(connection);
    if (it == _connections.end())
        return;

    const ClientId client = it->second.clientId;

    // Refreshed rather than trusted: control can be assigned between two ticks
    // and a disconnect does not wait for one. One entry per controlled entity,
    // so the rebuild costs nothing worth saving.
    RebuildControlIndex();

    if (const auto controlled = _controlledByClient.find(client.value); controlled != _controlledByClient.end())
    {
        for (const ECS::Entity entity : controlled->second)
        {
            if (!_scene.IsAlive(entity))
                continue;
            const ControlledBy *claim = _scene.Get<ControlledBy>(entity);
            if (claim == nullptr || claim->client != client.value)
                continue; // control moved on since the index was built

            // Both outcomes already have a wire path — despawn rides the
            // acked-set diff, removal rides the presence diff — so nothing new
            // has to be sent for either.
            if (claim->despawnOnDisconnect)
                _scene.Destroy(entity);
            else
                _scene.Remove<ControlledBy>(entity);
        }
        _controlledByClient.erase(controlled);
    }

    // Otherwise a provider's per-client state (dwell counters, last-known
    // distances) is held for the life of the session against an id that will
    // never be handed out again.
    if (_relevancy != nullptr)
        _relevancy->ForgetClient(client);

    _connectionByClient.erase(client.value);
    _connections.erase(it);
}

ClientId ReplicationServer::ClientIdOf(Net::ConnectionId connection) const
{
    const auto it = _connections.find(connection);
    return it == _connections.end() ? InvalidClientId : it->second.clientId;
}

Net::ConnectionId ReplicationServer::ConnectionOf(ClientId client) const
{
    const auto it = _connectionByClient.find(client.value);
    return it == _connectionByClient.end() ? Net::InvalidConnection : it->second;
}

void ReplicationServer::SetControl(ECS::Entity entity, ClientId client, bool despawnOnDisconnect)
{
    if (!_scene.IsAlive(entity))
        return;

    if (!client.IsValid())
    {
        ClearControl(entity);
        return;
    }

    // **Through GetMut**, so the write stamps a change tick. An unstamped
    // transfer is one the delta path never sends: correct on the server, stale
    // on every client until the next keyframe sweep.
    if (ControlledBy *claim = _scene.GetMut<ControlledBy>(entity))
    {
        if (claim->client != client.value)
        {
            if (const auto it = _controlledByClient.find(claim->client); it != _controlledByClient.end())
                std::erase(it->second, entity);
        }
        claim->client              = client.value;
        claim->despawnOnDisconnect = despawnOnDisconnect;
    }
    else
    {
        (void)_scene.Add<ControlledBy>(entity, ControlledBy{client.value, despawnOnDisconnect});
    }

    std::vector<ECS::Entity> &controlled = _controlledByClient[client.value];
    if (std::find(controlled.begin(), controlled.end(), entity) == controlled.end())
        controlled.push_back(entity);
}

void ReplicationServer::ClearControl(ECS::Entity entity)
{
    if (!_scene.IsAlive(entity))
        return;
    if (const ControlledBy *claim = _scene.Get<ControlledBy>(entity))
    {
        if (const auto it = _controlledByClient.find(claim->client); it != _controlledByClient.end())
            std::erase(it->second, entity);
        _scene.Remove<ControlledBy>(entity);
    }
}

NetId ReplicationServer::EnsureNetId(ECS::Entity entity)
{
    if (entity == ECS::NullEntity || !_scene.IsAlive(entity))
        return InvalidNetId;

    if (const NetId existing = NetIdOf(entity); existing != InvalidNetId)
        return existing;

    // Only for entities that actually replicate. An id for something the
    // snapshot never mentions makes an event reference resolve to nothing on
    // arrival, which is worse than resolving to nothing here.
    if (!_scene.Has<Replicated>(entity))
        return InvalidNetId;

    // A blueprint member takes its id from its instance's block, so every member
    // is derivable from one base. Falls through to the counter when there is no
    // instance to describe.
    NetId netId = EnsureInstanceBlock(entity);
    if (netId == InvalidNetId)
        netId = NetId{_nextNetId++};

    _netIdByEntity.emplace(PackEntity(entity), netId);
    _entityByNetId.emplace(netId, entity);

    // Sorted insert, never an append: a block is reserved whole, so member 0 can
    // be assigned after member 2 and take a lower id than one already listed.
    _liveNetIds.insert(std::lower_bound(_liveNetIds.begin(), _liveNetIds.end(), netId), netId);
    return netId;
}

NetId ReplicationServer::EnsureInstanceBlock(ECS::Entity entity)
{
    if (_instanceInfo == nullptr)
        return InvalidNetId;

    const ECS::BlueprintMember *tag = _scene.Get<ECS::BlueprintMember>(entity);
    if (tag == nullptr || !tag->instanceId.IsValid())
        return InvalidNetId;

    auto it = _instanceBlocks.find(tag->instanceId);
    if (it == _instanceBlocks.end())
    {
        InstanceInfo info;
        if (!_instanceInfo->Describe(tag->instanceId, info) || info.memberCount == 0)
            return InvalidNetId; // not describable: its members replicate individually

        InstanceBlock block;
        block.base        = NetId{_nextNetId};
        block.memberCount = info.memberCount;
        block.info        = info;
        // All of them until a reconcile pass finds otherwise; ReconcileNetIds is
        // what actually seeds this, in the same pass that gives every existing
        // member its id.
        block.derivable.assign(info.memberCount, 1u);

        // The whole range at once. Reserving lazily per member would let an
        // ordinary entity land in the middle of the block and break the
        // contiguity the record depends on.
        _nextNetId += info.memberCount;
        it = _instanceBlocks.emplace(tag->instanceId, block).first;
        _blockRanges.emplace_back(block.base, block.memberCount); // sorted by construction
    }

    // A member index outside the block would alias whatever was allocated next,
    // so refuse and let it replicate as an ordinary entity. The tag disagreeing
    // with the definition is a bug; a colliding NetId is a corrupt mirror.
    if (tag->memberIndex >= it->second.memberCount)
    {
        Core::Log::Error("Replication: instance {} member index {} is outside its block of {} — "
                         "replicating it as a loose entity",
                         tag->instanceId, tag->memberIndex, it->second.memberCount);
        return InvalidNetId;
    }

    return NetId{it->second.base.value + tag->memberIndex};
}

void ReplicationServer::SetInstanceInfoProvider(std::unique_ptr<InstanceInfoProvider> provider)
{
    _instanceInfo = std::move(provider);
}

ClientId ReplicationServer::ControllerOf(ECS::Entity entity) const
{
    if (const ControlledBy *claim = _scene.Get<ControlledBy>(entity))
        return ClientId{claim->client};
    return InvalidClientId;
}

std::span<const ECS::Entity> ReplicationServer::ControlledEntities(ClientId client) const
{
    const auto it = _controlledByClient.find(client.value);
    return it == _controlledByClient.end() ? std::span<const ECS::Entity>{} : std::span<const ECS::Entity>{it->second};
}

void ReplicationServer::SetRelevancyProvider(std::unique_ptr<RelevancyProvider> provider)
{
    _relevancy = std::move(provider);

    // Every remembered set describes a world the old provider believed in.
    // Clearing them makes the next snapshot treat whatever the new provider
    // names as a re-entry and resend full state — over-sending once, which is
    // the safe direction when the policy has just changed under everyone.
    for (auto &[id, connection] : _connections)
    {
        (void)id;
        connection.relevant.clear();
    }
}

void ReplicationServer::SetViewAnchors(Net::ConnectionId connection, std::span<const ECS::Entity> anchors)
{
    const auto it = _connections.find(connection);
    if (it == _connections.end())
        return;
    it->second.anchors.assign(anchors.begin(), anchors.end());
}

std::span<const ECS::Entity> ReplicationServer::ViewAnchors(Net::ConnectionId connection) const
{
    const auto it = _connections.find(connection);
    if (it == _connections.end())
        return {};
    if (!it->second.anchors.empty())
        return std::span<const ECS::Entity>{it->second.anchors};

    const auto controlled = _controlledByClient.find(it->second.clientId.value);
    return controlled == _controlledByClient.end() ? std::span<const ECS::Entity>{}
                                                   : std::span<const ECS::Entity>{controlled->second};
}

void ReplicationServer::GrantRelevance(Net::ConnectionId connection, NetId netId)
{
    const auto it = _connections.find(connection);
    if (it == _connections.end() || netId == InvalidNetId)
        return;

    std::vector<NetId> &grants = it->second.grants;
    const auto          slot   = std::lower_bound(grants.begin(), grants.end(), netId);
    if (slot == grants.end() || *slot != netId)
        grants.insert(slot, netId);
}

void ReplicationServer::RevokeRelevance(Net::ConnectionId connection, NetId netId)
{
    const auto it = _connections.find(connection);
    if (it == _connections.end())
        return;

    std::vector<NetId> &grants = it->second.grants;
    const auto          slot   = std::lower_bound(grants.begin(), grants.end(), netId);
    if (slot != grants.end() && *slot == netId)
        grants.erase(slot);
}

bool ReplicationServer::IsRelevant(Net::ConnectionId connection, NetId netId) const
{
    const auto it = _connections.find(connection);
    if (it == _connections.end())
        return false;
    if (_relevancy == nullptr)
        return std::binary_search(_liveNetIds.begin(), _liveNetIds.end(), netId);
    return std::binary_search(it->second.relevant.begin(), it->second.relevant.end(), netId);
}

std::span<const NetId> ReplicationServer::RelevantSet(Net::ConnectionId connection) const
{
    const auto it = _connections.find(connection);
    if (it == _connections.end())
        return {};
    return _relevancy == nullptr ? std::span<const NetId>{_liveNetIds} : std::span<const NetId>{it->second.relevant};
}

const std::vector<NetId> &ReplicationServer::ComputeEffective(Connection &connection)
{
    // The identity case, deliberately not "a provider that returns everything":
    // no call, no copy, no intersection. A test pins that the wire bytes match
    // an identity-filter run.
    if (_relevancy == nullptr)
        return _liveNetIds;

    // The anchors this connection views from: whatever the session set, or — as
    // a default only — the entities it controls. A provider handed an empty list
    // is being told this connection has no viewpoint; what to do about that is
    // its decision.
    connection.anchorScratch.clear();
    if (!connection.anchors.empty())
    {
        for (const ECS::Entity anchor : connection.anchors)
        {
            if (_scene.IsAlive(anchor))
                connection.anchorScratch.push_back(anchor);
        }
    }
    else if (const auto controlled = _controlledByClient.find(connection.clientId.value);
             controlled != _controlledByClient.end())
    {
        connection.anchorScratch.assign(controlled->second.begin(), controlled->second.end());
    }

    _providerScratch.clear();
    const RelevancyQuery query{
        connection.clientId,
        std::span<const ECS::Entity>{connection.anchorScratch},
        std::span<const NetId>{_liveNetIds},
        &_scene,
        this,
        _simTick,
    };
    _relevancy->Compute(query, _providerScratch);

    // Everything the provider named, plus what policy adds to it regardless of
    // what the provider thinks.
    std::vector<NetId> &merged = connection.mergeScratch;
    merged.assign(_providerScratch.begin(), _providerScratch.end());
    merged.insert(merged.end(), connection.grants.begin(), connection.grants.end());

    // The implicit grant: a connection is always told about the entities it
    // controls. Without it a player's own pawn can drift out of its own radius
    // when the view anchor is set elsewhere, and prediction — which needs the
    // controller to always hold its subject — becomes impossible.
    if (const auto controlled = _controlledByClient.find(connection.clientId.value);
        controlled != _controlledByClient.end())
    {
        for (const ECS::Entity entity : controlled->second)
        {
            const NetId netId = NetIdOf(entity);
            if (netId != InvalidNetId)
                merged.push_back(netId);
        }
    }

    // Relevance::Always — the escape from whatever the provider decided. Every
    // connection, unconditionally: a radius is a bandwidth tool, and an
    // objective marker that vanishes at 60 metres is not worth the saving.
    merged.insert(merged.end(), _alwaysRelevant.begin(), _alwaysRelevant.end());

    std::sort(merged.begin(), merged.end());
    merged.erase(std::unique(merged.begin(), merged.end()), merged.end());

    // Relevance::ControllerOnly — the escape in the other direction. **Applied
    // last**, so it outranks the provider, a grant, and Always alike: "only this
    // player may know about it" is not a preference to be outvoted.
    if (!_controllerOnly.empty())
    {
        std::erase_if(merged,
                      [this, &connection](NetId netId)
                      {
                          const auto entry = std::lower_bound(
                              _controllerOnly.begin(), _controllerOnly.end(), netId,
                              [](const auto &pair, NetId value) { return pair.first < value; });
                          if (entry == _controllerOnly.end() || entry->first != netId)
                              return false; // not one of them
                          // Uncontrolled (client 0) means nobody may see it.
                          return entry->second != connection.clientId.value;
                      });
    }

    // A provider may name whatever it likes; only live entities exist.
    std::vector<NetId> &effective = connection.effectiveScratch;
    effective.clear();
    std::set_intersection(merged.begin(), merged.end(), _liveNetIds.begin(), _liveNetIds.end(),
                          std::back_inserter(effective));

    // Instances are relevant whole or not at all. A provider naming one wheel
    // pulls the car: the client derives every member from one record, so a
    // partial instance leaves it holding member ids it cannot attribute, and the
    // record's memberCount disagrees with what actually arrived.
    //
    // Runs on what survived the policy filters, and re-adds **every** member of
    // any block that kept one — including members the filters just removed. A
    // ControllerOnly member is escalated back in whenever a sibling is relevant;
    // no filter is re-applied afterwards.
    if (!_blockRanges.empty())
    {
        _escalateScratch.clear();
        for (const NetId netId : effective)
        {
            // The last block whose base is at or below this id.
            auto range = std::upper_bound(_blockRanges.begin(), _blockRanges.end(), netId,
                                          [](NetId value, const auto &entry) { return value < entry.first; });
            if (range == _blockRanges.begin())
                continue;
            --range;
            if (netId.value >= range->first.value + range->second)
                continue; // past the end of that block: an ordinary entity

            for (std::uint32_t member = 0; member < range->second; ++member)
                _escalateScratch.push_back(NetId{range->first.value + member});
        }

        if (!_escalateScratch.empty())
        {
            std::sort(_escalateScratch.begin(), _escalateScratch.end());
            _escalateScratch.erase(std::unique(_escalateScratch.begin(), _escalateScratch.end()),
                                   _escalateScratch.end());

            merged.clear();
            std::set_union(effective.begin(), effective.end(), _escalateScratch.begin(),
                           _escalateScratch.end(), std::back_inserter(merged));

            // Back through the live set: a member destroyed on its own is not
            // resurrected by its siblings being relevant.
            effective.clear();
            std::set_intersection(merged.begin(), merged.end(), _liveNetIds.begin(), _liveNetIds.end(),
                                  std::back_inserter(effective));
        }
    }

    // Re-entry inside one round trip. An entity in the set now, absent last
    // snapshot, and still in the acked set has an unacked despawn in flight: the
    // ordinary path would send a delta, and the client — which destroyed its
    // mirror when that despawn landed — would rebuild the entity out of whatever
    // components that delta carried. Forgetting it sends full state instead.
    //
    // Reachable through the grant API, through teleports, and through plain
    // oscillation at a provider's boundary.
    for (const NetId netId : effective)
    {
        if (std::binary_search(connection.relevant.begin(), connection.relevant.end(), netId))
            continue; // never left
        if (!std::binary_search(connection.acked.begin(), connection.acked.end(), netId))
            continue; // already forgotten — the empty-baseline path has it covered
        ForgetAcked(connection, netId);

        // Forgetting the member is not enough: the client destroyed the whole
        // instance when the despawn landed, so the record has to go out again
        // too. Without this the member arrives as full state with nothing to
        // attach it to and the expander is never asked to rebuild it.
        if (const ECS::BlueprintMember *tag = _scene.Get<ECS::BlueprintMember>(EntityOf(netId));
            tag != nullptr && tag->instanceId.IsValid())
        {
            ForgetAckedInstance(connection, tag->instanceId);
        }
    }

    // Count enters and exits: boundary thrash otherwise looks exactly like
    // ordinary bandwidth, and it is what hysteresis exists to prevent — enters
    // climbing in lockstep with exits is the shape to watch for. One merge over
    // two sorted sequences counts both directions with no output to discard.
    {
        std::size_t before = 0;
        std::size_t now    = 0;
        while (before < connection.relevant.size() && now < effective.size())
        {
            if (connection.relevant[before] < effective[now])
            {
                ++connection.diagnostics.relevancyExits;
                ++before;
            }
            else if (effective[now] < connection.relevant[before])
            {
                ++connection.diagnostics.relevancyEnters;
                ++now;
            }
            else
            {
                ++before;
                ++now;
            }
        }
        connection.diagnostics.relevancyExits += connection.relevant.size() - before;
        connection.diagnostics.relevancyEnters += effective.size() - now;
    }
    connection.diagnostics.relevantEntities = static_cast<std::uint32_t>(effective.size());

    connection.relevant = effective;
    return effective;
}

void ReplicationServer::ForgetAcked(Connection &connection, NetId netId)
{
    if (const auto slot = std::lower_bound(connection.acked.begin(), connection.acked.end(), netId);
        slot != connection.acked.end() && *slot == netId)
    {
        connection.acked.erase(slot);
    }

    // This netId's packed-ref range. ComponentId{0} is the low bound rather
    // than "invalid" — ids are dense from 0 — and netId + 1 is the exclusive
    // upper bound, spelled out because NetId has no arithmetic of its own.
    const auto low  = std::lower_bound(connection.ackedComponents.begin(), connection.ackedComponents.end(),
                                       PackComponentRef(netId, Core::Reflect::ComponentId{0}));
    const auto high = std::lower_bound(connection.ackedComponents.begin(), connection.ackedComponents.end(),
                                       PackComponentRef(NetId{netId.value + 1}, Core::Reflect::ComponentId{0}));
    connection.ackedComponents.erase(low, high);

    connection.baselines.erase(netId);
    connection.diagnostics.baselineEntries = static_cast<std::uint32_t>(connection.baselines.size());

    // ...and the ring, because HandleAck installs a record's entity set
    // wholesale. A late ack for a pre-revoke snapshot would otherwise put the
    // entity straight back into the acked set with its old baseline, and the
    // next snapshot would send the delta this call exists to prevent.
    for (SentSnapshot &record : connection.inFlight)
    {
        if (const auto slot = std::lower_bound(record.netIds.begin(), record.netIds.end(), netId);
            slot != record.netIds.end() && *slot == netId)
        {
            record.netIds.erase(slot);
        }

        std::erase_if(record.written, [netId](const WrittenEntity &entry) { return entry.netId == netId; });

        // The same packed-ref range as above.
        const auto recordLow  = std::lower_bound(record.components.begin(), record.components.end(),
                                                 PackComponentRef(netId, Core::Reflect::ComponentId{0}));
        const auto recordHigh = std::lower_bound(record.components.begin(), record.components.end(),
                                                 PackComponentRef(NetId{netId.value + 1}, Core::Reflect::ComponentId{0}));
        record.components.erase(recordLow, recordHigh);
    }
}

void ReplicationServer::ForgetAckedInstance(Connection &connection, ECS::InstanceId instanceId)
{
    if (const auto known = std::lower_bound(connection.knownInstances.begin(),
                                            connection.knownInstances.end(), instanceId);
        known != connection.knownInstances.end() && *known == instanceId)
    {
        connection.knownInstances.erase(known);
    }

    // ...and the ring, for the same reason ForgetAcked scrubs it: HandleAck
    // installs `instances` wholesale, so a straggler ack for a snapshot sent
    // before the instance left would put it straight back and the resend this
    // call exists to cause would never happen.
    for (SentSnapshot &record : connection.inFlight)
    {
        if (const auto slot = std::lower_bound(record.instances.begin(), record.instances.end(), instanceId);
            slot != record.instances.end() && *slot == instanceId)
        {
            record.instances.erase(slot);
        }
    }
}

void ReplicationServer::RebuildControlIndex()
{
    for (auto &[client, entities] : _controlledByClient)
    {
        (void)client;
        entities.clear(); // keep the buckets; only the contents are per-tick truth
    }

    for (auto [entity, controlled] : _scene.Query<ControlledBy>())
    {
        if (controlled.client == InvalidClientId.value)
            continue; // claims nobody, so it indexes under nobody
        _controlledByClient[controlled.client].push_back(entity);
    }
}

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

} // namespace Assisi::NetSync
