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
#include <Assisi/Core/Reflect/ComponentMask.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/Net/NetTransport.hpp>
#include <Assisi/NetSync/BodyState.hpp>
#include <Assisi/NetSync/InputCommand.hpp>
#include <Assisi/NetSync/MessageDispatch.hpp>
#include <Assisi/NetSync/NetClock.hpp>
#include <Assisi/NetSync/NetProtocol.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

#include <typeindex>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Assisi::NetSync
{

/// @brief Which relevancy provider a game wants, and how to tune it.
///
/// Deliberately **not** part of the protocol hash, by the same argument that
/// keeps `neverReplicate` out of it: this changes what is *sent*, never how
/// bytes *decode*. A client applies whatever arrives regardless of its own
/// settings, so two builds differing only here pair fine and the server's
/// numbers govern. Hashing it would make a tuning edit refuse connections for
/// no correctness gain.
struct RelevancyConfig
{
    /// @brief What decides who is told about what.
    enum class Provider : std::uint8_t
    {
        /// Everyone is told about everything, with the intersection skipped
        /// outright. The default, and free.
        All = 0,
        /// A radius around each connection's view anchors, with hysteresis.
        Distance = 1,
    };

    Provider provider = Provider::All;

    /// @brief Metres at which an entity *enters* a connection's set.
    float radius = 60.f;

    /// @brief Metres at which it leaves again. Must exceed `radius`.
    ///
    /// The gap is not a refinement, it is the mechanism: with one radius, an
    /// entity sitting on the boundary converts into a despawn and a full
    /// respawn on every crossing, which is the one failure mode every surveyed
    /// system either engineered around or suffered. A value at or below
    /// `radius` is corrected at load with a warning.
    float exitRadius = 75.f;

    /// @brief Ticks an entity must stay beyond `exitRadius` before the revoke
    /// takes effect. Belt to the hysteresis braces.
    ///
    /// **Gates revokes only.** Entering is immediate, so an anchor teleport — a
    /// level transition, a spectator jump — shows the world at once instead of
    /// after a fraction of a second of emptiness. Symmetric dwell is the
    /// reflex to resist here.
    std::uint32_t dwellTicks = 30;
};

/// @brief Read the `networking.relevancy` object of a game config.
///
/// Absent key yields the default (everything relevant, costing nothing) and a
/// malformed one warns and yields the same — the contract every other loader in
/// this file follows, and the safe direction: a config typo must not silently
/// *narrow* what a game sends.
[[nodiscard]] RelevancyConfig LoadRelevancyFromConfig(std::string_view configPath = "game.json");

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

    /// Ceiling on intents of *one message type* accepted per connection per
    /// second. Per type so a flood of one cannot starve another.
    ///
    /// Generous by default: an intent is a deliberate act, and the shapes that
    /// legitimately repeat — map pings, look-here markers — still do not repeat
    /// sixty times a second. Checked before the payload is decoded, so exceeding
    /// it costs a comparison.
    std::uint32_t maxIntentsPerTypePerSecond = 60;

    /// How far behind the server an intent's client tick may be before it is
    /// dropped as stale, and how far ahead before it is dropped as impossible.
    ///
    /// Load-bearing for *unreliable* intents, where out-of-order arrival is
    /// normal: without a window, a map ping that arrived late would time-travel
    /// into a world that has moved on. The input queue's stale-drop is the same
    /// idea one layer down.
    std::uint32_t intentStaleWindowTicks = 120; ///< Two seconds at 60 Hz.
    std::uint32_t intentLeadWindowTicks  = 60;  ///< One second of client lead.

    /// Bytes the message section may run *past* maxSnapshotBytes by, when
    /// something is waiting to go out.
    ///
    /// An allowance rather than a reservation, and the difference is the whole
    /// point. Holding bytes back up front does not work: the entity and body
    /// passes stop once they are already at the budget, and the last entity
    /// written can overshoot by more than any reservation — so the floor would
    /// evaporate exactly when the world is busiest, which is when events most
    /// need it. maxSnapshotBytes is a soft cap already; overrunning it by a
    /// bounded amount, only when a connection has events pending, is what
    /// actually guarantees a full world cannot permanently starve them.
    std::size_t reservedEventBytes = 96;

    /// How many unreliable events one connection may have waiting for the
    /// entities they are about. Past this, the oldest is dropped and counted.
    ///
    /// A queue that grows without bound is a connection whose events are about
    /// entities it will never be told about, which is a bug in the game's
    /// scoping rather than a condition to absorb. The cap makes it visible in
    /// bytes instead of in memory.
    std::size_t maxHeldEventsPerConnection = 64;

    /// Bounds a command must satisfy before the simulation sees it.
    InputLimits inputLimits;

    /// Component type names this *game* never replicates, whatever the engine
    /// says they are capable of.
    ///
    /// The third gate, between the type's capability and each entity's own
    /// exclusion mask, and the one that answers the incident this design came
    /// from: an engine module marks something replicable to serve one use, and a
    /// game that does not want it should be able to say so **once**, rather than
    /// on every entity that happens to carry it.
    ///
    /// Names rather than ids, because ids are alphabetical and dense and would
    /// rot the moment any component is added. Names that resolve to nothing warn
    /// and are ignored.
    ///
    /// Deliberately **not** part of the protocol hash. Quantization is hashed
    /// because it changes how bytes *decode* — a mismatch there is silent
    /// corruption, the one thing a handshake exists to prevent — while this only
    /// changes which self-describing blocks are *sent*. A client applies whatever
    /// arrives regardless of its own list, so two builds differing only here pair
    /// fine and the server's list governs. Hashing it would make a config edit
    /// refuse connections for no correctness gain.
    ///
    /// Filled by whoever owns the session (from game.json), never read from the
    /// filesystem by the server itself — so a test can set it without a file.
    std::vector<std::string> neverReplicate;

    /// Who is told about what. Defaults to everything, which costs nothing; a
    /// server whose provider is `Distance` installs one at construction.
    ///
    /// Filled by the session-owning layer from game.json, on the same terms as
    /// `neverReplicate` — the server never reads the filesystem itself.
    RelevancyConfig relevancy;
};

