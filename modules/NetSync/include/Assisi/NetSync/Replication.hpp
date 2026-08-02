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
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Net/NetTransport.hpp>
#include <Assisi/NetSync/BodyState.hpp>
#include <Assisi/NetSync/InputCommand.hpp>
#include <Assisi/NetSync/NetClock.hpp>
#include <Assisi/NetSync/NetProtocol.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

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

    /// How often, in ticks, to re-anchor every connection from the empty
    /// baseline. 0 disables it. Default 512 — about 8.5 s at 60 Hz.
    ///
    /// **Insurance, not a pillar.** Delivery is already guaranteed by the acked
    /// baseline: every state change, including a body's final rest pose, is
    /// resent until the client confirms it, and a lost despawn self-heals
    /// through the acked-set diff. What no delivery guarantee can fix is what
    /// happens *after* delivery — state that arrived, was acked, and then went
    /// wrong locally. This sweep is the answer to that class, which is exactly
    /// the class nobody designs for.
    ///
    /// Turning it off saves almost nothing (~64 entities × ~80 bytes spread over
    /// 8.5 s ≈ 0.6 kB/s) and, for any client-side system that writes replicated
    /// fields on mirrors, converts "wrong until the next sweep" into "wrong
    /// forever" — the delta path never resends a field the server is not
    /// re-stamping. A knob that saves that little and removes that much has to
    /// say so where it is flipped.
    std::uint64_t keyframeIntervalTicks = 512;

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
    /// Entities this connection holds a delta baseline for. Should track the
    /// live replicated count; a value that climbs without bound is a retired
    /// NetId that never left the map.
    std::uint32_t baselineEntries    = 0;
    /// How many times this connection has been re-anchored from the empty
    /// baseline by the keyframe sweep, or by a client asking for one.
    std::uint64_t keyframeSweeps     = 0;
    /// Entities that had something to send and did not fit in the last
    /// snapshot. Zero is the normal state; a number that stays high means the
    /// byte budget is binding and correction *frequency* is degrading — which
    /// the priority accumulator makes fair, not free.
    std::uint32_t dirtyBacklog       = 0;
};

/// @brief The authoritative half. Owns NetId assignment and snapshot sending.
///
/// Drive it from the fixed-step loop: Poll() at the top of the tick to take
/// input and acks, the simulation in between, then Tick() at the end to send
/// what changed.
class ReplicationServer
{
  public:
    /// @param physics The world whose bodies are the authority on motion. Null
    ///   keeps the pre-body behaviour exactly — Transforms replicate as ordinary
    ///   components and no body state is sent — which is what every test with no
    ///   physics world in scope wants.
    ReplicationServer(Net::NetTransport &transport, ECS::Scene &scene, Physics::PhysicsWorld *physics = nullptr,
                      ReplicationConfig config = {});

    ReplicationServer(const ReplicationServer &)            = delete;
    ReplicationServer &operator=(const ReplicationServer &) = delete;

    /// @brief Declare which level this server is running.
    ///
    /// Carried in every subsequent `ServerHello`, so set it before the first
    /// connection arrives. A server that leaves it unset advertises
    /// `LevelAddressing::None`, which a joining editor treats as a clean abort
    /// — better than letting it join a world whose static half it cannot build.
    void SetLevelIdentity(LevelIdentity level) { _level = std::move(level); }

    [[nodiscard]] const LevelIdentity &Level() const { return _level; }

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
    /// How much of one entity a connection is known to have.
    ///
    /// Per entity rather than one tick per connection, and that distinction is
    /// the whole fix for a real bug in the shipped core: an entity skipped for
    /// byte budget was still recorded in the in-flight snapshot, whose *global*
    /// change tick became the baseline on ack — so the skipped entity's pending
    /// changes were retroactively declared delivered and never sent again. Here,
    /// "included in the record" and "delivered at tick X" are separate facts,
    /// and an entity the budget skipped simply keeps its old baseline.
    struct EntityBaseline
    {
        std::uint64_t componentTick = 0; ///< Component state delivered up to here.
        std::uint64_t bodyTick      = 0; ///< Body state delivered up to here (R5 fills this).
    };

