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

    // Resolve the *capable* component set once: exactly the types annotated
    // ACOMP(replicable). Opt-in, and the opt-in is the point — "everything
    // serializable travels" shipped a `Camera` with every marked entity, whose
    // isActive could take over the receiving client's view, and would have put
    // every future gameplay-local component on the wire by default. The
    // Replicated marker is not in the set for a different reason: it says only
    // *that* an entity replicates, which the client learns from the spawn.
    //
    // Capability, not policy: this is what *may* travel, not what does. Which of
    // these a given entity actually sends is narrowed later — by the game's
    // neverReplicate list and by each entity's own exclusion mask.
    const Core::Reflect::ComponentRegistry &registry = Core::Reflect::ComponentRegistry::Instance();

    // The game's veto, resolved once. A name nobody registered is a typo or a
    // renamed type, and silently ignoring it would leave the author believing
    // something is off the wire when it is not — the exact class of quiet
    // wrongness this design exists to remove.
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
            continue; // the game says never, so it is not even a candidate
        _replicatedComponents.push_back(meta->id);
        // Ordinals stay the *registry's*, not this list's index: the exclusion
        // masks authored in level files are indexed by the registry's numbering,
        // and a game filter must not silently renumber them.
        _replicatedOrdinals.push_back(registry.ReplicableOrdinalOf(meta->id));
    }

    // Say what this session can send, once, where someone will see it. The
    // residual risk of default-send policy is that a future engine module marks
    // a new type replicable and every marked entity quietly starts carrying it;
    // a capability surface you are shown is the cheap fence against that.
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

    // Session start. Whatever claims this scene arrived carrying, they were made
    // in a session that is over.
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
    // Collected first, then removed: mutating a component pool while a query
    // over it is running is the documented hazard in Scene.hpp, and this is
    // exactly the shape it warns about.
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
    // Only if this is genuinely a new connection. Re-registering a live one must
    // not renumber it — every ControlledBy already on the wire names the old id.
    if (!entry.clientId.IsValid())
    {
        entry.clientId = ClientId{_nextClientId++};
        _connectionByClient.emplace(entry.clientId.value, connection);
    }

    // Registered, but silent until this host knows its own content set. A client
    // that gets a hello answers it exactly once and never again, so a hello sent
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

    // Refreshed rather than trusted: control can be assigned between two ticks,
    // and a disconnect is not obliged to wait for one. The index is one entry
    // per controlled entity, so this costs nothing worth saving.
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

            // Both outcomes already have a wire path: a despawn rides NetId
            // retirement through the acked-set diff, and a component removal
            // rides the presence diff. Nothing new travels for either.
            if (claim->despawnOnDisconnect)
                _scene.Destroy(entity);
            else
                _scene.Remove<ControlledBy>(entity);
        }
        _controlledByClient.erase(controlled);
    }

    // A provider keeping per-pair state (dwell counters, last-known distances)
    // would otherwise hold it for the life of the session against an id that
    // will never be handed out again.
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

    // Through GetMut so the write stamps a change tick. A transfer that does not
    // stamp is a transfer the delta path never sends — the component would sit
    // correct on the server and stale on every client until a keyframe sweep.
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

    // Only for entities that actually replicate — handing an id to something the
    // snapshot will never mention would make an event reference resolve to
    // nothing on arrival, which is worse than resolving to nothing here.
    if (!_scene.Has<Replicated>(entity))
        return InvalidNetId;

    // A blueprint member takes its id from its instance's block, so that every
    // member is derivable from one base and one record can stand in for all of
    // them. Falls through to the counter when there is no instance to describe.
    NetId netId = EnsureInstanceBlock(entity);
    if (netId == InvalidNetId)
        netId = NetId{_nextNetId++}; // turns a raw counter into an id — see _nextNetId

    _netIdByEntity.emplace(PackEntity(entity), netId);
    _entityByNetId.emplace(netId, entity);

    // Sorted insert rather than an append. Ids no longer only climb: a block is
    // reserved whole, so member 0 can be assigned after member 2 and take a
    // lower id than one already in the list.
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
        // All of them, until a reconcile pass finds otherwise. Every member that
        // exists is given its id in the same pass this block is allocated in, so
        // the first pass over the live set is what actually seeds this — see
        // ReconcileNetIds.
        block.derivable.assign(info.memberCount, 1u);

        // The whole range at once. Reserving lazily per member would let an
        // ordinary entity land in the middle of the block and break the one
        // property the record depends on.
        _nextNetId += info.memberCount;
        it = _instanceBlocks.emplace(tag->instanceId, block).first;
        _blockRanges.emplace_back(block.base, block.memberCount); // sorted by construction
    }

    // A member index outside the block would alias whatever was allocated next.
    // Refuse and let it replicate as an ordinary entity: the tag disagreeing
    // with the definition is a bug, but a colliding NetId is a corrupt mirror.
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

    // Every connection's remembered set describes a world the old provider
    // believed in. Clearing it makes the next snapshot treat whatever the new
    // provider names as a re-entry, which resends full state — over-sending
    // once, which is the correct direction to be wrong in when the policy
    // deciding who sees what has just been replaced under everyone.
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
    // The identity case, and it is deliberately not "a provider that returns
    // everything": no call, no copy, no intersection — the same vector the
    // pre-relevancy code walked. A test pins that the wire bytes are identical
    // to an identity-filter run, which is what makes this claim checkable
    // rather than asserted.
    if (_relevancy == nullptr)
        return _liveNetIds;

    // The anchors this connection actually views from: whatever the session set,
    // or — only as a default — the entities it controls. A provider handed an
    // empty list is being told this connection has no viewpoint, and what to do
    // about that is its decision, not the engine's.
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
    // controls. Every surveyed system pins this, and the failure without it is
    // absurd on its face — a player's own pawn drifting out of its own radius
    // because the view anchor was set somewhere else. It is also the precondition
    // for any future prediction, which needs the controller to always hold its
    // subject.
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
    // connection, unconditionally, because a radius is a bandwidth tool and an
    // objective marker that vanishes at 60 metres is a bug the saving does not
    // pay for.
    merged.insert(merged.end(), _alwaysRelevant.begin(), _alwaysRelevant.end());

    std::sort(merged.begin(), merged.end());
    merged.erase(std::unique(merged.begin(), merged.end()), merged.end());

    // Relevance::ControllerOnly — the escape in the other direction, and the one
    // class that reads ControlledBy, which is its whole job description. Applied
    // last so it outranks the provider, a grant, and Always alike: "only this
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
                          // Uncontrolled means nobody, which is the honest
                          // reading of "only the controller may see it".
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
    // partial instance would leave it holding member ids it cannot attribute —
    // and the record's memberCount would disagree with what actually arrived.
    //
    // After the policy filters deliberately. ControllerOnly removing a member
    // must not be undone by escalation, which is why this reads the filtered set
    // and adds to it rather than running before them... except that escalation
    // then re-adds siblings a filter dropped. So it runs on what survived, and
    // the filters are re-applied to nothing: a ControllerOnly member's siblings
    // are pulled in, but the member itself stays out. That is the honest reading
    // of "only this player may know about it" — the car is visible, that one
    // part of it is not.
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

    // Re-entry inside one round trip. An entity that is in the set now, was not
    // last snapshot, and is still in the acked set has an unacked despawn in
    // flight: the ordinary path would send it a delta, and the client — which
    // destroyed its mirror when that despawn landed — would build a fresh entity
    // out of whichever components the delta happened to carry. Forgetting it
    // sends full state instead.
    //
    // Reachable through the grant API, through teleports, and through plain
    // oscillation at a provider's boundary, so it is not a corner worth leaving
    // to chance.
    for (const NetId netId : effective)
    {
        if (std::binary_search(connection.relevant.begin(), connection.relevant.end(), netId))
            continue; // never left
        if (!std::binary_search(connection.acked.begin(), connection.acked.end(), netId))
            continue; // already forgotten — the empty-baseline path has it covered
        ForgetAcked(connection, netId);

        // Instance-granular, because forgetting a member is not enough. The
        // client destroyed the whole instance when the despawn landed — the run
        // covered the block and took the record with it — so re-entry has to
        // resend the record too. Without this the member arrives as full state
        // with nothing to attach it to, and the expander is never asked to
        // rebuild what the client threw away.
        if (const ECS::BlueprintMember *tag = _scene.Get<ECS::BlueprintMember>(EntityOf(netId));
            tag != nullptr && tag->instanceId.IsValid())
        {
            ForgetAckedInstance(connection, tag->instanceId);
        }
    }

    // Boundary thrash is invisible otherwise — it looks exactly like ordinary
    // bandwidth — and it is the one failure mode hysteresis exists to prevent.
    // Enters climbing in lockstep with exits is the shape to watch for.
    // One merge over two sorted sequences, counting both directions at once —
    // cheaper than two set_differences and it needs no output to discard.
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

    // ComponentId{0}: the packed key's lower bound, not "invalid" — 0 is the
    // lowest possible ordinal, and component ids are dense from there.
    const auto low  = std::lower_bound(connection.ackedComponents.begin(), connection.ackedComponents.end(),
                                       PackComponentRef(netId, Core::Reflect::ComponentId{0}));
    // netId + 1: the exclusive upper bound of this netId's packed-ref range,
    // spelled explicitly since NetId has no arithmetic of its own.
    const auto high = std::lower_bound(connection.ackedComponents.begin(), connection.ackedComponents.end(),
                                       PackComponentRef(NetId{netId.value + 1}, Core::Reflect::ComponentId{0}));
    connection.ackedComponents.erase(low, high);

    connection.baselines.erase(netId);
    connection.diagnostics.baselineEntries = static_cast<std::uint32_t>(connection.baselines.size());

    // ...and the ring, because HandleAck installs a record's entity set
    // wholesale. A late ack for a snapshot sent before the revoke would
    // otherwise put the entity straight back into the acked set, with its old
    // baseline, and the next snapshot would send the delta this call exists to
    // prevent.
    for (SentSnapshot &record : connection.inFlight)
    {
        if (const auto slot = std::lower_bound(record.netIds.begin(), record.netIds.end(), netId);
            slot != record.netIds.end() && *slot == netId)
        {
            record.netIds.erase(slot);
        }

        std::erase_if(record.written, [netId](const WrittenEntity &entry) { return entry.netId == netId; });

        // ComponentId{0}: the packed key's lower bound, not "invalid" — 0 is the
        // lowest possible ordinal, and component ids are dense from there.
        const auto recordLow  = std::lower_bound(record.components.begin(), record.components.end(),
                                                 PackComponentRef(netId, Core::Reflect::ComponentId{0}));
        // netId + 1: the exclusive upper bound of this netId's packed-ref range,
        // spelled explicitly since NetId has no arithmetic of its own.
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

    // ...and the ring, for the same reason ForgetAcked scrubs it: `instances` is
    // installed wholesale by HandleAck, so a straggler ack for a snapshot sent
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