/// @brief Read the `networking.neverReplicate` array of a game config.
///
/// Absent key yields an empty list and a malformed one warns and yields empty —
/// the same contract as the quantization loader. Call at session start; the
/// value goes into ReplicationConfig::neverReplicate.
///
/// Per session rather than per process (unlike quantization, which is fixed at
/// startup because it is inside the handshake hash): editing this and hosting
/// again takes effect, which is safe precisely because it is not hashed.
[[nodiscard]] std::vector<std::string> LoadNeverReplicateFromConfig(std::string_view configPath = "game.json");

class ReplicationServer;

/// @brief One connection's situation, handed to a relevancy provider once per
/// snapshot.
///
/// Everything a provider could reasonably need and nothing it could use to
/// reach back into the protocol: it answers a question, it does not participate
/// in sending.
struct RelevancyQuery
{
    /// Who is asking. Providers that key policy off the participant — a
    /// spectator seeing more than a player — read this.
    ClientId client;

    /// The entities this connection views the world from. Usually one; empty
    /// for a connection that has not been given any, which is the fail-open
    /// case a distance provider must handle by returning everything.
    std::span<const ECS::Entity> anchors;

    /// Every replicated entity, sorted ascending. The candidate set, and the
    /// only NetIds a provider may legitimately name.
    std::span<const NetId> live;

    /// Read-only scene access, for providers that measure something about
    /// entities. Never null.
    const ECS::Scene *scene = nullptr;

    /// For `EntityOf` / `NetIdOf`. Never null.
    const ReplicationServer *server = nullptr;

    /// The tick this snapshot is being built at, for providers that keep
    /// per-pair state over time (hysteresis dwell counters, for instance).
    std::uint64_t simTick = 0;
};

/// @brief Decides which entities one connection is told about at all.
///
/// The pluggable half of relevancy: the engine owns the *set* and the guarantee
/// that follows from it — an entity outside a connection's set contributes zero
/// bytes to that connection — while the provider owns the policy that fills it.
/// That split is what makes a game-side information boundary (line of sight,
/// fog of war) a provider rather than an engine feature: the guarantee is what
/// makes the provider sufficient.
///
/// **No provider is the default, and it is not a provider.** A server with none
/// tells every connection about everything, skipping the intersection entirely,
/// so the mechanism costs exactly nothing when unused. A provider that returns
/// the live set verbatim produces byte-identical output — pinned by test — and
/// exists only as the reference the zero-cost path is measured against.
///
/// Providers run server-side at snapshot cadence. The engine consumes the set,
/// never the method: a provider free to compute it event-driven, from a spatial
/// grid, or from a game-specific visibility structure is free to do so.
class RelevancyProvider
{
  public:
    virtual ~RelevancyProvider() = default;

    RelevancyProvider()                                     = default;
    RelevancyProvider(const RelevancyProvider &)            = delete;
    RelevancyProvider &operator=(const RelevancyProvider &) = delete;

    /// @brief Fill @p out with the NetIds @p query's connection should be told
    /// about, **sorted ascending and without duplicates**.
    ///
    /// @p out arrives empty. Naming a NetId that is not live is harmless — the
    /// engine intersects with the live set regardless — and naming none is a
    /// complete answer meaning "this connection sees nothing", which explicit
    /// grants and the implicit controlled-entity grant may still add to.
    virtual void Compute(const RelevancyQuery &query, std::vector<NetId> &out) = 0;

    /// @brief Forget whatever per-pair state @p client accumulated. Called when
    /// a connection leaves, so a provider keeping dwell counters or last-known
    /// distances does not leak them for the life of the session.
    virtual void ForgetClient(ClientId client) { (void)client; }
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

    /// Entities this connection is currently told about. Equal to the live
    /// count when nothing filters; the number a radius is actually buying
    /// otherwise.
    std::uint32_t relevantEntities   = 0;

    /// Entities that have entered and left this connection's set over the
    /// session. Totals rather than rates, like everything else here — a rate
    /// needs a clock and a window, and whoever draws this has both.
    ///
    /// Worth surfacing because boundary thrash is invisible otherwise: it looks
    /// like ordinary bandwidth, and it is the failure mode hysteresis exists to
    /// prevent. Enters climbing in lockstep with exits is the shape to watch
    /// for.
    std::uint64_t relevancyEnters    = 0;
    std::uint64_t relevancyExits     = 0;

    // ── Intents ──────────────────────────────────────────────────────────────
    // One counter per way an intent can be refused, because "intents dropped"
    // is not a diagnosis. A rate-limited client is misbehaving, a stale one has
    // a clock problem, an out-of-range one is lying or mismatched, and an
    // unhandled one means somebody forgot to write a handler — four different
    // conversations.

    /// Intents that made it to a handler.
    std::uint64_t intentsAccepted    = 0;
    /// Rejected because the type is an event, which a client may not speak.
    std::uint64_t intentsWrongWay    = 0;
    /// Rejected by the per-type rate limit, before any payload work.
    std::uint64_t intentsRateLimited = 0;
    /// Rejected because the client's tick was outside the accepted window.
    std::uint64_t intentsStale       = 0;
    /// Rejected because a field was outside its declared range — the client is
    /// lying, or the two builds disagree. Never clamped: see FieldsWithinBounds.
    std::uint64_t intentsOutOfRange  = 0;
    /// Rejected because the sender does not control the entity it named.
    /// Reachable by an honest client during a control transfer, which is why it
    /// is counted rather than treated as an attack.
    std::uint64_t intentsNotYours    = 0;
    /// Dropped because nothing handles the type. Normal — the sender's build
    /// may care about something this one does not.
    std::uint64_t intentsUnhandled   = 0;
    /// Dropped because the bytes did not decode: truncated, malformed, or an id
    /// this build has never heard of.
    std::uint64_t intentsMalformed   = 0;

    // ── Events ───────────────────────────────────────────────────────────────

