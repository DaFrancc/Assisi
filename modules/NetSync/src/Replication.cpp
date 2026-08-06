/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/Replication.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/DistanceRelevancy.hpp>
#include <Assisi/NetSync/NetComponents.hpp>

#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <expected>
#include <string>
#include <utility>

namespace Assisi::NetSync
{
namespace
{

/// Entity handles are (index, generation); the maps here want one integer key.
///
/// **The layout matches `BinaryCodec`'s exactly — index low, generation high.**
/// That is not tidiness: the codec hands `entityToWire`/`entityFromWire` a
/// handle it packed itself, so a second convention here silently swaps the two
/// halves of every entity reference that crosses the wire. It read correctly
/// only for handles whose index happens to equal their generation — which
/// includes `{0, 0}`, the first entity in any scene, and is exactly why nothing
/// noticed until a message referenced a second one.
std::uint64_t PackEntity(ECS::Entity entity)
{
    return static_cast<std::uint64_t>(entity.index) | (static_cast<std::uint64_t>(entity.generation) << 32);
}

/// `(netId, componentId)` as one sortable integer. The component-set diff that
/// finds removals is a set_difference over these, so they must order by entity
/// first and component second — which the shift gives for free.
std::uint64_t PackComponentRef(NetId netId, Core::Reflect::ComponentId componentId)
{
    // .value on both: packing into a sortable integer, not a NetId/ComponentId
    // operation.
    return (static_cast<std::uint64_t>(netId.value) << 32) | static_cast<std::uint64_t>(componentId.value);
}

/// The component half of a packed ref. There is deliberately no NetId half: the
/// only consumer is the removal diff, which already knows the entity it is
/// diffing and needs the component out of each pair.
Core::Reflect::ComponentId ComponentIdOfRef(std::uint64_t packed)
{
    // packed key boundary: unpacking a sortable integer back into an id.
    return Core::Reflect::ComponentId{static_cast<std::uint32_t>(packed & 0xFFFFFFFFull)};
}

ECS::Entity UnpackEntity(std::uint64_t packed)
{
    return ECS::Entity{static_cast<std::uint32_t>(packed & 0xFFFFFFFFull), static_cast<std::uint32_t>(packed >> 32)};
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

std::vector<std::string> LoadNeverReplicateFromConfig(std::string_view configPath)
{
    const std::expected<std::string, Core::AssetError> text = Core::AssetSystem::ReadText(configPath);
    if (!text)
        return {}; // no config is not a problem; replicating everything capable is a complete answer

    try
    {
        const nlohmann::json json = nlohmann::json::parse(*text);
        if (!json.contains("networking"))
            return {};

        const nlohmann::json &block = json.at("networking");
        if (!block.contains("neverReplicate"))
            return {};

        const nlohmann::json &list = block.at("neverReplicate");
        if (!list.is_array())
        {
            Core::Log::Warn("NetSync: 'networking.neverReplicate' in '{}' must be an array of component names — "
                            "ignoring it.",
                            configPath);
            return {};
        }

        std::vector<std::string> names;
        for (const nlohmann::json &element : list)
        {
            if (element.is_string())
                names.push_back(element.get<std::string>());
            else
                Core::Log::Warn("NetSync: 'networking.neverReplicate' entries must be component names — skipping "
                                "a '{}'.",
                                element.type_name());
        }
        return names;
    }
    catch (const std::exception &error)
    {
        // A malformed config must not be able to silently *widen* what a game
        // sends, so say so rather than falling through quietly.
        Core::Log::Warn("NetSync: cannot read 'networking.neverReplicate' from '{}' ({}) — replicating every "
                        "capable component.",
                        configPath, error.what());
        return {};
    }
}

RelevancyConfig LoadRelevancyFromConfig(std::string_view configPath)
{
    const std::expected<std::string, Core::AssetError> text = Core::AssetSystem::ReadText(configPath);
    if (!text)
        return {}; // no config is not a problem; telling everyone everything is a complete answer

    try
    {
        const nlohmann::json json = nlohmann::json::parse(*text);
        if (!json.contains("networking"))
            return {};

        const nlohmann::json &block = json.at("networking");
        if (!block.contains("relevancy"))
            return {};

        const nlohmann::json &relevancy = block.at("relevancy");
        if (!relevancy.is_object())
        {
            Core::Log::Warn("NetSync: 'networking.relevancy' in '{}' must be an object — ignoring it.", configPath);
            return {};
        }

        RelevancyConfig config;
        if (relevancy.contains("provider"))
        {
            const std::string name = relevancy.at("provider").get<std::string>();
            if (name == "all")
            {
                config.provider = RelevancyConfig::Provider::All;
            }
            else if (name == "distance")
            {
                config.provider = RelevancyConfig::Provider::Distance;
            }
            else
            {
                // A name nobody implements is a typo or a renamed provider, and
                // quietly falling back to "everything" would leave the author
                // believing a radius is in force when it is not.
                Core::Log::Warn("NetSync: 'networking.relevancy.provider' is '{}', which is not a provider this "
                                "build knows ('all' or 'distance') — telling every connection about everything.",
                                name);
            }
        }
        if (relevancy.contains("radius"))
            config.radius = relevancy.at("radius").get<float>();
        if (relevancy.contains("exitRadius"))
            config.exitRadius = relevancy.at("exitRadius").get<float>();
        if (relevancy.contains("dwellTicks"))
            config.dwellTicks = relevancy.at("dwellTicks").get<std::uint32_t>();
        return config;
    }
    catch (const std::exception &error)
    {
        // Same direction as every other loader here: a malformed config must not
        // silently *narrow* what a game sends.
        Core::Log::Warn("NetSync: cannot read 'networking.relevancy' from '{}' ({}) — telling every connection "
                        "about everything.",
                        configPath, error.what());
        return {};
    }
}

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

    // The one place that turns a raw counter into an id — see _nextNetId.
    const NetId netId{_nextNetId++};
    _netIdByEntity.emplace(PackEntity(entity), netId);
    _entityByNetId.emplace(netId, entity);
    // Ids only ever climb, so the live set stays sorted by appending.
    _liveNetIds.push_back(netId);
    return netId;
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
        // Only from a client that has proved it speaks our protocol. Before the
        // handshake completes we do not know that the bytes mean what they
        // appear to, and dispatching them would be acting on a guess.
        if (it->second.ready)
            HandleIntent(it->second, reader);
        break;
    case MessageType::RequestKeyframe:
        // No payload to validate, and nothing a hostile client gains: the worst
        // it can do is ask for its own full state repeatedly, which costs it
        // bandwidth it is already receiving.
        if (it->second.ready)
        {
            Core::Log::Info("NetSync: connection {} asked for a full re-anchor.", connection);
            ResetBaselines(it->second);
        }
        break;
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

    if (hello.contentSetHash != _contentSetHash)
    {
        // With only a hash the server cannot name what differs, which the design
        // accepts: the check exists so that after a successful join both machines
        // are known to expand any blueprint identically, and a diagnosable-but-
        // weaker check would not buy that.
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
/// Both live here rather than in each handler on purpose: hand-written
/// validation spread across receive sites is the exact shape every documented
/// exploit in the RPC survey came out of, and the single site is the feature.
bool ValidateIntent(const Core::Reflect::MessageMeta &meta, const void *message, void *userData)
{
    IntentGate &gate = *static_cast<IntentGate *>(userData);

    // Step 6 — range. Reject, never clamp: the input path clamps because a
    // stick can legitimately saturate, while an out-of-range intent field means
    // the client is lying or the builds disagree, and clamping would convert a
    // detectable attack into a silently accepted one.
    std::string offending;
    if (!Core::Reflect::FieldsWithinBounds(meta.fields, message, &offending))
    {
        ++gate.diagnostics->intentsOutOfRange;
        Core::Log::Warn("NetSync: dropping '{}' from client {} — field '{}' is outside its declared range.",
                        meta.name, gate.sender.value, offending);
        return false;
    }

    // Step 7 — control. Only fields the author marked as the intent's *subject*;
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
            continue; // named nothing, which is a claim about nothing

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
    // Step 1 — envelope. Fixed-size, bounded, and read before anything decides
    // whether to care.
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
        // An id this build does not know. The length prefix means we could step
        // over it, but there is nothing after it in an intent packet, so the
        // count is the whole response.
        ++connection.diagnostics.intentsMalformed;
        return;
    }

    // Step 2 — direction. Free, and first among the semantic checks: the
    // vocabulary itself says a client does not speak events.
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
    // out-of-order arrival is normal and a late map ping must not time-travel
    // into a world that has moved past it.
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

    // Entity references arrive as NetIds and have to become local handles before
    // a handler — or the control check — can do anything with them.
    Core::Reflect::CodecContext codec;
    codec.entityFromWire = [this](std::uint64_t wire) -> std::uint64_t
    // The codec's entity-ref slot is a bare uint64_t; NetId{...} here is the
    // wire boundary where that number becomes a NetId.
    { return PackEntity(EntityOf(NetId{static_cast<std::uint32_t>(wire)})); };

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
    // Nothing waiting, nothing allowed. A game that sends no events must not pay
    // for the feature — the same performance-first rule that makes relevancy
    // cost nothing without a provider.
    return connection.pendingEvents.empty() ? 0 : _config.reservedEventBytes;
}

bool ReplicationServer::EventReaches(const Connection &connection, NetId subject) const
{
    if (!connection.ready)
        return false;
    if (subject == InvalidNetId)
        return true; // independent: nothing to scope it by, so everyone ready
    // The relevancy boundary, reused rather than re-derived. This is what makes
    // the zero-bytes guarantee cover messages and not only state.
    return IsRelevant(connection.id, subject);
}

void ReplicationServer::QueueEvent(Connection &connection, NetId subject, std::vector<std::byte> bytes)
{
    while (connection.pendingEvents.size() >= _config.maxHeldEventsPerConnection)
    {
        // Oldest first. A queue at its cap is a connection being told about
        // entities it will never hold, and the newest event is the one most
        // likely to still be about something.
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

    // Encoded once. Entity references translate to NetIds identically for every
    // recipient, so there is nothing per-connection about the bytes.
    //
    // Assigned on demand rather than looked up: spawning something and
    // announcing it in the same frame is the common case, and NetIds are
    // otherwise handed out at the next tick — so a lookup would silently encode
    // "nothing" for exactly the entity the event is about.
    Core::Reflect::CodecContext codec;
    codec.entityToWire = [this](std::uint64_t packed) -> std::uint64_t
    // .value: the codec's entity-ref slot is a bare uint64_t — the wire boundary.
    { return EnsureNetId(UnpackEntity(packed)).value; };

    Core::BitWriter writer;
    if (!Core::Reflect::WriteMessage(*meta, event, writer, &codec))
        return;
    const std::span<const std::byte> encoded = writer.Data();
    std::vector<std::byte>           bytes(encoded.begin(), encoded.end());

    // What relevancy scopes this by: the first entity the message names. An
    // `independent` event names none by declaration, and reflectgen refuses an
    // event that names none without saying so.
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
            announcement.WriteVarUInt32(subject.value); // wire write
            announcement.WriteBytes(bytes);
            _transport.Send(connection.id, announcement.Data(), Net::SendMode::Reliable, Net::Lane::Control);
            ++connection.diagnostics.announcementsSent;
            return;
        }
        QueueEvent(connection, subject, bytes);
    };

    const auto deliverToHost = [&]()
    {
        // No transport, no reliability distinction: the host's own delivery is a
        // queue drained at the end of its tick, which gives it the same
        // "the world is at least as new as the message" property packet ordering
        // gives a remote client.
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
        // The authority sees everything, so the host is in the all-relevant
        // class by definition — and can be the excluded instigator like anyone
        // else.
        if (!exclude || who != HostClientId)
            deliverToHost();
        break;
    }

    case Recipients::Directed:
    {
        if (!who.IsValid())
        {
            // Nobody to address. Reachable honestly — an uncontrolled entity has
            // no controller — so it is counted rather than treated as an error,
            // on the host's counters since no connection owns the failure.
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
        // A directed event is addressed to a person, not to a viewpoint, so it
        // deliberately does *not* consult relevancy: "you died" must reach you
        // whether or not your corpse is in your own set.
        deliver(connection->second);
        break;
    }
    }
}

void ReplicationServer::WriteEventSection(Connection &connection, Core::BitWriter &writer,
                                          const SentSnapshot &record)
{
    // The section is allowed to run *past* the snapshot's soft cap, by the
    // configured floor, rather than having those bytes held back from the entity
    // and body passes.
    //
    // Reserving up front does not work: those passes stop when they are already
    // at the budget, and the last entity written can overshoot it by more than
    // the reservation — so the "floor" would be gone exactly when the world is
    // busiest, which is when events most need it. Overrunning a soft cap by a
    // bounded amount, only when something is waiting, actually guarantees it.
    const std::size_t limit = _config.maxSnapshotBytes + EventFloorBytes(connection);

    // Bool-chained like the entity and body sections, for the same reason: a
    // count would have to be known before the first record is written, which
    // means predicting the budget cut instead of discovering it.
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
        // gate uses, and what makes a message about an entity spawned in this
        // snapshot arrive *after* the spawn without any ordering machinery.
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

    // Swapped out first: a handler may send further events, and appending to the
    // vector being iterated would either reallocate under it or run this tick's
    // consequences inside this tick's drain.
    std::vector<std::pair<Core::Reflect::MessageId, std::vector<std::byte>>> events;
    events.swap(_hostEvents);

    Core::Reflect::CodecContext codec;
    codec.entityFromWire = [this](std::uint64_t wire) -> std::uint64_t
    // The codec's entity-ref slot is a bare uint64_t; NetId{...} here is the
    // wire boundary where that number becomes a NetId.
    { return PackEntity(EntityOf(NetId{static_cast<std::uint32_t>(wire)})); };

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

    // The host has no connection, so it has no ConnectionDiagnostics either.
    // One kept here means its intents are counted like everyone else's rather
    // than being invisible.
    Core::BitWriter writer;
    Core::Reflect::CodecContext codec;
    codec.entityToWire = [this](std::uint64_t packed) -> std::uint64_t
    // .value: the codec's entity-ref slot is a bare uint64_t — the wire boundary.
    { return NetIdOf(UnpackEntity(packed)).value; };
    if (!Core::Reflect::WriteMessage(*meta, intent, writer, &codec))
        return;

    Core::BitReader reader(writer.Data());
    (void)Core::Reflect::ReadMessageId(reader); // the id we already have

    // Entering at step 5 rather than step 1: there is no envelope to read, no
    // transport to rate-limit, and no clock skew to be stale against — the host
    // *is* the clock. Everything from decoding onwards is identical, including
    // the range and control checks, because the host being trusted is not the
    // same as the host being correct.
    DispatchIntent(HostClientId, _hostDiagnostics, *meta, reader);
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
            // The other place that turns a raw counter into an id — see EnsureNetId.
            netId = NetId{_nextNetId++};
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

    // Here rather than anywhere incremental: this is the one point per tick that
    // has already accepted the scene as it is, including whatever came back from
    // the dead since the last one.
    RebuildControlIndex();

    // The escape classes, resolved once per tick rather than per connection.
    // Both lists are tiny by construction — an entity opting out of the provider
    // is unusual by definition — and the alternative is walking the whole live
    // set once per connection, which is exactly the cost relevancy exists to
    // avoid paying.
    _alwaysRelevant.clear();
    _controllerOnly.clear();
    if (_relevancy != nullptr)
    {
        for (const NetId netId : _liveNetIds)
        {
            const ECS::Entity entity = EntityOf(netId);
            if (entity == ECS::NullEntity)
                continue;

            const Replicated *marker = _scene.Get<Replicated>(entity);
            if (marker == nullptr || marker->relevance == Relevance::Default)
                continue;

            if (marker->relevance == Relevance::Always)
            {
                _alwaysRelevant.push_back(netId);
            }
            else
            {
                const ControlledBy *claim = _scene.Get<ControlledBy>(entity);
                _controllerOnly.emplace_back(netId, claim == nullptr ? InvalidClientId.value : claim->client);
            }
        }
    }
    // `_liveNetIds` is sorted, so both come out sorted without a sort — but the
    // binary searches downstream depend on it, so it is stated rather than left
    // to be rediscovered.
}

Core::Reflect::ComponentMask ReplicationServer::ExclusionMaskOf(ECS::Entity entity) const
{
    if (const Replicated *marker = _scene.Get<Replicated>(entity))
        return marker->excluded;
    return Core::Reflect::ComponentMask{};
}

bool ReplicationServer::ReplicatesAsBody(ECS::Entity entity, const Core::Reflect::ComponentMask &excluded) const
{
    if (_physics == nullptr || _scene.Get<Physics::RigidBody>(entity) == nullptr)
        return false;

    // Authored geometry is not simulated, whatever its live motion type says: a
    // static body's pose is *authored data* and already has a replication path,
    // the ordinary tracked-Transform delta. Keyed off the descriptor rather than
    // the live motion type on purpose — the editor holds a *dynamic* body Static
    // for the duration of a gizmo drag, and that body is still a simulated one
    // being placed.
    const Physics::RigidBodyDescriptor *descriptor = _scene.Get<Physics::RigidBodyDescriptor>(entity);
    if (descriptor == nullptr || descriptor->isStatic)
        return false;

    // Policy: the client will never build a body it was never sent a descriptor
    // for, so correcting one would be talking to nobody. Its Transform replicates
    // normally instead and the mirror becomes an interpolated visual — "show this
    // moving, don't simulate it", which is a useful thing to be able to author.
    if (excluded.Test(_descriptorOrdinal))
        return false;

    // ...and both client-side body builders require a Transform, so a bodied
    // entity withholding one can never have a body built either. Sending body
    // state it must drop on arrival is pure waste; treat it as non-bodied and let
    // the editor's warning explain that the mirror will sit at its load pose.
    if (excluded.Test(_transformOrdinal))
        return false;

    return true;
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

        // One predicate for every "does motion travel as body state" question in
        // this file — see ReplicatesAsBody for the four cases it rules out.
        if (!ReplicatesAsBody(entity, ExclusionMaskOf(entity)))
        {
            // Erasing is not tidiness. The retirement sweep at the bottom only
            // drops records for *retired* NetIds, so a record left behind by an
            // entity that stopped being bodied would keep its nonzero tick — and
            // that tick beats every post-sweep empty baseline, so the stale state
            // would be resent to every client after every keyframe sweep, for the
            // rest of the session. It would also suppress the first-sighting
            // capture if the entity ever became bodied again.
            _bodyStates.erase(netId);
            continue;
        }

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

        // Not active. Three reasons to record it anyway, and none of them is
        // "every tick":
        //
        //  - first sighting, which is what makes joining an already-settled
        //    world produce sleeping mirrors at the server's rest poses rather
        //    than a client-side re-settle;
        //  - it has just gone to sleep — the transition is the change, and the
        //    one whose loss used to be permanent;
        //  - it *moved while not simulating*. Nothing wakes a body for that, so
        //    the active set says nothing about it: the editor's gizmo holds a
        //    body Static for the duration of a drag precisely so the solver
        //    stops fighting the teleport, an inspector Transform edit does the
        //    same, gameplay can reposition a sleeping body, and a static body is
        //    never active at all — so for a replicated wall, "record it when it
        //    stops being active" fires once, at its load pose, and never again.
        //
        // Polling the pose rather than having every mutation site announce
        // itself is deliberate, and it is the same argument this design already
        // makes against Unreal's dormancy discipline: correctness that depends
        // on every call site remembering to flush is correctness that will be
        // forgotten. The cost is one transform read per resting replicated body
        // per tick, which is what "the physics world is the truth" is worth.
        //
        // A body that is asleep and has not moved records nothing, which is
        // where the idle-bandwidth property comes from.
        {
            const auto [position, rotation] = _physics->GetBodyTransform(*body);

            const bool firstSighting = record.tick == 0;
            const bool justSlept     = !firstSighting && !record.state.asleep;
            // Both poses are frozen while a body rests — nothing integrates them
            // — so this compares against float noise, not against motion. The
            // epsilons are well under one quantization step, so a move too small
            // to survive the encoder cannot trigger a send either.
            const bool movedAtRest =
                !firstSighting && record.state.asleep &&
                (glm::distance(record.state.position, position) > 1e-5f ||
                 std::abs(glm::dot(record.state.rotation, rotation)) < 1.f - 1e-6f);

            if (!firstSighting && !justSlept && !movedAtRest)
                continue;

            const bool transition = justSlept;
            record.state = BodyState{netId, position, rotation, glm::vec3{0.f}, glm::vec3{0.f}, /*asleep=*/true};
            record.tick  = _bodyStateTick;

            // Every other update is superseded by the next one; the final rest
            // pose is the one whose delay is permanently visible, because after
            // it the server has nothing more to say about this body.
            if (transition)
            {
                for (auto &[id, connection] : _connections)
                {
                    (void)id;
                    connection.priority[netId] += kSleepTransitionBoost;
                }
            }
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
        return referenced.value; // wire boundary
    };

    const Core::Reflect::ComponentRegistry &registry = Core::Reflect::ComponentRegistry::Instance();

    // This entity's own policy, read live. There is deliberately no cache: the
    // mask is a plain value on a component we already have to look up, so
    // caching would buy a hash lookup and cost an invalidation problem — which
    // is also why `Replicated` needs no change tracking.
    Core::Reflect::ComponentMask excluded;
    if (const Replicated *marker = _scene.Get<Replicated>(entity))
        excluded = marker->excluded;

    // What this entity has right now *and* is willing to send, in the same
    // packed, sorted form as the acked baseline — _replicatedComponents is
    // registry order, which is ascending by id, so this comes out sorted without
    // a sort.
    //
    // Excluded components are absent from this list, which is what makes the
    // rest fall out for free: the removal diff below sees them disappear from
    // the client's acked slice and sends removals, and D11 sees them reappear
    // and force-sends. One filter, both directions.
    const std::size_t componentsBegin = outComponents.size();
    for (std::size_t slot = 0; slot < _replicatedComponents.size(); ++slot)
    {
        if (excluded.Test(_replicatedOrdinals[slot]))
            continue;
        const Core::Reflect::ComponentId    id   = _replicatedComponents[slot];
        const Core::Reflect::ComponentMeta *meta = registry.ById(id);
        if (meta != nullptr && meta->getByEntity(&_scene, entity.index, entity.generation) != nullptr)
            outComponents.push_back(PackComponentRef(netId, id));
    }

    // Removals. Change detection stamps writes, not removals — there is no tick
    // to consult for "this component is gone" — so it is found by diffing this
    // entity's slice of the acked baseline against what it has now. Exactly the
    // shape of the despawn comparison, one level down.
    // ComponentId{0}: the packed key's lower bound, not "invalid" — 0 is the
    // lowest possible ordinal, and component ids are dense from there.
    const auto ackedLow  = std::lower_bound(connection.ackedComponents.begin(), connection.ackedComponents.end(),
                                            PackComponentRef(netId, Core::Reflect::ComponentId{0}));
    // netId + 1: the exclusive upper bound of this netId's packed-ref range,
    // spelled explicitly since NetId has no arithmetic of its own.
    const auto ackedHigh = std::lower_bound(connection.ackedComponents.begin(), connection.ackedComponents.end(),
                                            PackComponentRef(NetId{netId.value + 1}, Core::Reflect::ComponentId{0}));

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
        writer.WriteVarUInt32(id.value); // wire write

    // For an entity the *solver* owns, motion travels as body state, not as a
    // Transform. Sending both would double the cost of every moving object and
    // hand the client two disagreeing answers about where it is.
    //
    // The same predicate the capture uses, and it must be: an entity one of them
    // considers bodied and the other does not gets its Transform suppressed *and*
    // no body state, leaving the mirror frozen at its load pose.
    const bool bodied = ReplicatesAsBody(entity, excluded);

    for (std::size_t slot = 0; slot < _replicatedComponents.size(); ++slot)
    {
        if (excluded.Test(_replicatedOrdinals[slot]))
            continue; // this entity declines to send it

        const Core::Reflect::ComponentId     id   = _replicatedComponents[slot];
        const Core::Reflect::ComponentMeta *meta = registry.ById(id);
        if (meta == nullptr)
            continue;

        const void *component = meta->getByEntity(&_scene, entity.index, entity.generation);
        if (component == nullptr)
            continue;

        // D11, the removal diff's dual: the client does not have this component,
        // so send its full state regardless of what the change tick says.
        //
        // The change tick answers "did this value change since the client last
        // saw it", which is the wrong question whenever the *presence* changed
        // instead. Re-including an excluded component is exactly that case —
        // policy moved, the component did not, so its tick still predates the
        // baseline and the gate below would skip it until the next keyframe
        // sweep, up to several seconds of a mirror the server believes is whole.
        // It also covers the general case of a component the client lost while
        // the server has nothing new to stamp on it.
        const bool clientHasIt =
            std::binary_search(ackedLow, ackedHigh, PackComponentRef(netId, id));

        // sinceChangeTick == 0 is the empty baseline: spawn, late join, and a
        // client that has acked nothing all take this path, which is the whole
        // point of not having a separate full-state message.
        if (sinceChangeTick != 0 && clientHasIt && !_scene.ChangedById(entity, id, sinceChangeTick))
            continue;

        // ...and on that empty baseline a bodied entity's Transform still goes,
        // because it carries scale and the initial placement the client builds
        // its body at. Afterwards it is suppressed. The honest cost: a *non-pose*
        // Transform edit on a live body — a runtime scale change — reaches
        // clients only at the next keyframe sweep. Accepted for v1, because a
        // scale change on a live body needs a collider reshape the engine does
        // not do yet either (docs/replication-plan-v4.md §5).
        //
        // Suppression yields to D11 for the same reason the change gate does: a
        // client that has never received this Transform has nowhere to build its
        // body from, so "the body owns motion" has nothing to be true about yet.
        if (bodied && sinceChangeTick != 0 && clientHasIt && id == _transformComponentId)
            continue;

        writer.WriteBool(true);
        Core::Reflect::WriteComponent(*meta, component, writer, Core::Reflect::kAllFields, &context);
    }
    writer.WriteBool(false);
}

void ReplicationServer::WriteBodyStates(Connection &connection, const std::vector<NetId> &effective,
                                        Core::BitWriter &writer, SentSnapshot &record,
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

    // The effective set, not the live one. This loop is an independent walk with
    // its own acked-based gate, and that gate does *not* imply relevancy: an
    // entity that has left the set is still acked until its despawn round-trips,
    // so walking `_liveNetIds` here would ship body state for it every tick of
    // that window. Zero bytes has to mean zero.
    for (const NetId netId : effective)
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
    // `live ∩ R(c)`, once, and every pass below uses it instead of the live set.
    // Four passes, not three: the body-state section walks the set on its own
    // (see WriteBodyStates). Filtering happens strictly here, before priority —
    // the ordering every scaled system converged on, because a prioritizer that
    // also filters is two axes fused into one number.
    const std::vector<NetId> &effective = ComputeEffective(connection);

    Core::BitWriter writer;
    WriteMessageType(MessageType::Snapshot, writer);

    SnapshotHeader header;
    header.serverTick       = _simTick;
    header.baselineTick     = connection.ackedTick;
    header.inputBufferDepth = static_cast<std::uint32_t>(connection.input.Depth());
    header.starvedTicks     = static_cast<std::uint32_t>(connection.input.StarvedTicks());
    // Complete when the acked set already covers everything this connection is
    // *told* about — which is what "am I done joining?" means for a filtered
    // connection, and what it has always meant for an unfiltered one. Computed
    // against the previous ack rather than this snapshot, because that is the
    // only thing the client has actually confirmed receiving.
    header.worldComplete =
        std::includes(connection.acked.begin(), connection.acked.end(), effective.begin(), effective.end());
    WriteSnapshotHeader(header, writer);

    // Despawns: everything the client is known to have that it should no longer
    // have. Falls straight out of the same set difference that already found
    // destroyed entities — which is why leaving relevancy is not a second
    // mechanism. A revoke is a despawn, resent until acked, with the baseline
    // and priority entries erased when it lands.
    std::vector<NetId> despawns;
    std::set_difference(connection.acked.begin(), connection.acked.end(), effective.begin(), effective.end(),
                        std::back_inserter(despawns));
    writer.WriteVarUInt32(static_cast<std::uint32_t>(despawns.size()));
    for (const NetId netId : despawns)
        writer.WriteVarUInt32(netId.value); // wire write

    // Collect, order, drain to a budget. The ordering is a Tribes-style priority
    // accumulator: every live entity gains max(Replicated::priority, eps) each
    // snapshot tick, entities go out highest-first, and only the ones that
    // actually went reset. Under no budget pressure this is inert — everything
    // dirty goes every tick and everything resets. Under pressure, correction
    // *frequency* degrades smoothly and per object, steered by an authored
    // number, instead of by whichever NetId happened to be lowest.
    SentSnapshot record;
    record.serverTick = _simTick;
    record.netIds.reserve(effective.size());
    record.written.reserve(effective.size());

    // Sampled once, before anything is written, and stamped onto every entity
    // this snapshot writes. Nothing mutates the scene while a snapshot is being
    // built, so one reading is honest for all of them — and taking it up front
    // means a change made after this point cannot be mistaken for delivered.
    const std::uint64_t captureTick = _scene.CurrentChangeTick();

    std::vector<std::pair<float, NetId>> order;
    order.reserve(effective.size());
    for (const NetId netId : effective)
    {
        const ECS::Entity entity = EntityOf(netId);
        if (entity == ECS::NullEntity)
            continue;

        float authored = 1.f;
        if (const Replicated *marker = _scene.Get<Replicated>(entity))
            authored = marker->priority;

        float &accumulator = connection.priority[netId];
        accumulator += std::max(authored, kMinPriorityGain);
        order.emplace_back(accumulator, netId);
    }
    // Highest first; NetId breaks ties so the order is stable rather than
    // whatever the float comparison happened to do.
    std::sort(order.begin(), order.end(),
              [](const auto &lhs, const auto &rhs)
              { return lhs.first != rhs.first ? lhs.first > rhs.first : lhs.second < rhs.second; });

    std::uint32_t backlog = 0;
    for (const auto &[accumulated, netId] : order)
    {
        (void)accumulated;
        const ECS::Entity entity = EntityOf(netId);
        if (entity == ECS::NullEntity)
            continue;

        const bool known = std::binary_search(connection.acked.begin(), connection.acked.end(), netId);

        if (writer.BytesWritten() >= _config.maxSnapshotBytes)
        {
            ++backlog;
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
                //
                // ComponentId{0}: the packed key's lower bound, not "invalid" —
                // 0 is the lowest possible ordinal, and component ids are dense
                // from there.
                const auto low  = std::lower_bound(connection.ackedComponents.begin(),
                                                   connection.ackedComponents.end(),
                                                   PackComponentRef(netId, Core::Reflect::ComponentId{0}));
                // netId + 1: the exclusive upper bound of this netId's packed-ref
                // range, spelled explicitly since NetId has no arithmetic of its own.
                const auto high = std::lower_bound(connection.ackedComponents.begin(),
                                                   connection.ackedComponents.end(),
                                                   PackComponentRef(NetId{netId.value + 1}, Core::Reflect::ComponentId{0}));
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
        writer.WriteVarUInt32(netId.value); // wire write
        writer.WriteBool(!known); // isSpawn

        WriteEntityComponents(netId, entity, sinceChangeTick, connection, writer, record.components);
        record.netIds.push_back(netId);
        record.written.push_back(WrittenEntity{netId, EntityBaseline{captureTick, 0}});

        // It went out, so its turn is over. The ones that did not go keep what
        // they accumulated — which is the whole anti-starvation property.
        connection.priority[netId] = 0.f;
    }
    writer.WriteBool(false);
    connection.diagnostics.dirtyBacklog = backlog;

    // Priority order is not NetId order, and both the body pass and the ack path
    // binary-search these.
    std::sort(record.netIds.begin(), record.netIds.end());
    std::sort(record.written.begin(), record.written.end(),
              [](const WrittenEntity &lhs, const WrittenEntity &rhs) { return lhs.netId < rhs.netId; });

    // Motion, for everything the physics world owns. After the entity blocks so
    // ordering with a spawn in the same packet is free: the entity and its
    // descriptor exist by the time the body state lands, and the client's body
    // starts at the authoritative state rather than re-settling from the level
    // file's pose.
    const std::size_t writtenFromComponents = record.written.size();
    WriteBodyStates(connection, effective, writer, record, writtenFromComponents);

    // Last, after the entity blocks and the body states, so a message about an
    // entity established earlier *in this same packet* inherits its ordering
    // from the framing itself. That is the one genuine improvement the survey
    // found in replicon's tick-sync guarantee, and here it costs nothing: the
    // snapshot already carries the tick and the wire order already puts entity
    // blocks first.
    WriteEventSection(connection, writer, record);

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

    // End of tick: after every state mutation this tick produced, so a host-side
    // handler sees a world at least as new as the message it is handling — the
    // property a remote client gets free from packet ordering.
    DispatchHostEvents();
}

// ===========================================================================
// ReplicationClient
// ===========================================================================

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

    const std::uint32_t despawnCount = reader.ReadVarUInt32();
    if (!reader.Ok() || despawnCount > 65536u)
    {
        reader.Invalidate();
        return false;
    }
    for (std::uint32_t i = 0; i < despawnCount; ++i)
    {
        const NetId netId = NetId{reader.ReadVarUInt32()}; // wire read
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

void ReplicationClient::SyncMirrorBody(NetId netId, ECS::Entity entity)
{
    if (_physics == nullptr || !_scene.IsAlive(entity))
        return;

    const Physics::RigidBodyDescriptor *descriptor = _scene.Get<Physics::RigidBodyDescriptor>(entity);
    const ECS::Transform               *transform  = _scene.Get<ECS::Transform>(entity);
    if (descriptor == nullptr || transform == nullptr)
        return; // not a physical entity, or not fully described yet

    if (_scene.Get<Physics::RigidBody>(entity) == nullptr)
    {
        (void)_physics->AddBodyFromDescriptor(_scene, entity, *transform, *descriptor);
        const Physics::RigidBody *created = _scene.Get<Physics::RigidBody>(entity);
        if (created == nullptr)
            return;

        _bodies[netId].body = *created;
        // It is a simulated mirror now, not an interpolated one.
        _transformHistory.erase(netId);
        return;
    }

    // A dynamic body is owned by the correction stream from here on; touching it
    // from the component path would fight it every snapshot.
    if (!descriptor->isStatic)
        return;

    // Authored-static geometry moves by being *authored*, so its Transform is
    // the truth and the collider has to follow it. Without this the client's
    // visual moves and its collision does not — the worse half of a desync,
    // because it is invisible until something falls through it.
    const Physics::RigidBody *body = _scene.Get<Physics::RigidBody>(entity);
    _physics->SetBodyTransform(*body, transform->position, transform->rotation);
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

    const Physics::RigidBody *body   = _scene.Get<Physics::RigidBody>(entity);
    MirrorBody               &record = _bodies[state.netId];

    // How far the two simulations drifted apart since the last correction. Taken
    // before the snap, against the *physics* pose rather than the rendered one:
    // this is the number the correction cadence has to be justified by, and
    // folding the previous frame's cosmetic offset into it would flatter it.
    const auto [simulatedPosition, simulatedRotation] = _physics->GetBodyTransform(*body);
    const float divergence                            = glm::length(simulatedPosition - state.position);

    ++_corrections.applied;
    _corrections.divergenceSum += static_cast<double>(divergence);
    _corrections.divergenceMax = std::max(_corrections.divergenceMax, divergence);

    // The rendered pose right now, which the correction must not change: the
    // sim is snapped, and the offset absorbs the whole difference so the screen
    // sees nothing happen at this instant. Successive corrections accumulate
    // into the same offset, which is what keeps a stream of small ones smooth
    // rather than each one fighting the last.
    const glm::vec3 renderedPosition = simulatedPosition + record.positionError;
    const glm::quat renderedRotation = record.rotationError * simulatedRotation;

    // The simulation is snapped hard, with no smoothing: extrapolation has to
    // proceed from a valid physics state, and a half-applied correction is not
    // one. Hiding the jump belongs to the view.
    _physics->ApplyBodyState(*body, state.position, state.rotation, state.linearVelocity, state.angularVelocity,
                             /*activate=*/!state.asleep);

    record.positionError = renderedPosition - state.position;
    record.rotationError = glm::normalize(renderedRotation * glm::inverse(state.rotation));

    const ViewSmoothing &smoothing = Smoothing();
    const float          carried   = glm::length(record.positionError);

    // Two ways an offset is not worth carrying. Below the floor the correction
    // is too small to see, so smoothing it buys nothing and only delays
    // convergence. Past the ceiling, smoothing reads worse than admitting the
    // jump: a body sliding half a room to catch up looks like a bug, where a
    // teleport looks like a teleport.
    if (carried < smoothing.snapBelowDistance || carried > smoothing.hardSnapDistance)
    {
        record.positionError   = glm::vec3{0.f};
        record.rotationError   = glm::quat{1.f, 0.f, 0.f, 0.f};
        record.smoothingWindow = 0.f;
    }
    else
    {
        // A bigger jump gets a shorter window: it is worth being over with
        // sooner. The time-domain form of the published two-rate split.
        record.smoothingWindow =
            carried <= smoothing.smallErrorDistance
                ? smoothing.positionCorrectionTime
                : (carried >= smoothing.largeErrorDistance
                       ? smoothing.positionCorrectionTimeFast
                       : glm::mix(smoothing.positionCorrectionTime, smoothing.positionCorrectionTimeFast,
                                  (carried - smoothing.smallErrorDistance) /
                                      (smoothing.largeErrorDistance - smoothing.smallErrorDistance)));

        // Restarted by every correction, from wherever the picture currently is,
        // which is what keeps a correction arriving mid-convergence continuous.
        record.positionErrorStart = record.positionError;
        record.rotationErrorStart = record.rotationError;
        record.smoothingElapsed   = 0.f;
    }

    record.asleep       = state.asleep;
    record.restPosition = state.position;
    record.restRotation = state.rotation;
}

void ReplicationClient::SmoothView(double serverTimeTicks, float dt)
{
    // Non-bodied mirrors: interpolate between received samples, unchanged.
    Interpolate(serverTimeTicks);

    if (_physics == nullptr || dt <= 0.f)
        return;

    const ViewSmoothing &smoothing = Smoothing();

    for (auto &[netId, record] : _bodies)
    {
        const auto entity = _entityByNetId.find(netId);
        if (entity == _entityByNetId.end() || !_scene.IsAlive(entity->second))
            continue;

        ECS::Transform *transform = _scene.GetMut<ECS::Transform>(entity->second);
        if (transform == nullptr)
            continue;

        if (record.smoothingWindow <= 0.f)
            continue; // nothing to hide

        // Linear over the window, so the offset is gone by the deadline and
        // moves at a constant on-screen speed until it is. Advancing in *time*
        // rather than per frame is what keeps the feel identical at 30 and at
        // 144 Hz.
        record.smoothingElapsed += dt;
        const float remaining =
            1.f - std::min(1.f, record.smoothingElapsed / record.smoothingWindow);

        record.positionError = record.positionErrorStart * remaining;
        record.rotationError = glm::normalize(
            glm::slerp(glm::quat{1.f, 0.f, 0.f, 0.f}, record.rotationErrorStart,
                       // Orientation gets its own, shorter, window.
                       1.f - std::min(1.f, record.smoothingElapsed / smoothing.rotationCorrectionTime)));

        // On top of the physics writeback's pose, which ran just before this.
        transform->position += record.positionError;
        transform->rotation = record.rotationError * transform->rotation;
    }
}

void ReplicationClient::RequestKeyframe()
{
    if (!_synchronized)
        return;

    Core::BitWriter writer;
    WriteMessageType(MessageType::RequestKeyframe, writer);
    // Reliable: the point of asking is that something is already wrong, and an
    // unreliable request that gets dropped looks exactly like a button that does
    // nothing.
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
    _corrections       = CorrectionStats{};
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
