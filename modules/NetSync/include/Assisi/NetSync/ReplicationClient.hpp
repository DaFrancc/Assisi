/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ReplicationClient.hpp
/// @brief The receiving half of the state-replication protocol.
///
/// The two halves of this protocol are one design and must be read together:
/// a change to either one's wire handling is a change to both.

#include <Assisi/NetSync/InstanceRecord.hpp>
#include <Assisi/NetSync/ReplicationProviders.hpp>
#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/InstanceId.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
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

    /// @brief Install what turns an instance record into local entities.
    ///
    /// Absent, records are remembered and nothing is built — the members still
    /// arrive as ordinary entities, which is correct and merely larger.
    void SetInstanceExpander(std::unique_ptr<InstanceExpander> expander)
    {
        _instanceExpander = std::move(expander);
    }

    /// @brief The instances the server has named, keyed by base NetId.
    ///
    /// Recorded but not yet acted on: expanding one into local members is 7c.
    /// Exposed now so the receiving half can be tested before the half that
    /// consumes it exists.
    [[nodiscard]] const std::unordered_map<NetId, InstanceRecord> &InstanceRecords() const
    {
        return _instanceRecords;
    }

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

    /// Instances named by the server, keyed by base NetId. Never cleared by an
    /// individual snapshot: a record is a fact about the session, not about the
    /// packet that carried it, and the same one arrives repeatedly until acked.
    std::unordered_map<NetId, InstanceRecord>            _instanceRecords;

    std::unique_ptr<InstanceExpander>                    _instanceExpander;

    /// Base NetId → the local instance id the expander made for it. What
    /// `instanceFromWire` reads, so a replicated BlueprintMember tag names this
    /// machine's instance rather than the server's.
    std::unordered_map<NetId, ECS::InstanceId>           _instanceIdByBase;
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