    /// Unreliable events written into this connection's snapshot sections.
    std::uint64_t eventsSent         = 0;
    /// Reliable announcements sent on the control lane. Worth its own number:
    /// the rule is that these are rare, and a rule nobody can see is a rule
    /// nobody keeps.
    std::uint64_t announcementsSent  = 0;
    /// Events waiting for the entity they are about to arrive. A number that
    /// climbs and stays high means the game is talking about entities this
    /// connection will never be told about.
    std::uint32_t eventsHeld         = 0;
    /// Held events dropped because their subject despawned before it ever
    /// arrived — the event is now about nothing.
    std::uint64_t eventsEvicted      = 0;
    /// Held events dropped because the queue hit its cap, oldest first.
    std::uint64_t eventsOverflowed   = 0;
    /// Directed events with nobody to direct them at: an uncontrolled entity,
    /// or a client that has left.
    std::uint64_t eventsUndeliverable = 0;
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
    ///
    /// Allocates this connection's `ClientId` — monotonic from
    /// kFirstRemoteClientId, never reused within the session — and carries it in
    /// the hello. Allocation is at *assignment*; the id becomes meaningful when
    /// the handshake completes, which is a distinction with no observable
    /// consequence beyond a gap in the numbering if a joiner is rejected.
    void AddConnection(Net::ConnectionId connection);

    /// @brief Tell the server which content set it is running, so it can check a
    /// joiner's against it.
    ///
    /// **Until this is called, no `ServerHello` goes out.** A connection is still
    /// registered — it just waits. That is the literal reading of "the server
    /// cannot be reached without one" (docs/blueprint-system-concept.md §9), and
    /// the alternative is worse than it looks: `ClientHello` is sent exactly once
    /// and never resent, so a server that received one while its own hash was
    /// pending could neither verify it nor safely drop it.
    ///
    /// Hashing is a job, so this normally lands a frame or two after hosting
    /// starts. Setting it flushes every hello that was waiting.
    void SetContentSetHash(std::uint64_t hash);

    /// @brief Whether SetContentSetHash has been called. Hosting is not reachable
    /// until it has.
    [[nodiscard]] bool HasContentSetHash() const { return _contentSetHashReady; }

    /// @brief Forget a connection. Its NetIds stay allocated — they belong to
    /// the entities, not to whoever was watching them.
    ///
    /// What does *not* stay is whatever that client controlled: each entity in
    /// its control set is despawned or loses its `ControlledBy`, per that
    /// component's own `despawnOnDisconnect`. The sweep runs **before** the
    /// connection's bookkeeping is erased, because it needs the leaving
    /// client's id to know what to sweep.
    void RemoveConnection(Net::ConnectionId connection);

    /// @brief This connection's session identity, or InvalidClientId if it is
    /// not one of ours.
    [[nodiscard]] ClientId ClientIdOf(Net::ConnectionId connection) const;

    /// @brief Inverse of ClientIdOf. `Net::InvalidConnection` for the host
    /// (which is not a connection) and for ids that have left.
    [[nodiscard]] Net::ConnectionId ConnectionOf(ClientId client) const;

    /// @brief Give @p client control of @p entity, replacing whatever held it.
    ///
    /// The one way control is established: a component write on the server,
    /// replicated to every client like any other component. Transfer is this
    /// same call with a different id — one write, one propagation delay, rather
    /// than the five simultaneous semantic changes a fused ownership pointer
    /// makes of it.
    ///
    /// @param despawnOnDisconnect What happens to @p entity when @p client
    ///   leaves. True — despawn — is right for a player-spawned pawn; false for
    ///   a world object someone is temporarily driving.
    ///
    /// Passing InvalidClientId is the same as ClearControl(). An entity that
    /// does not replicate can still be given control (nothing forbids it), but
    /// no client will ever hear about it.
    void SetControl(ECS::Entity entity, ClientId client, bool despawnOnDisconnect = true);

    /// @brief Remove @p entity's `ControlledBy`, if it has one. The entity
    /// survives; only the claim on it ends.
    void ClearControl(ECS::Entity entity);

    /// @brief Who controls @p entity, or InvalidClientId.
    [[nodiscard]] ClientId ControllerOf(ECS::Entity entity) const;

    /// @brief Install the provider that decides who is told about what, or
    /// null to tell everyone about everything.
    ///
    /// Null is the default and is *not* a provider that returns everything: the
    /// intersection is skipped outright, so relevancy costs nothing at all in
    /// the games that do not use it. See RelevancyProvider.
    void SetRelevancyProvider(std::unique_ptr<RelevancyProvider> provider);

    [[nodiscard]] RelevancyProvider *Relevancy() const { return _relevancy.get(); }

    /// @brief Set the entities @p connection views the world from.
    ///
    /// Session state, not a component, and deliberately *not* derived from
    /// `ControlledBy` at the point of use. A v1 joiner is a spectator with no
    /// controlled entity and still needs a viewpoint; Unreal's anchor is the
    /// view target rather than the pawn, and spectator and camera actors are
    /// exactly where owner-derived anchoring leaks. The pawn itself never
    /// depends on anchors to stay visible to its controller — that is the
    /// implicit grant's job.
    ///
    /// Passing an empty list restores the default, which is the connection's
    /// controlled entities.
    void SetViewAnchors(Net::ConnectionId connection, std::span<const ECS::Entity> anchors);

    /// @brief The anchors in effect for @p connection: whatever was set, or its
    /// controlled entities if nothing was.
    [[nodiscard]] std::span<const ECS::Entity> ViewAnchors(Net::ConnectionId connection) const;

    /// @brief Pin @p netId into @p connection's set regardless of what the
    /// provider says. Idempotent.
    ///
    /// The escape hatch every surveyed system ships in some form: spectator
    /// tooling, quest markers, an entity a game wants one particular player to
    /// keep seeing. Merged *after* the provider, so a grant always wins.
    void GrantRelevance(Net::ConnectionId connection, NetId netId);

    /// @brief Undo a GrantRelevance. The entity may still be relevant for
    /// another reason — the provider, or the implicit grant below.
    void RevokeRelevance(Net::ConnectionId connection, NetId netId);