    /// What one entity was written at inside one snapshot. Only entities the
    /// snapshot *actually wrote* get an entry — that is the point.
    struct WrittenEntity
    {
        NetId          netId = InvalidNetId;
        EntityBaseline ticks;
    };

    /// One in-flight snapshot's worth of "what the client would know if it acks
    /// this". The entity set is what makes spawn and despawn fall out of the
    /// same comparison, and the per-entity ticks are the delta baselines.
    struct SentSnapshot
    {
        std::uint64_t              serverTick = 0;
        std::vector<WrittenEntity> written; ///< Sorted ascending by netId.
        std::vector<NetId>         netIds;  ///< Sorted ascending.

        /// Which components each of those entities had, as sorted
        /// `(netId << 32) | componentId` pairs.
        ///
        /// Change detection stamps writes, not removals — nothing in the ECS
        /// reports "this component is gone". So component removal is found the
        /// same way entity despawn is: by comparing what the client is known to
        /// have against what exists now. One packed integer per component per
        /// entity, which is small and, unlike a fixed bitmask, has no ceiling on
        /// how many component types the game may register.
        std::vector<std::uint64_t> components;
    };

    struct Connection
    {
        Net::ConnectionId     id    = Net::InvalidConnection;
        bool                  ready = false; ///< Handshake completed.
        std::deque<SentSnapshot> inFlight;

        /// The acked baseline: the entity set the client is known to have, the
        /// components those entities had, and — per entity — how far its state
        /// has been delivered.
        std::vector<NetId>         acked;
        std::vector<std::uint64_t> ackedComponents;
        std::uint64_t              ackedTick = 0;

        /// One entry per entity this connection has acked. Erased when its
        /// despawn acks: NetIds are never reused, so without that this grows
        /// with every entity that has *ever* replicated — unbounded under
        /// projectile-style churn. Two uint64s per live entity per connection
        /// otherwise, which is noise at the target scale.
        std::unordered_map<NetId, EntityBaseline> baselines;

        /// The Tribes-lineage send priority, per entity. Each snapshot tick every
        /// entity with something to send gains `max(Replicated::priority, eps)`;
        /// entities drain highest-first into the byte budget, and **only the
        /// drained reset**, so the ones that missed keep climbing and cannot
        /// starve.
        ///
        /// Inert when the budget is not binding: everything dirty goes every
        /// tick and every accumulator resets. Under pressure it degrades
        /// correction *frequency* smoothly, per object, steered by an authored
        /// number — the debris pile at priority 0.5 yields to the door at 10
        /// precisely when bandwidth forces the choice.
        std::unordered_map<NetId, float> priority;

        InputCommandQueue     input;
        ConnectionDiagnostics diagnostics;

        /// Sliding one-second window for the input rate limit.
        std::uint64_t rateWindowTick   = 0;
        std::uint32_t packetsInWindow  = 0;
    };

    void SendHello(Connection &connection);
    void SendReject(Connection &connection, RejectReason reason);
    void SendSnapshot(Connection &connection);

    /// Re-anchor @p connection from the empty baseline: forget every per-entity
    /// tick, and clear the in-flight ring with them.
    ///
    /// The ring clear is not tidiness. An ack for a pre-sweep snapshot arriving
    /// *after* the sweep would fold that record's per-entity ticks back into the
    /// baselines and silently cancel the re-anchor for exactly the entities it
    /// covered. With the ring cleared, a late ack finds no record and is ignored
    /// — at the cost of one over-full resend, which is the correct direction to
    /// be wrong in.
    ///
    /// The acked entity and component *sets* are deliberately untouched: this
    /// resets what the client is known to have *seen*, not what it is known to
    /// *hold*, and clearing the sets would turn every entity into a spawn and
    /// break despawn detection.
    static void ResetBaselines(Connection &connection);
    void HandleClientHello(Connection &connection, Core::BitReader &reader);
    void HandleAck(Connection &connection, Core::BitReader &reader);
    void HandleInput(Connection &connection, Core::BitReader &reader);

