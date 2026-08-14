/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ReplicationServer.hpp
/// @brief The sending half of the state-replication protocol.
///
/// Sender and receiver are one design: a change to either one's wire handling
/// is a change to both.

#include <Assisi/NetSync/ReplicationConfig.hpp>
#include <Assisi/NetSync/ReplicationProviders.hpp>
#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/InstanceId.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Core/BiMap.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/ComponentMask.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/Net/NetTransport.hpp>
#include <Assisi/NetSync/BodyState.hpp>
#include <Assisi/NetSync/InputCommand.hpp>
#include <Assisi/NetSync/MessageDispatch.hpp>
#include <Assisi/NetSync/NetProtocol.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

#include <typeindex>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace Assisi::NetSync
{

/// @brief The authoritative half. Owns NetId assignment and snapshot sending.
///
/// Drive it from the fixed-step loop: Poll() at the top of the tick to take
/// input and acks, the simulation in between, then Tick() at the end to send
/// what changed.
class ReplicationServer
{
public:
    /// @param physics The world whose bodies are the authority on motion. Null
    ///   means Transforms replicate as ordinary components and no body state is
    ///   sent at all.
    ReplicationServer(Net::NetTransport &transport, ECS::Scene &scene, Physics::PhysicsWorld *physics = nullptr,
                      ReplicationConfig config = {});

    ReplicationServer(const ReplicationServer &)            = delete;
    ReplicationServer &operator=(const ReplicationServer &) = delete;

    /// @brief Declare which level this server is running.
    ///
    /// Carried in every subsequent `ServerHello`, so **set it before the first
    /// connection arrives**. Left unset, the server advertises
    /// `LevelAddressing::None` and a joining editor aborts rather than join a
    /// world whose static half it cannot build.
    void SetLevelIdentity(LevelIdentity level) { _level = std::move(level); }

    [[nodiscard]] const LevelIdentity &Level() const { return _level; }

    /// @brief Register a connection the transport has reported as Connected,
    /// allocate its `ClientId`, and send the handshake carrying it.
    ///
    /// The client is not eligible for snapshots until it answers with a matching
    /// protocol hash. ClientIds are monotonic from kFirstRemoteClientId and never
    /// reused within a session, so a rejected joiner leaves a gap.
    void AddConnection(Net::ConnectionId connection);

    /// @brief Tell the server which content set it is running, so it can check a
    /// joiner's against it. Flushes every hello that was waiting.
    ///
    /// **Until this is called, no `ServerHello` goes out** — connections are
    /// registered and simply wait. Holding them is required, not tidy:
    /// `ClientHello` is sent exactly once and never resent, so a server that
    /// received one while its own hash was pending could neither verify it nor
    /// safely drop it. Hashing is a job, so this normally lands a frame or two
    /// after hosting starts.
    void SetContentSetHash(std::uint64_t hash);

    /// @brief Whether SetContentSetHash has been called. Hosting is not reachable
    /// until it has.
    [[nodiscard]] bool HasContentSetHash() const { return _contentSetHashReady; }

    /// @brief Forget a connection, and despawn or release everything it
    /// controlled per each `ControlledBy`'s own `despawnOnDisconnect`.
    ///
    /// NetIds stay allocated — they belong to the entities, not to whoever was
    /// watching them. The control sweep must run **before** the connection's
    /// bookkeeping is erased; it needs the leaving client's id to know what to
    /// sweep.
    void RemoveConnection(Net::ConnectionId connection);

    /// @brief This connection's session identity, or InvalidClientId if it is
    /// not one of ours.
    [[nodiscard]] ClientId ClientIdOf(Net::ConnectionId connection) const;

    /// @brief Inverse of ClientIdOf. `Net::InvalidConnection` for the host
    /// (which is not a connection) and for ids that have left.
    [[nodiscard]] Net::ConnectionId ConnectionOf(ClientId client) const;

    /// @brief Give @p client control of @p entity, replacing whatever held it.
    ///
    /// The one way control is established: a `ControlledBy` write on the server,
    /// replicated like any other component. Transfer is this same call with a
    /// different id.
    ///
    /// @param despawnOnDisconnect What happens to @p entity when @p client
    ///   leaves. True — despawn — is right for a player-spawned pawn; false for
    ///   a world object someone is temporarily driving.
    ///
    /// Passing InvalidClientId is the same as ClearControl(). A non-replicating
    /// entity can be given control, but no client will ever hear about it.
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
    /// intersection is skipped outright, so relevancy costs nothing in games
    /// that do not use it. See RelevancyProvider.
    void SetRelevancyProvider(std::unique_ptr<RelevancyProvider> provider);

    [[nodiscard]] RelevancyProvider *Relevancy() const { return _relevancy.get(); }

    /// @brief Install the provider that describes blueprint instances, or null
    /// to replicate every member as an ordinary entity.
    ///
    /// Null is the default. Installing one changes how NetIds are handed out —
    /// an instance's members take a contiguous block — so **install it before
    /// the first entity replicates, never mid-session**: assigned ids are never
    /// reissued, and a half-blocked instance would have no base to send.
    void SetInstanceInfoProvider(std::unique_ptr<InstanceInfoProvider> provider);

    /// Not named InstanceInfo(): a member function of that name would shadow the
    /// struct of that name inside this class.
    [[nodiscard]] InstanceInfoProvider *Instances() const { return _instanceInfo.get(); }

    /// @brief Set the entities @p connection views the world from.
    ///
    /// Session state, not a component, and deliberately *not* derived from
    /// `ControlledBy`: a spectator has no controlled entity and still needs a
    /// viewpoint. A pawn never depends on anchors to stay visible to its own
    /// controller — that is the implicit grant's job.
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
    /// The escape hatch for spectator tooling, quest markers, and anything one
    /// player must keep seeing. Merged *after* the provider, so a grant always
    /// wins.
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
    /// Served from an index rebuilt once per tick in ReconcileNetIds rather than
    /// maintained incrementally: the editor's play/stop restore and undo-revive
    /// both resurrect entities outside any incremental hook.
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
    /// client by design (NetSession.hpp) — so without this the host would be the
    /// one participant that cannot speak. Enters the same dispatch site as a
    /// remote intent with sender = HostClientId, and passes the same checks
    /// minus the transport framing there is none of.
    template <typename T>
    void SubmitLocalIntent(const T &intent)
    {
        static_assert(Core::Reflect::MessageTraits<T>::direction == Core::Reflect::MessageDirection::Intent,
                      "SubmitLocalIntent takes an AMSG(intent, ...). An event is the authority speaking, "
                      "and the host is the authority — send it, do not submit it.");
        DispatchLocalIntent(&intent, typeid(T));
    }

    // ── Sending events ───────────────────────────────────────────────────────
    // Three recipient classes, and no fourth. Membership is *computed* from
    // relevancy and control, never enumerated by gameplay code — an arbitrary
    // per-call connection list is how an event leaks exactly what state
    // filtering withholds.
    //
    // Delivery form comes from the type, not the call: an `AMSG(event,
    // unreliable)` rides the next snapshot, where its ordering against the
    // entity it names is free; an `AMSG(event, reliable)` goes out immediately
    // on the control lane with a tick stamp the client defers against.

    /// @brief To everyone who can see the entity this event is about.
    ///
    /// The default. An `independent` event, naming no entity, goes to every
    /// ready connection instead. The host is included, through a local queue
    /// dispatched at the end of its own tick.
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
    /// For events the instigator has already shown itself locally. The host can
    /// be the excluded instigator like anyone else.
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

    /// @brief Instances currently holding a NetId block.
    ///
    /// A gauge, not a total: a block is retired with its last member, so this
    /// counts the live instances rather than every one the session has seen. It
    /// tracks the world; climbing while the world does not is a leak.
    [[nodiscard]] std::size_t InstanceBlockCount() const { return _instanceBlocks.size(); }

    /// @brief Whether the connection completed its handshake and is receiving
    /// snapshots.
    [[nodiscard]] bool IsReady(Net::ConnectionId connection) const;

    [[nodiscard]] const ConnectionDiagnostics *Diagnostics(Net::ConnectionId connection) const;

    /// @brief Counters for the host's own submissions.
    ///
    /// The host has no connection and therefore no ConnectionDiagnostics of its
    /// own. Only the intent counters are meaningful here — there is no snapshot
    /// to send itself.
    [[nodiscard]] const ConnectionDiagnostics &HostDiagnostics() const { return _hostDiagnostics; }

    /// @brief True when @p simTick is one the config says to send state on.
    [[nodiscard]] bool IsSnapshotTick(std::uint64_t simTick) const;

    [[nodiscard]] const ReplicationConfig &Config() const { return _config; }

private:
    /// How much of one entity a connection is known to have.
    ///
    /// Per entity, never one tick per connection: an entity the byte budget
    /// skipped is still listed in the in-flight snapshot, so a single global tick
    /// would declare its pending changes delivered on ack and never send them.
    /// Per entity, the skipped one simply keeps its old baseline.
    struct EntityBaseline
    {
        std::uint64_t componentTick = 0; ///< Component state delivered up to here.
        std::uint64_t bodyTick      = 0; ///< Body state delivered up to here.
    };

    /// What one entity was written at inside one snapshot. Only entities the
    /// snapshot *actually wrote* get an entry — that is the point.
    struct WrittenEntity
    {
        NetId netId = InvalidNetId;
        EntityBaseline ticks;
    };

    /// One in-flight snapshot's worth of "what the client would know if it acks
    /// this". The entity set is what makes spawn and despawn fall out of the
    /// same comparison, and the per-entity ticks are the delta baselines.
    struct SentSnapshot
    {
        std::uint64_t serverTick = 0;
        std::vector<WrittenEntity> written; ///< Sorted ascending by netId.
        std::vector<NetId>         netIds;  ///< Sorted ascending.

        /// Which components each of those entities had, as sorted
        /// `(netId << 32) | componentId` pairs.
        ///
        /// Change detection stamps writes, not removals — nothing in the ECS
        /// reports "this component is gone" — so removal is found the same way
        /// despawn is, by comparing what the client is known to have against
        /// what exists now. Unlike a fixed bitmask this has no ceiling on how
        /// many component types the game may register.
        std::vector<std::uint64_t> components;

        /// The instances this connection is known to have been told about as of
        /// this snapshot — cumulative, like `netIds`, not the delta that went
        /// out in it. An instance whose members all stopped being relevant drops
        /// out, so re-entry resends the record rather than leaving the client to
        /// compose members against something it has forgotten.
        std::vector<ECS::InstanceId> instances;
    };

    struct Connection
    {
        Net::ConnectionId id    = Net::InvalidConnection;
        /// Assigned at AddConnection, monotonic, never reused. What
        /// `ControlledBy` names and what directed messages address.
        ClientId clientId;
        bool ready = false;                  ///< Handshake completed.
        std::deque<SentSnapshot> inFlight;

        /// The acked baseline: the entity set the client is known to have, the
        /// components those entities had, and — per entity — how far its state
        /// has been delivered.
        std::vector<NetId>         acked;
        std::vector<std::uint64_t> ackedComponents;
        std::uint64_t ackedTick = 0;

        /// Instances this connection has acked a record for, sorted. A record is
        /// resent every snapshot until it lands here: a member block the client
        /// cannot attribute to an instance is a corrupt mirror.
        std::vector<ECS::InstanceId> knownInstances;

        /// One entry per entity this connection has acked. **Erased when its
        /// despawn acks** — NetIds are never reused, so without that this grows
        /// with every entity that has *ever* replicated, unbounded under
        /// projectile-style churn.
        std::unordered_map<NetId, EntityBaseline> baselines;

        /// Send priority, per entity. Each snapshot tick every entity with
        /// something to send gains `max(Replicated::priority, kMinPriorityGain)`;
        /// entities drain highest-first into the byte budget, and **only the
        /// drained reset**, so the ones that missed keep climbing and cannot
        /// starve.
        ///
        /// Inert when the budget is not binding: everything dirty goes every
        /// tick and every accumulator resets. Under pressure it degrades
        /// correction *frequency* per object, steered by the authored number.
        std::unordered_map<NetId, float> priority;

        /// The effective set as of the last snapshot: `live ∩ R(c)`, sorted.
        ///
        /// Kept rather than recomputed because it is what "did this entity just
        /// re-enter?" is asked against — see the re-entry rule in
        /// ComputeEffective, whose wrong answer builds a corrupt half-mirror.
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

        InputCommandQueue input;
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
            /// Encoded once and copied per recipient — entity references
            /// translate to NetIds identically for everyone, so there is
            /// nothing per-connection about the bytes.
            std::vector<std::byte> bytes;
        };

        /// Events queued for this connection's next snapshot.
        ///
        /// **Held, not dropped.** An event about an entity the connection does
        /// not hold yet — a spawn the byte budget cut, a mid-join page — waits
        /// until that entity lands. The body-state gate resolves the same
        /// situation the opposite way, deliberately: a state can wait because
        /// the next tick restates it, while an event cannot be regenerated.
        std::deque<PendingEvent> pendingEvents;

        /// The same window, per *message type*, for intents.
        ///
        /// Per type rather than one bucket for all of them: a client spamming
        /// map pings must not squeeze out its own weapon-fire intents, and
        /// adding a chatty message type must not shrink every existing one's
        /// budget.
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
    /// not an intersection. A test pins that the wire bytes match an
    /// identity-filter run, so keep the no-provider path allocation-free.
    const std::vector<NetId> &ComputeEffective(Connection &connection);

    /// Drop from @p ids every Relevance::ControllerOnly entity @p connection does
    /// not control. Order-preserving, so a sorted set stays sorted.
    ///
    /// Called twice per computation, and the second call is the point (B6): the
    /// first pass runs before block escalation so a withheld member cannot pull
    /// its own block, and escalation then re-adds every member of any block a
    /// surviving sibling belongs to — including the ones this had just removed.
    /// A privacy class that a sibling's relevance can undo is not one.
    void ApplyControllerOnly(const Connection &connection, std::vector<NetId> &ids) const;

    /// Make @p netId's next appearance a full state rather than a delta, by
    /// forgetting that @p connection ever had it.
    ///
    /// For revoke → re-grant within one round trip. The acked set still lists
    /// the entity, so the ordinary path would send a *delta*, while the client
    /// destroyed its mirror when the despawn landed and would rebuild it out of
    /// whatever partial block that delta carried.
    ///
    /// **Must scrub the in-flight ring too**: `HandleAck` installs a record's
    /// sets wholesale, so a late ack for a pre-revoke snapshot would put the
    /// entity back into the acked set with its old baseline.
    static void ForgetAcked(Connection &connection, NetId netId);

    /// The same forgetting one level up: make @p instanceId's record go out
    /// again by dropping it from everything that says @p connection has it,
    /// in-flight ring included.
    ///
    /// Four cumulative sets travel with a connection — acked entities, their
    /// components, the written baselines, and the instances. ForgetAcked
    /// scrubs the first three; leave the fourth alone and a late ack reinstates
    /// `knownInstances`, no fresh instance is computed, and the members land as
    /// bare mirrors permanently.
    static void ForgetAckedInstance(Connection &connection, ECS::InstanceId instanceId);

    /// Re-anchor @p connection from the empty baseline: forget every per-entity
    /// tick, and clear the in-flight ring with them.
    ///
    /// The ring clear is not tidiness: an ack for a pre-sweep snapshot arriving
    /// after the sweep would fold that record's per-entity ticks back in and
    /// silently cancel the re-anchor. Cleared, a late ack finds no record and is
    /// ignored, at the cost of one over-full resend.
    ///
    /// The acked entity and component *sets* are deliberately untouched: this
    /// resets what the client has *seen*, not what it *holds*, and clearing the
    /// sets would turn every entity into a spawn and break despawn detection.
    static void ResetBaselines(Connection &connection);
    void HandleClientHello(Connection &connection, Core::BitReader &reader);
    void HandleAck(Connection &connection, Core::BitReader &reader);
    void HandleInput(Connection &connection, Core::BitReader &reader);

    /// The single validated door every intent comes through, in the order the
    /// steps **have to** be in: envelope, direction, rate, staleness, decode,
    /// range, control, dispatch.
    ///
    /// Rate limiting precedes decoding so a flood costs a comparison instead of
    /// a parse; the direction check precedes everything expensive; range
    /// validation follows decoding because it is the first step that needs a
    /// value.
    void HandleIntent(Connection &connection, Core::BitReader &reader);

    /// The tail shared by a remote and a host-local intent: decode, validate,
    /// dispatch. Everything before it is transport.
    void DispatchIntent(ClientId sender, ConnectionDiagnostics &diagnostics,
                        const Core::Reflect::MessageMeta &meta, Core::BitReader &reader);

    /// Whether an entity with no NetId yet gets one while it is being encoded.
    enum class IdAssignment : std::uint8_t
    {
        Existing, ///< Look up only; something unidentified encodes as zero.
        OnDemand, ///< Assign one — for "spawn it and announce it in one frame".
    };

    /// @brief The hooks every server-side *encode* installs, in one place.
    ///
    /// Both reference kinds or neither: an entity handle and a blueprint instance
    /// id are equally per-machine, and a codec site that remembers one and
    /// forgets the other writes a number naming the sender's world. The hooks
    /// live here rather than at each site, so no site can install half the set.
    ///
    /// @param assignment Whether an unidentified entity is given a NetId. Events
    ///                   want OnDemand — announcing something spawned this frame
    ///                   would otherwise encode "nothing" for the very entity the
    ///                   message is about. Everything else looks up.
    [[nodiscard]] Core::Reflect::CodecContext EncodeContext(IdAssignment assignment);

    /// @brief The hooks every server-side *decode* installs — the inverse of
    /// EncodeContext, and installed for the same reason.
    ///
    /// An id that names nothing here resolves to the null entity or to
    /// InstanceId{0} rather than to whatever local object happens to wear that
    /// number.
    [[nodiscard]] Core::Reflect::CodecContext DecodeContext();

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
    /// ReconcileNetIds assigns lazily once per tick, leaving a window every
    /// frame where a just-spawned entity has no wire identity — and "spawn it
    /// and announce it" is the common case. Returns InvalidNetId for anything
    /// that does not replicate.
    NetId EnsureNetId(ECS::Entity entity);

    /// Whether @p connection should receive an event scoped to @p subject.
    [[nodiscard]] bool EventReaches(const Connection &connection, NetId subject, bool independent) const;

    /// Queue an already-encoded unreliable event for one connection's next
    /// snapshot section, evicting the oldest if the queue is over its cap.
    void QueueEvent(Connection &connection, NetId subject, std::vector<std::byte> bytes);

    /// Write the snapshot's message section: every pending event whose subject
    /// this connection already holds, or which this same packet just delivered.
    void WriteEventSection(Connection &connection, Core::BitWriter &writer, const SentSnapshot &record);

    /// Encode and re-decode the host's own intent so it travels the identical
    /// path a remote one does.
    ///
    /// The round trip is deliberate waste. The alternative is a second dispatch
    /// path that skips decoding — and the host's intents would be the ones no
    /// fuzz test ever covers.
    void DispatchLocalIntent(const void *intent, std::type_index type);

    /// Assign NetIds to newly-replicated entities and drop mappings for entities
    /// that are gone. Run once per tick, before any snapshot is built, so every
    /// connection sees the same world.
    void ReconcileNetIds();

    /// Rebuild the client → controlled-entities index from the scene.
    ///
    /// Rebuilt rather than maintained, for the reason ControlledEntities()
    /// gives. Cheap by construction: one entry per *controlled* entity, which is
    /// roughly one per player.
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
    /// The one predicate for this question, shared by the capture, the
    /// body-state write, and the Transform suppression in the component write.
    /// **They must agree**: an entity the capture treats as bodied but the
    /// component pass does not gets its Transform suppressed *and* no body state
    /// sent — a mirror frozen at its load pose.
    ///
    /// False in four cases, the last two by policy:
    ///  - no physics world, or no RigidBody — nothing to observe;
    ///  - an authored-static descriptor, whose pose is authored data and travels
    ///    as a Transform (docs/replication-plan-v4.md);
    ///  - the descriptor is *excluded*, so the client will never build a body to
    ///    correct — a visual-only mirror;
    ///  - the Transform is excluded on a bodied entity, so the client could
    ///    never build a body even if it wanted to (both build paths need a
    ///    Transform), and sending body state it must drop is pure waste.
    [[nodiscard]] bool ReplicatesAsBody(ECS::Entity entity, const Core::Reflect::ComponentMask &excluded) const;

    /// Refresh `_bodyStates` from the physics world. Once per tick, before any
    /// snapshot is built, for the same reason as ReconcileNetIds.
    ///
    /// Four cases:
    ///  - an awake body — recorded every tick;
    ///  - a body that just *stopped* being active — its rest state must be
    ///    recorded and its tick bumped, because the sleep transition is itself a
    ///    change and nothing later restates it;
    ///  - a resting body whose pose moved anyway — nothing wakes a body for an
    ///    editor drag or a gameplay reposition, so the active set never reports
    ///    it and the pose is polled instead;
    ///  - a NetId seen for the first time — recorded whatever its state, so
    ///    joining a world that settled before anyone connected gives sleeping
    ///    mirrors at the server's rest poses instead of a client-side re-settle.
    void CaptureBodyStates();

    /// Write the snapshot's body-state section for @p connection, recording the
    /// per-entity ticks it wrote into @p record so the ack can fold them in.
    ///
    /// @p effective is this connection's relevancy set and is **not optional**:
    /// this pass walks the live set independently and its own gate is
    /// acked-based, so without it body state keeps going out every tick for an
    /// entity that has left the set, until the despawn acks.
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

    Net::NetTransport &_transport;
    ECS::Scene &_scene;
    Physics::PhysicsWorld *_physics = nullptr;
    NetSession *_session = nullptr;            ///< For handler contexts; null in direct-drive tests.
    ReplicationConfig _config;
    LevelIdentity _level;

    /// The last state captured for a replicated body, and when.
    struct BodyRecord
    {
        BodyState state;
        std::uint64_t tick = 0; ///< 0 = never captured.
    };

    /// The priority bump a sleep transition earns.
    ///
    /// Deliberately large: every other update is superseded by the next one,
    /// while the final rest pose is the one whose delay is permanently visible —
    /// after it the server has nothing more to say about that body.
    static constexpr float kSleepTransitionBoost = 100.f;

    /// Floor on the per-tick priority gain. `Replicated::priority` is authored
    /// down to 0.0, and an unclamped gain of zero means the entity never climbs
    /// — silently starved forever under budget pressure. With the clamp, 0 means
    /// "last in line", never "never".
    static constexpr float kMinPriorityGain = 1.f / 64.f;

    /// Per-NetId body state, refreshed once per tick by CaptureBodyStates.
    std::unordered_map<NetId, BodyRecord> _bodyStates;

    /// Monotonic, bumped once per capture. Its own counter rather than the
    /// scene's change tick, which only advances when someone touches a
    /// component — and a moving body no longer does.
    std::uint64_t _bodyStateTick = 0;

    /// Scratch for CaptureBodyStates, kept so capturing does not allocate per
    /// tick.
    std::vector<Physics::PhysicsWorld::ActiveBodyState> _activeBodies;

    std::unordered_map<Net::ConnectionId, Connection> _connections;

    /// ClientId → the connection carrying it. The host has no entry: it is not
    /// a connection, which is the whole reason the two id spaces are separate.
    std::unordered_map<std::uint32_t, Net::ConnectionId> _connectionByClient;

    /// ClientId → the entities it controls. Rebuilt each ReconcileNetIds.
    std::unordered_map<std::uint32_t, std::vector<ECS::Entity>> _controlledByClient;

    /// The next id a joining connection gets. Starts past the reserved values
    /// and only ever climbs; see ClientId on why ids are never reused.
    std::uint32_t _nextClientId = kFirstRemoteClientId;

    /// This host's content set, and whether it is known yet. Until it is, a
    /// connection is registered but its ServerHello is withheld — see
    /// SetContentSetHash.
    std::uint64_t _contentSetHash      = 0;
    bool _contentSetHashReady = false;

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
    /// The host has no connection, so nothing would otherwise deliver to it.
    /// Dispatched **after** every state mutation for the tick, so a handler sees
    /// a world at least as new as the message — the property a remote client
    /// gets free from packet ordering.
    std::vector<std::pair<Core::Reflect::MessageId, std::vector<std::byte>>> _hostEvents;

    /// Drain _hostEvents through the same handler path a client uses.
    void DispatchHostEvents();

    /// The escape classes, resolved once per tick in ReconcileNetIds rather than
    /// per connection: both lists are tiny, and evaluating them per connection
    /// would mean walking the whole live set for each one.
    std::vector<NetId> _alwaysRelevant; ///< Relevance::Always, sorted.

    /// Relevance::ControllerOnly, sorted by NetId, paired with the client that
    /// may see it (0 while uncontrolled, which means nobody may).
    std::vector<std::pair<NetId, std::uint32_t>> _controllerOnly;

    /// Entity ↔ NetId, both directions. One container rather than two maps
    /// because the two directions have to be written together: a NetId whose
    /// reverse row went missing is an entity that holds an id and is skipped by
    /// every snapshot, and neither pass of ReconcileNetIds can see such a thing
    /// to repair it.
    Core::BiMap<ECS::Entity, NetId> _netIds;
    std::vector<NetId>              _liveNetIds; ///< Sorted; rebuilt each ReconcileNetIds.

    /// Raw counter, not a NetId — see ECS::InstanceTable::_nextId. Only
    /// EnsureNetId, ReconcileNetIds and EnsureInstanceBlock turn a count into an
    /// identity.
    ///
    /// An instance reserves `memberCount` ids at once, which is what makes a
    /// member's id derivable as `base + memberIndex` and lets one spawn record
    /// stand in for every member.
    NetIdValue _nextNetId = 1;

    /// The contiguous NetId range one instance's members occupy.
    struct InstanceBlock
    {
        NetId base;                      ///< Member 0's id. Member i is base + i.
        std::uint32_t memberCount = 0;
        InstanceInfo info;               ///< Captured at allocation — see below.

        /// One entry per member, non-zero while a client's own expansion of this
        /// blueprint could still stand in for it. Starts all-set and **only ever
        /// clears**: a member absent for one tick can never be derived again,
        /// since a client joining in that gap was told it did not exist and a
        /// client that had it was sent a despawn.
        ///
        /// Gates the authored-value elision, and only that. Eliding a component
        /// "the client already has from the file" is sound only while the file's
        /// copy is what the client holds; a member that was pruned and revived,
        /// or that appeared after the block was allocated, holds nothing — and
        /// the elision's own gate (`sinceChangeTick == 0 && !clientHasIt`) is
        /// true for exactly that case, so without this it arrives bare and stays
        /// bare.
        std::vector<std::uint8_t> derivable;
    };

    /// Allocated blocks, by instance. The info is captured **once**, at
    /// allocation, never re-read per snapshot: an unacked record must describe
    /// the instance as it was when its ids were handed out, or a late joiner and
    /// an early one compose the same members from different placements.
    ///
    /// Retired by `ReconcileNetIds` when the last member of an instance is gone,
    /// along with the two views below and the instance's slot in every
    /// connection. Ids are still never reused: an instance appearing again after
    /// that allocates a *new* block at a new base, which is what makes it a new
    /// instance to the client as well.
    std::unordered_map<ECS::InstanceId, InstanceBlock> _instanceBlocks;

    /// The same blocks the other way round: base NetId → the instance that owns
    /// it. Read by `instanceFromWire`, so an instance id arriving in an intent
    /// names this machine's instance rather than the sender's. Written and erased
    /// only beside the map above, so a base whose instance is gone resolves to
    /// nothing rather than to whatever holds that id next.
    std::unordered_map<NetId, ECS::InstanceId> _instanceByBase;

    /// The same blocks as `(base, memberCount)`, sorted by base, answering
    /// "which instance owns this NetId?" — which relevancy asks once per
    /// relevant entity per connection per snapshot and the map above cannot.
    ///
    /// Stays sorted by construction: `_nextNetId` only climbs, so each new block
    /// starts above every existing one and appending is enough. A retirement
    /// erases in place, which leaves the order it found.
    std::vector<std::pair<NetId, std::uint32_t>> _blockRanges;

    /// Scratch for relevancy's block escalation. A member, not a local, so a
    /// snapshot does not allocate.
    std::vector<NetId> _escalateScratch;

    std::unique_ptr<InstanceInfoProvider> _instanceInfo;

    /// @brief The NetId @p entity should take as a blueprint member, or
    /// InvalidNetId if it is not one the server can describe.
    ///
    /// Reached from both id-assignment paths, so an instance gets one block
    /// whether its first member is noticed by an event send or by the snapshot
    /// walk. InvalidNetId is the ordinary-entity fallback, not a failure.
    NetId EnsureInstanceBlock(ECS::Entity entity);

    /// @brief Bind @p entity to @p netId, or refuse and return InvalidNetId.
    ///
    /// `Core::BiMap` guarantees the two directions cannot disagree; what is
    /// decided here is the policy when they *would* — an id already held by
    /// another live entity is refused rather than stolen, and the caller goes
    /// without a wire identity this tick.
    ///
    /// The reachable case is a blueprint member destroyed and respawned inside
    /// one frame: `Scene::Destroy` is deferred to the end of the frame, so both
    /// claimants of `base + memberIndex` are briefly alive at once and nothing
    /// here can tell which one is leaving. The next tick's cleanup pass retires
    /// the one that left and the newcomer takes the id — one frame late, in its
    /// own block, rather than immediately and permanently outside it.
    [[nodiscard]] NetId BindNetId(ECS::Entity entity, NetId netId);

    std::uint64_t _simTick     = 0;
    std::uint64_t _snapshotDiv = 3; ///< tickRateHz / snapshotHz, at least 1.

    /// Component ids this server *may* send: every ACOMP(replicable) type minus
    /// the game's veto, resolved once. Capability, not policy — what an
    /// individual entity actually sends is this set minus its own
    /// `Replicated::excluded`. Cached because it is walked per entity per
    /// snapshot.
    std::vector<Core::Reflect::ComponentId> _replicatedComponents;

    /// Replicable ordinal of each entry above, in the same order — the bit index
    /// to test in an entity's exclusion mask. Resolved alongside the ids rather
    /// than looked up per component per entity per snapshot.
    ///
    /// **Never assume this equals the index.** The two happen to coincide today,
    /// but the game's veto removes entries from this list only, and the moment
    /// they diverge every exclusion is aimed at the wrong component.
    std::vector<std::size_t> _replicatedOrdinals;

    /// Resolved once, so the per-entity write loop can suppress the Transform of
    /// a bodied entity without a registry lookup per component per entity.
    Core::Reflect::ComponentId _transformComponentId = Core::Reflect::kInvalidComponentId;

    /// Exclusion-mask bit indices for the two components whose absence changes
    /// how an entity replicates at all, resolved once for the same reason.
    std::size_t _transformOrdinal  = Core::Reflect::ComponentRegistry::kInvalidOrdinal;
    std::size_t _descriptorOrdinal = Core::Reflect::ComponentRegistry::kInvalidOrdinal;
};

} // namespace Assisi::NetSync
