/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Replication.hpp
/// @brief Server and client halves of the state-replication protocol.
///
/// The model is server-authoritative delta snapshots against a per-connection
/// acked baseline. The server simulates; every net tick it sends each client
/// what changed since the last snapshot *that client acknowledged*. Nothing is
/// sent reliably, and nothing needs to be: a lost snapshot is not retransmitted,
/// it is superseded — the next one simply deltas against an older baseline and
/// therefore carries more. Loss degrades into bandwidth, never into desync.
///
/// The two classes are halves of one protocol and must be read together; a
/// change to either one's wire handling is a change to both.

#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Net/NetTransport.hpp>
#include <Assisi/NetSync/InputCommand.hpp>
#include <Assisi/NetSync/NetClock.hpp>
#include <Assisi/NetSync/NetProtocol.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Assisi::NetSync
{

/// @brief Tuning shared by both halves.
struct ReplicationConfig
{
    /// The simulation's fixed-step rate. Snapshots are timed against it.
    std::uint32_t tickRateHz = 60;

    /// How often state goes out. **Clamped to a divisor of tickRateHz** so
    /// snapshots always land on exact ticks — a rate that does not divide would
    /// make the interval alternate between two values, which shows up as
    /// interpolation judder rather than as an error anyone would look for.
    ///
    /// 20-30 Hz is the Source/Godot-normal band at this scale: simulation and
    /// input stay at the full tick rate, only *state* is sent at this one, and
    /// interpolation hides the gap. Halving it roughly halves both diff cost and
    /// bandwidth.
    std::uint32_t snapshotHz = 20;

    /// Soft cap on one snapshot's payload. The send loop stops adding entities
    /// once it is exceeded; whatever is left is picked up next snapshot, its
    /// baseline unchanged because the client cannot have acked what it never got.
    std::size_t maxSnapshotBytes = 1100;

    /// How many unacked snapshots to remember per connection. Each one holds the
    /// entity set and change tick a future ack would select as a baseline; once
    /// this many are outstanding the connection has effectively stopped acking,
    /// and the oldest is dropped so the memory is bounded.
    std::size_t maxInFlightSnapshots = 32;

    /// Ceiling on input packets accepted per connection per second. A client
    /// that exceeds it is flooding, and the excess is dropped before the codec
    /// runs — the cheapest possible place to say no.
    std::uint32_t maxInputPacketsPerSecond = 200;

    /// Bounds a command must satisfy before the simulation sees it.
    InputLimits inputLimits;
};

/// @brief Per-connection counters, for debug overlays and tests.
struct ConnectionDiagnostics
{
    std::uint64_t snapshotsSent      = 0;
    std::uint64_t bytesSent          = 0;
    std::uint64_t acksReceived       = 0;
    std::uint64_t inputPacketsDropped = 0; ///< Rate limit or malformed.
    std::uint64_t commandsClamped    = 0;  ///< Tripped ClampInputCommand.
    std::uint32_t inFlightSnapshots  = 0;
};

/// @brief The authoritative half. Owns NetId assignment and snapshot sending.
///
/// Drive it from the fixed-step loop: Poll() at the top of the tick to take
/// input and acks, the simulation in between, then Tick() at the end to send
/// what changed.
class ReplicationServer
{
  public:
    ReplicationServer(Net::NetTransport &transport, ECS::Scene &scene, ReplicationConfig config = {});

    ReplicationServer(const ReplicationServer &)            = delete;
    ReplicationServer &operator=(const ReplicationServer &) = delete;

    /// @brief Register a connection the transport has reported as Connected.
    /// Sends the handshake; the client is not eligible for snapshots until it
    /// answers with a matching protocol hash.
    void AddConnection(Net::ConnectionId connection);

    /// @brief Forget a connection. Its NetIds stay allocated — they belong to
    /// the entities, not to whoever was watching them.
    void RemoveConnection(Net::ConnectionId connection);

    /// @brief Handle one received message. Everything that arrives from a client
    /// goes through here, and everything here treats its input as hostile.
    void HandleMessage(Net::ConnectionId connection, std::span<const std::byte> payload);

    /// @brief Advance to @p simTick: reconcile the NetId map with the scene, and
    /// send a snapshot to every ready connection if this tick is a snapshot tick.
    void Tick(std::uint64_t simTick);

    /// @brief Take the command a connection's queue holds for @p tick, or null.
    /// Call once per connection per tick, from the simulation.
    const InputCommand *ConsumeInput(Net::ConnectionId connection, std::uint64_t tick);

    /// @brief The NetId assigned to @p entity, or InvalidNetId. Assigned lazily
    /// on the first Tick() that sees the entity, so an entity created this tick
    /// has no id until then.
    [[nodiscard]] NetId NetIdOf(ECS::Entity entity) const;

    /// @brief Inverse of NetIdOf.
    [[nodiscard]] ECS::Entity EntityOf(NetId netId) const;

    [[nodiscard]] std::size_t ConnectionCount() const { return _connections.size(); }

    /// @brief Whether the connection completed its handshake and is receiving
    /// snapshots.
    [[nodiscard]] bool IsReady(Net::ConnectionId connection) const;

    [[nodiscard]] const ConnectionDiagnostics *Diagnostics(Net::ConnectionId connection) const;

    /// @brief True when @p simTick is one the config says to send state on.
    [[nodiscard]] bool IsSnapshotTick(std::uint64_t simTick) const;

    [[nodiscard]] const ReplicationConfig &Config() const { return _config; }

  private:
    /// One in-flight snapshot's worth of "what the client would know if it acks
    /// this". The entity set is what makes spawn and despawn fall out of the
    /// same comparison, and the change tick is the delta baseline.
    struct SentSnapshot
    {
        std::uint64_t      serverTick      = 0;
        std::uint64_t      sceneChangeTick = 0;
        std::vector<NetId> netIds; ///< Sorted ascending.
    };

    struct Connection
    {
        Net::ConnectionId     id    = Net::InvalidConnection;
        bool                  ready = false; ///< Handshake completed.
        std::deque<SentSnapshot> inFlight;

        /// The acked baseline: the entity set the client is known to have, and
        /// the scene change tick it corresponds to.
        std::vector<NetId> acked;
        std::uint64_t      ackedTick       = 0;
        std::uint64_t      ackedChangeTick = 0;

        InputCommandQueue     input;
        ConnectionDiagnostics diagnostics;

        /// Sliding one-second window for the input rate limit.
        std::uint64_t rateWindowTick   = 0;
        std::uint32_t packetsInWindow  = 0;
    };

    void SendHello(Connection &connection);
    void SendReject(Connection &connection, RejectReason reason);
    void SendSnapshot(Connection &connection);
    void HandleClientHello(Connection &connection, Core::BitReader &reader);
    void HandleAck(Connection &connection, Core::BitReader &reader);
    void HandleInput(Connection &connection, Core::BitReader &reader);

    /// Assign NetIds to newly-replicated entities and drop mappings for entities
    /// that are gone. Run once per tick, before any snapshot is built, so every
    /// connection sees the same world.
    void ReconcileNetIds();

    /// Write one entity's component blocks. @p sinceChangeTick of 0 means "send
    /// everything" — the empty-baseline case that spawn and late-join share with
    /// an ordinary delta.
    /// @return the number of component blocks written.
    std::uint32_t WriteEntityComponents(ECS::Entity entity, std::uint64_t sinceChangeTick, Core::BitWriter &writer);

    Net::NetTransport &_transport;
    ECS::Scene        &_scene;
    ReplicationConfig  _config;

    std::unordered_map<Net::ConnectionId, Connection> _connections;

    std::unordered_map<NetId, ECS::Entity>      _entityByNetId;
    std::unordered_map<std::uint64_t, NetId>    _netIdByEntity; ///< Keyed by the entity's packed handle.
    std::vector<NetId>                          _liveNetIds;    ///< Sorted; rebuilt each ReconcileNetIds.
    NetId                                       _nextNetId = 1;

    std::uint64_t _simTick     = 0;
    std::uint64_t _snapshotDiv = 3; ///< tickRateHz / snapshotHz, at least 1.

    /// Component ids the protocol replicates, resolved once: every serializable
    /// reflected component except the marker itself. Cached because it is walked
    /// per entity per snapshot.
    std::vector<Core::Reflect::ComponentId> _replicatedComponents;
};

/// @brief The receiving half. Applies snapshots into a local scene.
class ReplicationClient
{
  public:
    ReplicationClient(Net::NetTransport &transport, ECS::Scene &scene, Net::ConnectionId connection);

    ReplicationClient(const ReplicationClient &)            = delete;
    ReplicationClient &operator=(const ReplicationClient &) = delete;

    /// @brief Handle one received message.
    void HandleMessage(std::span<const std::byte> payload);

    /// @brief Send this tick's input window. Call once per fixed step, after
    /// sampling.
    void SendInput(const InputCommandBuffer &buffer);

    /// @brief Whether the handshake succeeded and snapshots are being applied.
    [[nodiscard]] bool IsSynchronized() const { return _synchronized; }

    /// @brief Set when the server refused the connection, so a UI can say why.
    [[nodiscard]] const std::string &RejectMessage() const { return _rejectMessage; }

    /// @brief The clock feedback from the most recent snapshot, for NetClock.
    [[nodiscard]] const ClockFeedback &Feedback() const { return _feedback; }

    /// @brief The last server tick applied.
    [[nodiscard]] std::uint64_t LastAppliedTick() const { return _lastAppliedTick; }

    [[nodiscard]] std::uint64_t SnapshotsApplied() const { return _snapshotsApplied; }

    /// @brief Snapshots rejected as malformed. Should be zero; a nonzero value
    /// means either corruption the transport did not catch or a protocol bug,
    /// and both are worth surfacing rather than silently tolerating.
    [[nodiscard]] std::uint64_t SnapshotsRejected() const { return _snapshotsRejected; }

    /// @brief The local entity mirroring @p netId, or NullEntity.
    [[nodiscard]] ECS::Entity EntityOf(NetId netId) const;

    [[nodiscard]] std::size_t ReplicatedEntityCount() const { return _entityByNetId.size(); }

    /// @brief The server's advertised timing, valid once synchronized.
    [[nodiscard]] std::uint32_t ServerTickRateHz() const { return _tickRateHz; }
    [[nodiscard]] std::uint32_t ServerSnapshotHz() const { return _snapshotHz; }

    /// @brief Drop every replicated entity and forget the session. v1's
    /// reconnect is a full rejoin, which starts here.
    void Reset();

  private:
    bool ApplySnapshot(Core::BitReader &reader);
    void SendAck(std::uint64_t serverTick);
    void SendHello();

    /// A reference to a NetId that had not arrived yet when it was decoded.
    /// Component data can legitimately mention an entity whose spawn is in a
    /// later block or a later snapshot, so the reference is stored and patched
    /// when the target appears rather than resolved to nothing.
    struct PendingRef
    {
        ECS::Entity                entity;
        Core::Reflect::ComponentId component = Core::Reflect::kInvalidComponentId;
        std::size_t                fieldOffset = 0;
        NetId                      target      = InvalidNetId;
    };

    void ResolvePendingRefs();

    Net::NetTransport &_transport;
    ECS::Scene        &_scene;
    Net::ConnectionId  _connection;

    std::unordered_map<NetId, ECS::Entity> _entityByNetId;
    std::vector<PendingRef>                _pendingRefs;

    ClockFeedback _feedback;
    std::string   _rejectMessage;

    std::uint64_t _lastAppliedTick   = 0;
    std::uint64_t _snapshotsApplied  = 0;
    std::uint64_t _snapshotsRejected = 0;
    std::uint32_t _tickRateHz        = 60;
    std::uint32_t _snapshotHz        = 20;
    bool          _synchronized      = false;
};

} // namespace Assisi::NetSync