    /// Assign NetIds to newly-replicated entities and drop mappings for entities
    /// that are gone. Run once per tick, before any snapshot is built, so every
    /// connection sees the same world.
    void ReconcileNetIds();

    /// Refresh `_bodyStates` from the physics world. Once per tick, before any
    /// snapshot is built, for the same reason as ReconcileNetIds.
    ///
    /// Three cases, and the second is the one worth naming: a body that just
    /// *stopped* being active has to have its rest state recorded and its tick
    /// bumped, because the sleep transition is itself a change and it is the one
    /// whose loss used to be permanent. (The first is an awake body, recorded
    /// every tick. The third is a NetId seen for the first time, recorded
    /// whatever its state — so joining a world that settled before anyone
    /// connected produces sleeping mirrors at the server's rest poses instead of
    /// a client-side re-settle.)
    void CaptureBodyStates();

    /// Write the snapshot's body-state section for @p connection, returning the
    /// per-entity ticks it wrote so the ack can fold them in.
    void WriteBodyStates(Connection &connection, Core::BitWriter &writer, SentSnapshot &record,
                         std::size_t writtenFromComponents);

    /// Write one entity's removed-component list and then its changed-component
    /// blocks. @p sinceChangeTick of 0 means "send everything" — the
    /// empty-baseline case that spawn and late-join share with an ordinary
    /// delta. Appends this entity's current `(netId, componentId)` pairs to
    /// @p outComponents, which becomes the next baseline.
    void WriteEntityComponents(NetId netId, ECS::Entity entity, std::uint64_t sinceChangeTick,
                               const Connection &connection, Core::BitWriter &writer,
                               std::vector<std::uint64_t> &outComponents);

    Net::NetTransport     &_transport;
    ECS::Scene            &_scene;
    Physics::PhysicsWorld *_physics = nullptr;
    ReplicationConfig      _config;
    LevelIdentity          _level;

    /// The last state captured for a replicated body, and when.
    struct BodyRecord
    {
        BodyState     state;
        std::uint64_t tick = 0; ///< 0 = never captured.
    };

    /// The priority bump a sleep transition earns.
    ///
    /// Deliberately large. Every other update is superseded by the next one; the
    /// final rest pose is the one whose delay is *permanently* visible, because
    /// after it the server has nothing more to say about that body.
    static constexpr float kSleepTransitionBoost = 100.f;

    /// Floor on the per-tick priority gain. `Replicated::priority` is authored
    /// down to 0.0, and a raw gain of zero means a zero-priority entity never
    /// climbs — silently starved forever under budget pressure. With the clamp,
    /// 0 means "last in line", never "never".
    static constexpr float kMinPriorityGain = 1.f / 64.f;

    /// Per-NetId body state, refreshed once per tick by CaptureBodyStates.
    std::unordered_map<NetId, BodyRecord> _bodyStates;

    /// Monotonic, bumped once per capture. Its own counter rather than the
    /// scene's change tick, because the scene's only advances when someone
    /// touches a component — and the whole point of reading the physics world
    /// directly is that a moving body no longer does.
    std::uint64_t _bodyStateTick = 0;

    /// Scratch for CaptureBodyStates, kept so a tick that captures a hundred
    /// bodies does not allocate a hundred times a second.
    std::vector<Physics::PhysicsWorld::ActiveBodyState> _activeBodies;

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

    /// Resolved once, so the per-entity write loop can suppress the Transform of
    /// a bodied entity without a registry lookup per component per entity.
    Core::Reflect::ComponentId _transformComponentId = Core::Reflect::kInvalidComponentId;
};

/// @brief The receiving half. Applies snapshots into a local scene.
class ReplicationClient
{
  public:
    /// @param physics The world this client simulates its mirrors in. Null keeps
    ///   the pre-body behaviour exactly — every mirror is rendered by
    ///   interpolation and no body is ever built — which is what a headless
    ///   convergence test with no physics world in scope wants.
    ReplicationClient(Net::NetTransport &transport, ECS::Scene &scene, Net::ConnectionId connection,
                      Physics::PhysicsWorld *physics = nullptr);