    /// @brief Whether @p netId was in @p connection's set as of the last
    /// snapshot. False for connections we do not have.
    [[nodiscard]] bool IsRelevant(Net::ConnectionId connection, NetId netId) const;

    /// @brief @p connection's set as of the last snapshot, sorted. Empty for a
    /// connection that has not been sent one yet; the whole live set when no
    /// provider is installed.
    [[nodiscard]] std::span<const NetId> RelevantSet(Net::ConnectionId connection) const;

    /// @brief Every entity @p client controls, in NetId-agnostic scene order.
    ///
    /// Served from an index rebuilt once per tick in ReconcileNetIds rather
    /// than maintained incrementally: the editor's play/stop restore and
    /// undo-revive both resurrect entities outside any incremental hook, and an
    /// index that misses those is an index that is wrong exactly when someone
    /// is debugging.
    [[nodiscard]] std::span<const ECS::Entity> ControlledEntities(ClientId client) const;

    /// @brief Handle one received message. Everything that arrives from a client
    /// goes through here, and everything here treats its input as hostile.
    void HandleMessage(Net::ConnectionId connection, std::span<const std::byte> payload);

    /// @brief Advance to @p simTick: reconcile the NetId map with the scene, and
    /// send a snapshot to every ready connection if this tick is a snapshot tick.
    void Tick(std::uint64_t simTick);

    /// @brief Take the command a connection's queue holds for @p tick, or null.
    /// Call once per connection per tick, from the simulation.
    const InputCommand *ConsumeInput(Net::ConnectionId connection, std::uint64_t tick);

    /// @brief Submit an intent from the *host's own* player.
    ///
    /// A listen server's player is not a connection — there is no loopback
    /// client by design (NetSession.hpp) — so without this the person hosting
    /// would be the one participant who cannot speak. It enters the same
    /// dispatch site as a remote intent, with sender = HostClientId, and passes
    /// the same checks minus the transport framing there is none of. One door
    /// means one, including for the host.
    template <typename T>
    void SubmitLocalIntent(const T &intent)
    {
        static_assert(Core::Reflect::MessageTraits<T>::direction == Core::Reflect::MessageDirection::Intent,
                      "SubmitLocalIntent takes an AMSG(intent, ...). An event is the authority speaking, "
                      "and the host is the authority — send it, do not submit it.");
        DispatchLocalIntent(&intent, typeid(T));
    }

    // ── Sending events ───────────────────────────────────────────────────────
    // Three recipient classes, and no fourth. Membership of each is *computed*
    // from relevancy and control, never enumerated by gameplay code: an
    // arbitrary per-call connection list is the API through which an event leaks
    // exactly what state filtering withholds, and it duplicates in every call
    // site the recipient computation relevancy already owns in one place.
    //
    // Delivery form comes from the type, not the call: an `AMSG(event,
    // unreliable)` rides the next snapshot, where its ordering against the
    // entity it names is free; an `AMSG(event, reliable)` goes out immediately
    // on the control lane with a tick stamp the client defers against.

    /// @brief To everyone who can see the entity this event is about.
    ///
    /// The default, and structural: the section is built per connection
    /// alongside that connection's entity blocks, so "who can see it" is
    /// already computed. An `independent` event, naming no entity, goes to
    /// every ready connection instead.
    ///
    /// The host is included — the authority sees everything — through a local
    /// queue dispatched at the end of its own tick.
    template <typename T>
    void Send(const T &event)
    {
        static_assert(Core::Reflect::MessageTraits<T>::direction == Core::Reflect::MessageDirection::Event,
                      "A server sends AMSG(event, ...). An intent is a request, and the server does not "
                      "make requests of itself.");
        SendEvent(&event, typeid(T), Recipients::AllRelevant, InvalidClientId);
    }

    /// @brief To exactly one client: whoever controls @p entity.
    ///
    /// Dropped and counted when nobody does — an uncontrolled entity has no
    /// controller to address, and guessing would mean picking someone.
    template <typename T>
    void SendToController(ECS::Entity entity, const T &event)
    {
        static_assert(Core::Reflect::MessageTraits<T>::direction == Core::Reflect::MessageDirection::Event,
                      "A server sends AMSG(event, ...).");
        SendEvent(&event, typeid(T), Recipients::Directed, ControllerOf(entity));
    }

    /// @brief To exactly one client, named directly. For session-level events
    /// with no entity involved.
    template <typename T>
    void SendTo(ClientId client, const T &event)
    {
        static_assert(Core::Reflect::MessageTraits<T>::direction == Core::Reflect::MessageDirection::Event,
                      "A server sends AMSG(event, ...).");
        SendEvent(&event, typeid(T), Recipients::Directed, client);
    }

    /// @brief To everyone who can see it, except @p instigator.
    ///
    /// For events the instigator has already shown itself locally — the
    /// `COND_SkipOwner` pattern every system has. The host can be the excluded
    /// instigator like anyone else.
    template <typename T>
    void SendExcept(ClientId instigator, const T &event)
    {
        static_assert(Core::Reflect::MessageTraits<T>::direction == Core::Reflect::MessageDirection::Event,
                      "A server sends AMSG(event, ...).");
        SendEvent(&event, typeid(T), Recipients::ExceptInstigator, instigator);
    }

    /// @brief The scene the session is bound to. Handlers reach the world
    /// through the context rather than through a global.
    [[nodiscard]] ECS::Scene &Scene() { return _scene; }

    /// @brief The session this server belongs to, for handler contexts. Set by
    /// NetSession at construction; null in the tests that drive the server
    /// directly, which is why every consumer treats it as optional.
    void SetOwningSession(NetSession *session) { _session = session; }

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

    /// @brief Counters for the host's own submissions.
    ///
    /// The host has no connection and therefore no ConnectionDiagnostics, which
    /// would make its intents the one traffic nobody could see. Only the intent
    /// counters are meaningful here — there is no snapshot to send itself.
    [[nodiscard]] const ConnectionDiagnostics &HostDiagnostics() const { return _hostDiagnostics; }

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
        /// Assigned at AddConnection, monotonic, never reused. What
        /// `ControlledBy` names and what directed messages address.
        ClientId              clientId;
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