    ReplicationClient(const ReplicationClient &)            = delete;
    ReplicationClient &operator=(const ReplicationClient &) = delete;

    /// @brief Hold the `ClientHello` until the local world has been built.
    ///
    /// Off by default, which is exactly today's behaviour: answer the handshake
    /// the moment it arrives. A client that loads a level first must turn it on
    /// *before* the first Poll, because the ordering hazard is real — snapshots
    /// arriving against a world that has not been built yet map the server's
    /// NetIds onto whichever local entities happen to occupy those slots, and
    /// the resulting scene is wrong in a way nothing later corrects.
    ///
    /// With it set the sequence is: ServerHello arrives → IsAwaitingLevel() goes
    /// true and Handshake() names the level → the application builds its world →
    /// ConfirmLevelReady() (or AbortJoin() with a reason a human can act on).
    void SetDeferHandshake(bool defer) { _deferHandshake = defer; }

    /// @brief True between a verified `ServerHello` and ConfirmLevelReady() /
    /// AbortJoin(). Only reachable with SetDeferHandshake(true).
    [[nodiscard]] bool IsAwaitingLevel() const { return _awaitingLevel; }

    /// @brief The handshake the server sent, valid once one has arrived. Its
    /// `level` is what a deferred client builds its world from.
    [[nodiscard]] const ServerHello &Handshake() const { return _handshake; }

    /// @brief Answer the handshake: the local world is built and NetIds may now
    /// be mapped onto it. No-op unless awaiting.
    void ConfirmLevelReady();

    /// @brief Give up on a deferred join, recording @p reason for the UI. The
    /// client stays unsynchronized; the caller tears the session down.
    void AbortJoin(std::string reason);

    /// @brief Handle one received message.
    void HandleMessage(std::span<const std::byte> payload);

    /// @brief Send this tick's input window. Call once per fixed step, after
    /// sampling.
    void SendInput(const InputCommandBuffer &buffer);

    /// @brief Whether the handshake succeeded and snapshots are being applied.
    [[nodiscard]] bool IsSynchronized() const { return _synchronized; }

    /// @brief True once the server has confirmed we hold its whole world.
    ///
    /// Distinct from IsSynchronized(), which only means the handshake worked.
    /// A joining client's initial world arrives over as many snapshots as the
    /// byte budget needs, and it cannot tell a small world from the first page
    /// of a large one — so this comes from the server. Use it to hold a loading
    /// screen, or to decide when a mirrored scene is safe to render.
    [[nodiscard]] bool IsWorldComplete() const { return _worldComplete; }

    /// @brief Still receiving the initial world: synchronized but not complete.
    [[nodiscard]] bool IsJoining() const { return _synchronized && !_worldComplete; }

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

    /// @brief Bumped every time an applied snapshot changed the *shape* of the
    /// mirrored world — an entity spawned or despawned, a component added or
    /// removed, or component data written.
    ///
    /// The hook a presentation layer needs and cannot derive: a mirror arrives
    /// carrying a `MeshRenderer` whose asset ids are authored data and whose
    /// resolved GPU pointers are null, and nothing else in the frame loop knows
    /// to re-resolve it — which is precisely why the first live two-editor test
    /// replicated a world that drew nothing. Compare against the value you last
    /// acted on; re-resolve when it moves.
    [[nodiscard]] std::uint64_t StructureRevision() const { return _structureRevision; }

    /// @brief The server's advertised timing, valid once synchronized.
    [[nodiscard]] std::uint32_t ServerTickRateHz() const { return _tickRateHz; }
    [[nodiscard]] std::uint32_t ServerSnapshotHz() const { return _snapshotHz; }