        /// The effective set as of the last snapshot: `live ∩ R(c)`, sorted.
        ///
        /// Kept rather than recomputed because it is what "did this entity just
        /// re-enter?" is asked against — the question whose wrong answer builds
        /// a corrupt half-mirror (see the re-entry rule in SendSnapshot).
        /// Untouched, and unread, while no provider is installed.
        std::vector<NetId> relevant;

        /// Entities pinned into this connection's set by GrantRelevance,
        /// sorted. Merged after the provider, so a grant always wins.
        std::vector<NetId> grants;

        /// What this connection views the world from, when the session has
        /// said. Empty means "use whatever it controls" — see SetViewAnchors on
        /// why the default is not the *definition*.
        std::vector<ECS::Entity> anchors;

        /// Scratch holding the anchors actually handed to the provider this
        /// snapshot: `anchors` if non-empty, the controlled set otherwise.
        std::vector<ECS::Entity> anchorScratch;

        /// Scratch for the per-snapshot set algebra, kept so a filtering server
        /// does not allocate twice per connection per snapshot.
        std::vector<NetId> effectiveScratch;
        std::vector<NetId> mergeScratch;

        InputCommandQueue     input;
        ConnectionDiagnostics diagnostics;

        /// Sliding one-second window for the input rate limit.
        std::uint64_t rateWindowTick   = 0;
        std::uint32_t packetsInWindow  = 0;

        /// One unreliable event waiting for the next snapshot section.
        struct PendingEvent
        {
            /// The entity this event is about, or InvalidNetId for an
            /// `independent` one. What relevancy scoped it by, and what it
            /// waits for.
            NetId subject = InvalidNetId;

            /// The encoded message block, id and length prefix included.
            /// Encoded once server-side and copied per recipient — entity
            /// references translate to NetIds identically for everyone, so
            /// there is nothing per-connection about the bytes.
            std::vector<std::byte> bytes;
        };

        /// Events queued for this connection's next snapshot.
        ///
        /// **Held, not dropped.** An event about an entity the connection does
        /// not hold yet — a spawn the byte budget cut, a mid-join page — waits
        /// until that entity lands. That is the body-state gate's rule with the
        /// opposite resolution, and deliberately so: a state can wait for the
        /// next tick because the next tick restates it, while an event cannot
        /// be regenerated.
        std::deque<PendingEvent> pendingEvents;