    /// @brief Write interpolated transforms into the scene for the render
    /// moment @p serverTimeTicks (fractional server ticks).
    ///
    /// Snapshots arrive at 20-30 Hz while frames render at whatever the display
    /// does, so showing the latest snapshot directly would step remote entities
    /// at the snapshot rate — visible as stutter no amount of frame rate fixes.
    /// Instead the client renders slightly in the *past*, between the two
    /// snapshots straddling `serverTime - interpolationDelay`, which is smooth
    /// as long as the buffer holds. The cost is exactly that delay in remote
    /// positions, which is why it is kept to about two snapshot intervals
    /// rather than made generous.
    ///
    /// Call once per frame, after applying whatever arrived. Extrapolation is
    /// deliberately not attempted: when the buffer runs dry the last known pose
    /// is held, because a guess that turns out wrong has to be corrected with a
    /// visible snap, and holding still reads better than snapping.
    void Interpolate(double serverTimeTicks);

    /// @brief How far behind server time to render, in ticks. Defaults to two
    /// snapshot intervals, set from the server's advertised rate at handshake.
    [[nodiscard]] double InterpolationDelayTicks() const { return _interpolationDelayTicks; }
    void                 SetInterpolationDelayTicks(double ticks) { _interpolationDelayTicks = ticks; }

    /// @brief The render time to pass to Interpolate(), given an estimate of
    /// current server time: simply that estimate minus the delay.
    [[nodiscard]] double RenderTimeFor(double estimatedServerTick) const
    {
        return estimatedServerTick - _interpolationDelayTicks;
    }

    /// @brief Write this frame's rendered pose for every mirror: interpolate the
    /// ones with no local simulation, and decay the visual offset of the ones
    /// that have it.
    ///
    /// Call once per rendered frame, **after** the physics writeback and before
    /// transforms are propagated. The order is not incidental: the writeback
    /// overwrites a bodied mirror's Transform from its physics pose, so an offset
    /// applied before it is simply erased.
    ///
    /// Two timelines coexist here, and that is the design rather than an
    /// oversight. A bodied mirror renders at server-time-minus-transit, because
    /// it is being simulated locally; a non-bodied one renders ~two snapshot
    /// intervals in the past, because interpolating between received samples is
    /// the only honest thing to do with state nobody is simulating. The visible
    /// consequence — a non-bodied entity the server moves to track a bodied one
    /// lags it — is inherent to running both.
    void SmoothView(double serverTimeTicks);

    /// @brief Corrections applied, and the divergence they found.
    ///
    /// `divergence` is measured *before* the snap, between the client's own
    /// simulated pose and the authoritative one — i.e. how far the two
    /// simulations had drifted apart in the interval since the last correction.
    /// It is the number the correction cadence has to be justified by; there is
    /// no determinism argument available to justify it instead (§3.1).
    struct CorrectionStats
    {
        std::uint64_t applied      = 0;
        std::uint64_t bytesApplied = 0;
        double        divergenceSum = 0.0;
        float         divergenceMax = 0.f;
        float         divergenceMean() const
        {
            return applied == 0 ? 0.f : static_cast<float>(divergenceSum / static_cast<double>(applied));
        }
    };

    [[nodiscard]] const CorrectionStats &Corrections() const { return _corrections; }

    /// @brief Ask the server to re-anchor this client from the empty baseline.
    ///
    /// For the case the protocol cannot see and a human can: something is
    /// visibly wrong on screen and nobody wants to wait out a sweep to find out
    /// whether it heals. Costs one over-full snapshot.
    void RequestKeyframe();

    /// @brief Re-assert the server's sleep verdict on every mirror it applies to.
    /// Call once per fixed step, immediately after the local physics step.
    ///
    /// The local wake-cascade is why this exists. Client-side poses differ from
    /// the server's by whatever the last correction has not yet removed, so a
    /// settling pile can produce contacts the server never had — and Jolt wakes
    /// bodies by island, so one spurious local contact wakes a mirror the server
    /// will never speak of again. Left alone that body drifts forever on nobody's
    /// authority.
    ///
    /// The legitimate wake path is the server's: a correction with `asleep =
    /// false` activates the body, and until one arrives the mirror holds still.
    /// That costs one transit time of hesitation, which is the trade.
    void EnforceSleep();