        /// The same window, per *message type*, for intents.
        ///
        /// Per type rather than one bucket for all of them: a client spamming
        /// map pings must not be able to squeeze out its own weapon-fire
        /// intents, and a game that adds a chatty message type must not
        /// silently shrink the budget of every existing one. Unreal retrofitted
        /// FRPCDoSDetection onto a design that had neither; the shape is known
        /// in advance here.
        std::unordered_map<Core::Reflect::MessageId, std::uint32_t> intentsInWindow;
        std::uint64_t intentWindowTick = 0;
    };

    void SendHello(Connection &connection);
    void SendReject(Connection &connection, RejectReason reason);
    void SendSnapshot(Connection &connection);

    /// This connection's `live ∩ R(c)`, computed once per snapshot and used by
    /// every downstream pass.
    ///
    /// Returns `_liveNetIds` itself when no provider is installed — not a copy,
    /// not an intersection, the same object today's code already walks. That
    /// identity is the performance-first contract, and it is pinned by a test
    /// that compares wire bytes against an identity-filter run.
    const std::vector<NetId> &ComputeEffective(Connection &connection);

    /// Make @p netId's next appearance a full state rather than a delta, by
    /// forgetting that @p connection ever had it.
    ///
    /// The revoke → re-grant-within-one-round-trip case. The server's acked set
    /// still lists the entity, so the ordinary path would send a *delta*; but
    /// the client destroyed its mirror when the despawn landed and would build a
    /// fresh entity out of whatever partial block that delta happened to carry.
    /// Forgetting is the fix, and it must reach the in-flight ring too — a late
    /// ack for a pre-revoke snapshot would otherwise restore the entity to the
    /// acked set and resurrect exactly the bug.
    static void ForgetAcked(Connection &connection, NetId netId);

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

    /// The single validated door every intent comes through, in the order the
    /// steps have to be in: envelope, direction, rate, staleness, decode, range,
    /// control, dispatch.
    ///
    /// The ordering is not cosmetic. Rate limiting precedes decoding so a flood
    /// costs a comparison instead of a parse; the direction check precedes
    /// everything expensive because the vocabulary itself already says a client
    /// cannot speak events; and range validation follows decoding because it is
    /// the first step that needs a value.
    void HandleIntent(Connection &connection, Core::BitReader &reader);

    /// The tail shared by a remote and a host-local intent: decode, validate,
    /// dispatch. Everything before it is transport.
    void DispatchIntent(ClientId sender, ConnectionDiagnostics &diagnostics,
                        const Core::Reflect::MessageMeta &meta, Core::BitReader &reader);

    /// Who an event goes to. Three classes, computed rather than enumerated.
    enum class Recipients : std::uint8_t
    {
        AllRelevant,      ///< Everyone whose set contains the subject.
        Directed,         ///< One named client.
        ExceptInstigator, ///< All-relevant, minus one.
    };

    /// The type-erased half of Send/SendTo/SendToController/SendExcept.
    void SendEvent(const void *event, std::type_index type, Recipients recipients, ClientId who);

    /// Bytes the message section may run past the snapshot's soft cap by: zero
    /// when nothing is waiting, the configured floor otherwise.
    [[nodiscard]] std::size_t EventFloorBytes(const Connection &connection) const;

    /// This entity's NetId, assigning one if it replicates and has none yet.
    ///
    /// The lazy assignment in ReconcileNetIds happens once per tick, which
    /// leaves a window every frame where a just-spawned entity has no wire
    /// identity — and "spawn it and announce it" is the common case, not an
    /// exotic one. Returns InvalidNetId for anything that does not replicate.
    NetId EnsureNetId(ECS::Entity entity);

    /// Whether @p connection should receive an event scoped to @p subject.
    [[nodiscard]] bool EventReaches(const Connection &connection, NetId subject) const;

    /// Queue an already-encoded unreliable event for one connection's next
    /// snapshot section, evicting the oldest if the queue is over its cap.
    void QueueEvent(Connection &connection, NetId subject, std::vector<std::byte> bytes);

    /// Write the snapshot's message section: every pending event whose subject
    /// this connection already holds, or which this same packet just delivered.
    void WriteEventSection(Connection &connection, Core::BitWriter &writer, const SentSnapshot &record);

    /// Encode and re-decode the host's own intent so it travels the identical
    /// path a remote one does.
    ///
    /// A round trip through the codec for a message that never leaves the
    /// process looks wasteful, and it is — deliberately. The alternative is a
    /// second dispatch path that skips decoding, and a second path is exactly
    /// what "one validated door" is a promise against: the host's intents would
    /// be the ones no fuzz test ever covered.
    void DispatchLocalIntent(const void *intent, std::type_index type);

    /// Assign NetIds to newly-replicated entities and drop mappings for entities
    /// that are gone. Run once per tick, before any snapshot is built, so every
    /// connection sees the same world.
    void ReconcileNetIds();

    /// Rebuild the client → controlled-entities index from the scene.
    ///
    /// Rebuilt rather than maintained, for the reason ControlledEntities()
    /// gives: entities come back from the dead through paths no incremental
    /// hook sees. Cheap by construction — the index is one entry per
    /// *controlled* entity, which is roughly one per player.
    void RebuildControlIndex();

    /// Strip every `ControlledBy` the scene was loaded with. Control is
    /// session-scoped state assigned at runtime; a level file carrying it is
    /// carrying a claim from a session that ended. See the component's own
    /// header comment.
    void StripAuthoredControl();

    /// This entity's authored exclusion policy, or an empty mask if it has no
    /// marker. Read live — see `Replicated::excluded` for why nothing caches it.
    [[nodiscard]] Core::Reflect::ComponentMask ExclusionMaskOf(ECS::Entity entity) const;

    /// Whether this entity's motion travels as *body state* rather than as an
    /// ordinary replicated Transform.
    ///
    /// One predicate, consulted by all four places that used to ask the question
    /// separately (the capture, the body-state write, and the Transform
    /// suppression in the component write). They must agree: an entity the
    /// capture treats as bodied but the component pass does not would have its
    /// Transform suppressed *and* no body state sent — a mirror frozen at its
    /// load pose, which is the worst of both paths.
    ///
    /// False in four cases, and the last two are policy:
    ///  - no physics world, or no RigidBody — nothing to observe;
    ///  - an authored-static descriptor, whose pose is authored data and travels
    ///    as a Transform (docs/replication-plan-v4.md);
    ///  - the descriptor is *excluded*, so the client will never build a body to
    ///    correct — the visual-only mirror of D6;
    ///  - the Transform is excluded on a bodied entity, so the client could
    ///    never build a body even if it wanted to (both build paths need a
    ///    Transform), and sending body state it must drop would be pure waste.
    [[nodiscard]] bool ReplicatesAsBody(ECS::Entity entity, const Core::Reflect::ComponentMask &excluded) const;

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
    ///
    /// @p effective is this connection's relevancy set, and it is not optional:
    /// this pass is a *fourth* independent walk of the live set, and its own
    /// gate is acked-based. Left to itself it would keep shipping body state for
    /// an entity that has left the set, every tick, until the despawn acks —
    /// which is the zero-bytes guarantee failing exactly during the window it
    /// most needs to hold.
    void WriteBodyStates(Connection &connection, const std::vector<NetId> &effective, Core::BitWriter &writer,
                         SentSnapshot &record, std::size_t writtenFromComponents);

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
    NetSession            *_session = nullptr; ///< For handler contexts; null in direct-drive tests.
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

    /// ClientId → the connection carrying it. The host has no entry: it is not
    /// a connection, which is the whole reason the two id spaces are separate.
    std::unordered_map<std::uint32_t, Net::ConnectionId> _connectionByClient;

    /// ClientId → the entities it controls. Rebuilt each ReconcileNetIds.
    std::unordered_map<std::uint32_t, std::vector<ECS::Entity>> _controlledByClient;

    /// The next id a joining connection gets. Starts past the reserved values
    /// and only ever climbs — see ClientId on why reuse is not worth its
    /// ambiguity.
    std::uint32_t _nextClientId = kFirstRemoteClientId;

    /// This host's content set, and whether it is known yet. Until it is, a
    /// connection is registered but its ServerHello is withheld — see
    /// SetContentSetHash.
    std::uint64_t _contentSetHash      = 0;
    bool          _contentSetHashReady = false;

    /// Who decides what each connection is told about. Null — the default —
    /// means everyone is told everything, on today's exact code path.
    std::unique_ptr<RelevancyProvider> _relevancy;

    /// Scratch for one provider's output, reused across connections within a
    /// snapshot tick.
    std::vector<NetId> _providerScratch;

    /// Where the host's own intent counters go — see HostDiagnostics().
    ConnectionDiagnostics _hostDiagnostics;

    /// Events addressed to the host's own player, waiting for the end of the
    /// tick.
    ///
    /// The host has no connection, so nothing would otherwise deliver to it —
    /// which would make chat, this design's own example, invisible to the
    /// person hosting. Dispatched after every state mutation for the tick, so a
    /// handler sees a world at least as new as the message, which is the
    /// property a remote client gets free from packet ordering.
    std::vector<std::pair<Core::Reflect::MessageId, std::vector<std::byte>>> _hostEvents;

    /// Drain _hostEvents through the same handler path a client uses.
    void DispatchHostEvents();

    /// The escape classes, resolved once per tick in ReconcileNetIds rather
    /// than per connection: both lists are tiny (an entity opting out of the
    /// provider is by definition unusual), and evaluating them per connection
    /// would mean walking the whole live set for each one — which is the cost
    /// relevancy exists to avoid.
    std::vector<NetId> _alwaysRelevant; ///< Relevance::Always, sorted.

    /// Relevance::ControllerOnly, sorted by NetId, paired with the client that
    /// may see it (0 while uncontrolled, which means nobody may).
    std::vector<std::pair<NetId, std::uint32_t>> _controllerOnly;

    std::unordered_map<NetId, ECS::Entity>      _entityByNetId;
    std::unordered_map<std::uint64_t, NetId>    _netIdByEntity; ///< Keyed by the entity's packed handle.
    std::vector<NetId>                          _liveNetIds;    ///< Sorted; rebuilt each ReconcileNetIds.

    /// Raw counter, not a NetId — see ECS::InstanceTable::_nextId for why: the
    /// type is opaque everywhere except the two places that turn a count into an
    /// identity, EnsureNetId and ReconcileNetIds.
    std::uint32_t _nextNetId = 1;

    std::uint64_t _simTick     = 0;
    std::uint64_t _snapshotDiv = 3; ///< tickRateHz / snapshotHz, at least 1.

    /// Component ids this server *may* send: every ACOMP(replicable) type,
    /// resolved once. Capability, not policy — what an individual entity
    /// actually sends is this set minus its own `Replicated::excluded`.
    /// Cached because it is walked per entity per snapshot.
    std::vector<Core::Reflect::ComponentId> _replicatedComponents;

    /// Replicable ordinal of each entry above, in the same order — the bit index
    /// to test in an entity's exclusion mask. Resolved alongside the ids rather
    /// than looked up per component per entity per snapshot.
    ///
    /// Stored explicitly rather than assumed equal to the index: both sequences
    /// happen to be in ascending id order today, so they coincide, but the game
    /// filter removes entries from *this* list only — and a coincidence that
    /// silently becomes false would misaim every exclusion at once.
    std::vector<std::size_t> _replicatedOrdinals;

    /// Resolved once, so the per-entity write loop can suppress the Transform of
    /// a bodied entity without a registry lookup per component per entity.
    Core::Reflect::ComponentId _transformComponentId = Core::Reflect::kInvalidComponentId;

    /// Exclusion-mask bit indices for the two components whose absence changes
    /// how an entity replicates at all, resolved once for the same reason.
    std::size_t _transformOrdinal  = Core::Reflect::ComponentRegistry::kInvalidOrdinal;
    std::size_t _descriptorOrdinal = Core::Reflect::ComponentRegistry::kInvalidOrdinal;
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

    /// @brief The content set this build holds, which the server checks against
    /// its own before letting the join complete.
    ///
    /// **The `ClientHello` is withheld until this is set**, for the same reason the
    /// server withholds its own hello: the hello is sent exactly once and never
    /// resent, so sending one with a placeholder hash is a refused join that no
    /// retry can fix. Computing it is a job, so a client that connects before its
    /// scan finishes joins in appearance only and completes when the hash arrives.
    void SetContentSetHash(std::uint64_t hash);

    /// @brief Whether the content-set hash has been supplied.
    [[nodiscard]] bool HasContentSetHash() const { return _contentSetHashReady; }

    /// @brief Answer the handshake: the local world is built and NetIds may now
    /// be mapped onto it. No-op unless awaiting.
    ///
    /// Two preconditions, not one — the world *and* the hash. A caller that has
    /// only just finished loading should pass the hash here; one that had it
    /// earlier can call SetContentSetHash and then this with no argument.
    void ConfirmLevelReady();
    void ConfirmLevelReady(std::uint64_t contentSetHash);

    /// @brief Give up on a deferred join, recording @p reason for the UI. The
    /// client stays unsynchronized; the caller tears the session down.
    void AbortJoin(std::string reason);

    /// @brief Handle one received message.
    void HandleMessage(std::span<const std::byte> payload);

    /// @brief Send this tick's input window. Call once per fixed step, after
    /// sampling.
    void SendInput(const InputCommandBuffer &buffer);

    /// @brief Ask the server for something. The only thing a client may say
    /// besides its input and its acks.
    ///
    /// One call shape for every intent: the destination is implicit (there is
    /// nowhere else to send one) and the reliability comes from the type's own
    /// declaration, so a `reliable` intent must arrive and an `unreliable` one
    /// is fire-and-forget. Both are equally *untrusted* — reliability is about
    /// delivery, not about belief.
    ///
    /// Sending an event fails to compile: the direction is part of the type.
    ///
    /// @param clientTick The tick this intent is about, normally the clock's
    ///   command tick. The server drops intents outside its accepted window,
    ///   which is what keeps a late unreliable one from time-travelling.
    template <typename T>
    bool SendIntent(const T &intent, std::uint64_t clientTick)
    {
        static_assert(Core::Reflect::MessageTraits<T>::direction == Core::Reflect::MessageDirection::Intent,
                      "A client may only send AMSG(intent, ...). An event is the authority's word about "
                      "what happened, and a client is not the authority.");
        return SendIntentBytes(&intent, typeid(T), clientTick,
                               Core::Reflect::MessageTraits<T>::reliability ==
                                   Core::Reflect::MessageReliability::Reliable);
    }

    /// @brief Whether the handshake succeeded and snapshots are being applied.
    [[nodiscard]] bool IsSynchronized() const { return _synchronized; }

    /// @brief Who this client is, per the server's hello. InvalidClientId until
    /// one has arrived.
    ///
    /// The value to compare a mirror's `ControlledBy::client` against to answer
    /// "is this one mine?" — which is what input binding, prediction, and every
    /// bit of local UI that says "you" will ask.
    [[nodiscard]] ClientId LocalClientId() const { return _handshake.clientId; }

    /// @brief Whether @p entity is a mirror this client controls. False for
    /// anything uncontrolled, anything someone else controls, and everything
    /// before the handshake.
    [[nodiscard]] bool ControlsEntity(ECS::Entity entity) const;

    /// @brief The session this client belongs to, for handler contexts.
    void SetOwningSession(NetSession *session) { _session = session; }

    /// @brief Events delivered to a handler so far.
    [[nodiscard]] std::uint64_t EventsDispatched() const { return _eventsDispatched; }

    /// @brief Events dropped because nothing handles their type. Normal — the
    /// server's build may care about something this one does not — but visible.
    [[nodiscard]] std::uint64_t EventsUnhandled() const { return _eventsUnhandled; }

    /// @brief Announcements waiting for the world to catch up to them.
    [[nodiscard]] std::size_t DeferredAnnouncementCount() const { return _deferredAnnouncements.size(); }

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

    /// @brief Inverse of EntityOf: the wire identity of a local mirror, or
    /// InvalidNetId if this entity is not one.
    ///
    /// A linear scan, deliberately — the only caller is an inspector asking
    /// about one selected entity, and a second map maintained for that would
    /// cost more to keep correct than it saves.
    [[nodiscard]] NetId NetIdOf(ECS::Entity entity) const;

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
    /// @param dt Seconds since the last call. The offset decays in *time*, not
    ///   per frame — a constant per frame is a different time constant at every
    ///   refresh rate, so the same build would feel different on a 60 Hz and a
    ///   144 Hz display for no reason visible in the code.
    void SmoothView(double serverTimeTicks, float dt);

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

    /// Bring one mirror's Jolt body in line with the components just applied to
    /// it: build it if the descriptor has arrived and it has none, and — for
    /// authored-static geometry, whose pose travels as a Transform rather than
    /// as body state — move it to wherever that Transform now says.
    ///
    /// Without the first half a static mirror would never get a collider at all
    /// (nothing sends body state for it, and body state is what builds bodies),
    /// so the client's own dynamic bodies would fall straight through a wall the
    /// host can see them resting on.
    void SyncMirrorBody(NetId netId, ECS::Entity entity);
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

    /// The type-erased half of SendIntent, so the template stays a two-line
    /// static_assert and everything that needs a .cpp lives in one.
    bool SendIntentBytes(const void *intent, std::type_index type, std::uint64_t clientTick, bool reliable);

    /// Read and dispatch the snapshot's message section.
    ///
    /// Called *after* the packet's entity blocks and body states have been
    /// applied, which is the whole ordering guarantee: a handler for an event
    /// about an entity spawned in this same packet finds that entity already
    /// there.
    bool ApplyEventSection(Core::BitReader &reader);

    /// One reliable announcement, held until the world is new enough for it.
    struct DeferredAnnouncement
    {
        Core::Reflect::MessageId messageId = Core::Reflect::kInvalidMessageId;
        std::uint64_t            serverTick = 0; ///< Applied tick this needs before it means anything.
        NetId                    subject    = InvalidNetId;
        std::vector<std::byte>   bytes;
    };

    void HandleAnnouncement(Core::BitReader &reader);

    /// Dispatch every announcement whose moment has arrived.
    ///
    /// The control lane is not the snapshot lane, so an announcement can and
    /// does overtake the state it is about. Holding it until the applied tick
    /// reaches its stamp *and* the entity it names exists is the client-side
    /// half of the ordering the snapshot section gets from framing — needed
    /// only here, and only because these two things travel separately.
    void DrainAnnouncements();

    /// Decode one message body and hand it to its handler. Shared by the
    /// snapshot section and the announcement path so both translate NetIds the
    /// same way and both count the same drops.
    void DispatchEvent(const Core::Reflect::MessageMeta &meta, Core::BitReader &reader);

    std::vector<DeferredAnnouncement> _deferredAnnouncements;
    std::uint64_t                     _eventsDispatched = 0;
    std::uint64_t                     _eventsUnhandled  = 0;

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

        /// The visual offset that hides the last correction, as it stands this
        /// frame. Added on top of the physics writeback's pose every rendered
        /// frame, so the *simulation* is always honest and the *screen* is always
        /// smooth — two different jobs that a single blended pose cannot do.
        glm::vec3 positionError{0.f};
        glm::quat rotationError{1.f, 0.f, 0.f, 0.f};

        /// ...and what drives it to zero: the offset as it was when the
        /// correction landed, plus how far through its convergence window we
        /// are. Linear in that window, so the offset is gone by the deadline and
        /// travels at a *constant* on-screen speed while it lasts.
        ///
        /// A multiplicative decay was tried first and is the wrong shape twice
        /// over: it is frame-rate dependent, and it spends most of the offset in
        /// the first frame or two, which is the pop it exists to avoid. Unreal's
        /// CharacterMovementComponent smooths linearly over
        /// `NetworkSimulatedSmoothLocationTime` for the same reason.
        glm::vec3 positionErrorStart{0.f};
        glm::quat rotationErrorStart{1.f, 0.f, 0.f, 0.f};
        float     smoothingElapsed = 0.f;
        float     smoothingWindow  = 0.f; ///< Seconds; 0 = nothing to smooth.
    };

    /// Destroy the Jolt body behind a mirror, if it has one. The handle lives in
    /// `_bodies` rather than being read back off the entity because the two
    /// cases that need it — a despawn and a locally-destroyed mirror — have both
    /// already lost the entity by the time anyone notices.
    void DestroyMirrorBody(NetId netId);

    Net::NetTransport     &_transport;
    ECS::Scene            &_scene;
    Physics::PhysicsWorld *_physics = nullptr;
    NetSession            *_session = nullptr; ///< For handler contexts; null in direct-drive tests.
    Net::ConnectionId      _connection;

    /// Resolved once. A descriptor *removal* is the one component removal with a
    /// side effect — the mirror stops being body-corrected, so its Jolt body and
    /// the transient RigidBody handle must go too — and the removal path sees
    /// only ids, never types.
    Core::Reflect::ComponentId _descriptorComponentId = Core::Reflect::kInvalidComponentId;
    Core::Reflect::ComponentId _rigidBodyComponentId  = Core::Reflect::kInvalidComponentId;

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

    /// The content-set hash and whether it has been supplied. A separate flag
    /// rather than a sentinel because 0 is a legitimate hash — an empty content
    /// set — and "not yet" must not be confusable with "nothing to hash".
    std::uint64_t _contentSetHash      = 0;
    bool          _contentSetHashReady = false;
    /// True once ConfirmLevelReady has been told the world is built, so a hash
    /// arriving afterwards can complete the join on its own.
    bool _levelReady = false;
};

} // namespace Assisi::NetSync