    /// @brief Mirrors that were destroyed locally and had to be recreated when
    /// the server next mentioned them.
    ///
    /// Counted rather than silent, because from the gameplay chair "Destroy
    /// didn't destroy" is spooky, and a client-side cleanup system running over
    /// mirrors (a kill-Z volume, a timed despawner) would otherwise produce a
    /// quiet destroy/respawn churn loop with no signal anywhere.
    [[nodiscard]] std::uint64_t MirrorsResurrected() const { return _mirrorsResurrected; }

    /// @brief Drop every replicated entity and forget the session. v1's
    /// reconnect is a full rejoin, which starts here.
    void Reset();

  private:
    bool ApplySnapshot(Core::BitReader &reader);
    void ApplyBodyState(const BodyState &state);
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

    /// One entity's pose at one server tick. Only the transform is buffered:
    /// it is the only component whose value between two snapshots is
    /// meaningfully *interpolatable* — a health value or a state enum has no
    /// halfway point worth showing.
    struct TransformSample
    {
        std::uint64_t serverTick = 0;
        glm::vec3     position{};
        glm::quat     rotation{1.f, 0.f, 0.f, 0.f};
        glm::vec3     scale{1.f, 1.f, 1.f};
    };

    /// Capture the current pose of every mirrored entity at @p serverTick.
    /// Taken after a snapshot is applied and before Interpolate() has had a
    /// chance to overwrite anything, so the buffer only ever holds
    /// authoritative poses rather than previously interpolated ones.
    void CaptureTransforms(std::uint64_t serverTick);

    /// The last authoritative verdict about one mirrored body.
    ///
    /// The rest pose is kept, not just the bit: enforcing sleep means putting the
    /// body back where the server left it, and by the time a spurious local
    /// contact has woken it, where it *is* is no longer that.
    struct MirrorBody
    {
        Physics::RigidBody body;   ///< Kept so the body can be removed after its entity is gone.
        bool               asleep = false;
        glm::vec3          restPosition{};
        glm::quat          restRotation{1.f, 0.f, 0.f, 0.f};

        /// The visual offset that hides the last correction. Added on top of the
        /// physics writeback's pose every rendered frame and decayed toward
        /// zero, so the *simulation* is always honest and the *screen* is always
        /// smooth — two different jobs that a single blended pose cannot do.
        glm::vec3 positionError{0.f};
        glm::quat rotationError{1.f, 0.f, 0.f, 0.f};
    };

    /// Destroy the Jolt body behind a mirror, if it has one. The handle lives in
    /// `_bodies` rather than being read back off the entity because the two
    /// cases that need it — a despawn and a locally-destroyed mirror — have both
    /// already lost the entity by the time anyone notices.
    void DestroyMirrorBody(NetId netId);

    Net::NetTransport     &_transport;
    ECS::Scene            &_scene;
    Physics::PhysicsWorld *_physics = nullptr;
    Net::ConnectionId      _connection;

    std::unordered_map<NetId, ECS::Entity>               _entityByNetId;
    std::unordered_map<NetId, MirrorBody>                _bodies;
    std::unordered_map<NetId, std::deque<TransformSample>> _transformHistory;
    std::vector<PendingRef>                              _pendingRefs;

    /// Samples kept per entity. Three is enough to straddle the render time
    /// with one spare for a late snapshot; more only delays noticing a stall.
    static constexpr std::size_t kMaxSamples = 3;

    double _interpolationDelayTicks = 6.0; ///< Replaced at handshake from snapshotHz.

    ClockFeedback   _feedback;
    ServerHello     _handshake;
    CorrectionStats _corrections;
    std::string     _rejectMessage;

    std::uint64_t _lastAppliedTick    = 0;
    std::uint64_t _snapshotsApplied   = 0;
    std::uint64_t _snapshotsRejected  = 0;
    std::uint64_t _structureRevision  = 0;
    std::uint64_t _mirrorsResurrected = 0;
    std::uint32_t _tickRateHz         = 60;
    std::uint32_t _snapshotHz         = 20;
    bool          _synchronized       = false;
    bool          _worldComplete      = false;
    bool          _deferHandshake     = false;
    bool          _awaitingLevel      = false;
};

} // namespace Assisi::NetSync
