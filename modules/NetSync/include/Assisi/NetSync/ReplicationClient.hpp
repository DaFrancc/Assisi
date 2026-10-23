/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ReplicationClient.hpp
/// @brief The receiving half of the state-replication protocol.
///
/// Read with ReplicationServer: the two halves are one design, and a change to
/// either one's wire handling is a change to both.

#include <Assisi/NetSync/InstanceRecord.hpp>
#include <Assisi/NetSync/ReplicationProviders.hpp>
#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/InstanceId.hpp>
#include <Assisi/ECS/SceneFwd.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/Net/NetTransport.hpp>
#include <Assisi/NetSync/BodyReport.hpp>
#include <Assisi/NetSync/BodyState.hpp>
#include <Assisi/NetSync/InputCommand.hpp>
#include <Assisi/NetSync/MessageDispatch.hpp>
#include <Assisi/NetSync/NetClock.hpp>
#include <Assisi/NetSync/NetProtocol.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

#include <typeindex>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <unordered_set>
#include <string>
#include <unordered_map>
#include <vector>

namespace Assisi::NetSync
{

/// @brief What the server stepped this client's player on, as of one snapshot.
///
/// A tick the server had a command for ran on that command; one it had none
/// for ran on the controls it held, or released (App::RelayHeldCommand). A
/// client stepping a tick again steps it on the same.
struct CommandEcho
{
    /// The snapshot's tick, which bit 0 of `starvedMask` is.
    std::uint64_t serverTick = 0;
    /// The newest tick stepped on a command from this client; zero before one.
    std::uint64_t lastAppliedCommandTick = 0;
    /// Bit i set: tick `serverTick - i` was stepped with no command.
    std::uint64_t starvedMask = 0;

    /// Whether the server had a command for @p tick. False for a tick after
    /// `serverTick` or too far before it to be in the mask, which the server
    /// has not said anything about.
    [[nodiscard]] bool Applied(std::uint64_t tick) const;
};

/// @brief Ticks a client keeps of what it predicted, and of the commands it
/// sent: more than the furthest it runs ahead of the host, so the host's word
/// about a tick still finds it.
inline constexpr std::size_t kPredictionHistoryTicks = 64;

/// @brief The receiving half. Applies snapshots into a local scene.
class ReplicationClient
{
public:
    /// @param physics The world this client simulates its mirrors in. Null means
    ///   no body is ever built and every mirror is rendered by interpolation —
    ///   what a headless convergence test with no physics world wants.
    ReplicationClient(Net::NetTransport &transport, ECS::Scene &scene, Net::ConnectionId connection,
                      Physics::PhysicsWorld *physics = nullptr);

    ReplicationClient(const ReplicationClient &)            = delete;
    ReplicationClient &operator=(const ReplicationClient &) = delete;

    /// @brief Hold the `ClientHello` until the local world has been built. Off
    ///        by default: the handshake is answered the moment it arrives.
    ///
    /// **Set it before the first Poll.** Snapshots arriving against a world that
    /// has not been built map the server's NetIds onto whichever local entities
    /// occupy those slots, and the resulting scene is wrong in a way nothing
    /// later corrects.
    ///
    /// The sequence it enables: ServerHello arrives → IsAwaitingLevel() goes true
    /// and Handshake() names the level → the application builds its world →
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
    /// **The `ClientHello` is withheld until this is set.** The hello is sent
    /// exactly once and never resent, so one carrying a placeholder hash is a
    /// refused join no retry can fix.
    void SetContentSetHash(std::uint64_t hash);

    /// @brief Whether the content-set hash has been supplied.
    [[nodiscard]] bool HasContentSetHash() const { return _contentSetHashReady; }

    /// @brief Answer the handshake: the local world is built and NetIds may now
    /// be mapped onto it. No-op unless awaiting.
    ///
    /// Two preconditions, not one — the world *and* the hash. Pass the hash here
    /// if it has only just been computed; otherwise call SetContentSetHash first
    /// and this with no argument.
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
    /// Reliability comes from the type's own declaration, not the call site.
    /// Either way the server trusts it no further — reliability is about
    /// delivery, not about belief. Sending an event fails to compile: the
    /// direction is part of the type.
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
    /// Compare it against a mirror's `ControlledBy::client` to answer "is this
    /// one mine?" — what input binding, prediction and local UI all ask.
    [[nodiscard]] ClientId LocalClientId() const { return _handshake.clientId; }

    /// @brief Whether @p entity is a mirror this client controls. False for
    /// anything uncontrolled, anything someone else controls, and everything
    /// before the handshake.
    [[nodiscard]] bool ControlsEntity(ECS::Entity entity) const;

    /// @brief The host's word about one entity this client predicts, kept
    /// apart from the entity itself, which the client's own steps run ahead.
    ///
    /// Two sections, each as new as the snapshot that last carried it: the
    /// byte budget cuts an entity's component block and its body record
    /// separately, so either can be older than the other. A tick of 0 means
    /// that section has not arrived since the entity became predicted.
    struct Confirmed
    {
        /// One component's bytes, by component id.
        using Components = std::unordered_map<Core::Reflect::ComponentId, std::vector<std::byte>>;

        /// Each ACOMP(predicted) component the entity has, as the host had it.
        /// A block leaves out what has not changed since the baseline this
        /// client acked, so what it leaves out keeps its value.
        Components components;
        /// The host tick the components are as of.
        std::uint64_t componentTick = 0;
        /// The host tick `body` is as of.
        std::uint64_t physicsTick = 0;
        /// Where the host had the entity's body, and how it moved.
        BodyState body;
    };

    /// @brief Whether this client predicts @p entity: it controls it, it is a
    /// member of a blueprint instance whose root it controls, or the host gave
    /// it the entity to predict with `Predicted`.
    ///
    /// A predicted entity's ACOMP(predicted) components and body are this
    /// client's to move; a snapshot writes the host's into ConfirmedOf instead,
    /// and nothing interpolates them. Worked out after every snapshot.
    [[nodiscard]] bool Predicts(ECS::Entity entity) const;

    /// @brief The host's word about the predicted entity @p netId, or null for
    /// one this client does not predict.
    [[nodiscard]] const Confirmed *ConfirmedOf(NetId netId) const;

    /// @brief Whether this client simulates the body of @p entity for the
    /// host: the host marked it `SimulatedBy` this client.
    ///
    /// Such a body is this machine's like any body in a single-player game:
    /// the host's word about it is not applied, it is never put back to sleep
    /// for the host, and each step's command carries where the step left it.
    /// A step stepped again meets it where it was at that step. Worked out
    /// after every snapshot.
    [[nodiscard]] bool Simulates(ECS::Entity entity) const;

    /// @brief The host's copy of one ACOMP(predicted) component of @p netId, or
    /// null if this client does not predict it or it has no such component.
    template <typename T> [[nodiscard]] const T *ConfirmedComponent(NetId netId) const
    {
        const Confirmed *confirmed = ConfirmedOf(netId);
        if (confirmed == nullptr)
        {
            return nullptr;
        }
        const Confirmed::Components::const_iterator found =
            confirmed->components.find(Core::Reflect::ComponentIdOf<T>());
        return found == confirmed->components.end() ? nullptr : reinterpret_cast<const T *>(found->second.data());
    }

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
    /// The initial world arrives over as many snapshots as the byte budget
    /// needs, and a client cannot tell a small world from the first page of a
    /// large one — so this verdict comes from the server. Use it to hold a
    /// loading screen, or to decide when a mirrored scene is safe to render.
    [[nodiscard]] bool IsWorldComplete() const { return _worldComplete; }

    /// @brief Still receiving the initial world: synchronized but not complete.
    [[nodiscard]] bool IsJoining() const { return _synchronized && !_worldComplete; }

    /// @brief Set when the server refused the connection, so a UI can say why.
    [[nodiscard]] const std::string &RejectMessage() const { return _rejectMessage; }

    /// @brief The clock feedback from the most recent snapshot, for NetClock.
    [[nodiscard]] const ClockFeedback &Feedback() const { return _feedback; }

    /// @brief Which of this client's commands the server applied, from the
    /// most recent snapshot.
    [[nodiscard]] const CommandEcho &Echo() const { return _echo; }

    /// @brief The last server tick applied.
    [[nodiscard]] std::uint64_t LastAppliedTick() const { return _lastAppliedTick; }

    [[nodiscard]] std::uint64_t SnapshotsApplied() const { return _snapshotsApplied; }

    /// @brief Snapshots rejected as malformed. Should be zero; anything else is
    /// corruption the transport did not catch, or a protocol bug.
    [[nodiscard]] std::uint64_t SnapshotsRejected() const { return _snapshotsRejected; }

    /// @brief The local entity mirroring @p netId, or NullEntity.
    [[nodiscard]] ECS::Entity EntityOf(NetId netId) const;

    /// @brief Inverse of EntityOf: the wire identity of a local mirror, or
    /// InvalidNetId if this entity is not one.
    ///
    /// A linear scan, deliberately: both callers — an inspector asking about one
    /// selected entity, and the intent encoder's entity-ref hook — ask about a
    /// single entity at a time, and a reverse map would cost more to keep correct
    /// than it saves.
    [[nodiscard]] NetId NetIdOf(ECS::Entity entity) const;

    [[nodiscard]] std::size_t ReplicatedEntityCount() const { return _entityByNetId.size(); }

    /// @brief Install what turns an instance record into local entities.
    ///
    /// **Required for any client that mirrors a world containing instances.**
    /// Without one the records are remembered, nothing is built, and the members
    /// arrive as ordinary entities *missing* every component that still equals
    /// its authored value: the server elides those, taking the client's own
    /// expansion to hold them, and nothing re-stamps a value that does not
    /// change. A member whose components all match the file arrives with none at
    /// all, and stays that way.
    ///
    /// The server cannot detect the omission. Its gate is the block's
    /// `derivable` bits, which describe the file; whether the peer opened that
    /// file is not on the wire in either direction.
    void SetInstanceExpander(std::unique_ptr<InstanceExpander> expander)
    {
        _instanceExpander = std::move(expander);
    }

    /// @brief The instances the server has named, keyed by base NetId. Each is
    /// expanded into local entities the first time it arrives, if an expander is
    /// installed; without one the record is kept, nothing is built, and the
    /// members arrive stripped of their authored components — see
    /// SetInstanceExpander.
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
    /// to re-resolve it — a mirrored world that draws nothing. Compare against
    /// the value you last acted on; re-resolve when it moves.
    [[nodiscard]] std::uint64_t StructureRevision() const { return _structureRevision; }

    /// @brief The server's advertised timing, valid once synchronized.
    [[nodiscard]] std::uint32_t ServerTickRateHz() const { return _tickRateHz; }
    [[nodiscard]] std::uint32_t ServerSnapshotHz() const { return _snapshotHz; }

    /// @brief Write interpolated transforms into the scene for the render
    /// moment @p serverTimeTicks (fractional server ticks).
    ///
    /// The client renders slightly in the *past*, between the two snapshots
    /// straddling `serverTime - interpolationDelay`. Showing the latest snapshot
    /// directly would step remote entities at the 20-30 Hz snapshot rate, which
    /// no frame rate hides. The cost is exactly that delay in every remote
    /// position, hence a delay of about two snapshot intervals and no more.
    ///
    /// Call once per frame, after applying whatever arrived. Extrapolation is
    /// deliberately not attempted: when the buffer runs dry the last known pose
    /// is held, because a wrong guess has to be corrected with a visible snap.
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
    /// transforms are propagated. The writeback overwrites a bodied mirror's
    /// Transform from its physics pose, so an offset applied before it is erased.
    ///
    /// Two timelines coexist here by design: a bodied mirror renders at
    /// server-time-minus-transit because it is simulated locally, a non-bodied
    /// one ~two snapshot intervals in the past because interpolating received
    /// samples is all one can do with state nobody simulates. A non-bodied entity
    /// the server moves to track a bodied one therefore lags it.
    /// @param dt Seconds since the last call. The offset decays in *time*, not
    ///   per frame — a per-frame constant is a different time constant at every
    ///   refresh rate, so the build would feel different at 60 and 144 Hz.
    void SmoothView(double serverTimeTicks, float dt);

    /// @brief Corrections applied, and the divergence they found.
    ///
    /// `divergence` is measured *before* the snap, between the client's own
    /// simulated pose and the authoritative one: how far the two simulations
    /// drifted since the last correction. It is the number that justifies the
    /// correction cadence — no determinism argument is available to do it.
    struct CorrectionStats
    {
        std::uint64_t applied      = 0;
        std::uint64_t bytesApplied = 0;
        std::uint64_t predictionsKept     = 0; ///< Predicted entities the host agreed with at a tick.
        std::uint64_t predictionsReplayed = 0; ///< Rollbacks: the predicted set replayed from the host's word.
        /// Answers about a tick older than any kept: the entity is put where
        /// the host had it then, with nothing to replay on top.
        std::uint64_t hardSnaps = 0;
        double divergenceSum = 0.0;
        float divergenceMax = 0.f;
        float         divergenceMean() const
        {
            return applied == 0 ? 0.f : static_cast<float>(divergenceSum / static_cast<double>(applied));
        }
    };

    [[nodiscard]] const CorrectionStats &Corrections() const { return _corrections; }

    /// @brief Ask the server to re-anchor this client from the empty baseline.
    ///
    /// For what the protocol cannot see and a human can: something is visibly
    /// wrong on screen and waiting out the next keyframe sweep is not an answer.
    /// Costs one over-full snapshot.
    void RequestKeyframe();

    /// @brief Re-assert the server's sleep verdict on every mirror it applies to.
    /// Call once per fixed step, immediately after the local physics step.
    ///
    /// Guards against the local wake-cascade: client poses differ from the
    /// server's by whatever the last correction has not removed, so a settling
    /// pile can produce contacts the server never had, and Jolt wakes by island
    /// — one spurious contact wakes a mirror the server will never speak of
    /// again, and it drifts forever on nobody's authority.
    ///
    /// The only legitimate wake is a correction with `asleep = false`; until one
    /// arrives the mirror holds still, at the cost of one transit time of
    /// hesitation.
    void EnforceSleep();

    // --- Rollback ---------------------------------------------------------------
    //
    // The client keeps, for every tick it stepped, each predicted entity as the
    // step left it: its character's physics frame and its ACOMP(predicted)
    // components. The host's word about a tick arrives a round trip later and
    // is compared with that record, section by section. Within the tolerances
    // the client's result stands and nothing moves.
    //
    // A predicted component the host disagrees with takes the host's value at
    // once. A physics frame or a Transform it disagrees with is stepped again,
    // since a step moves and turns them from controls: every predicted entity's
    // are put back to that tick, the one the host disagreed with to the host's
    // word and every other to its own record, and the caller steps them all
    // again to the present: BeginRollback, then for each tick CommandAppliedAt,
    // one replayed step and RecordHistory, then EndRollback. A replayed step
    // runs only the engine's own systems that steer a character.

    /// @brief What a step stepped again runs on: the command the host applied
    /// for its tick, or none when the host had none and held or released the
    /// last, after @p starvedTicks steps in a row without one.
    struct AppliedCommand
    {
        const InputCommand *command = nullptr;
        std::uint32_t starvedTicks = 0;
    };

    /// @brief One rollback, as BeginRollback starts it.
    struct Replay
    {
        /// The ticks to step again, oldest first, each one this client stepped.
        std::vector<std::uint64_t> ticks;
        /// The characters to step in them: the predicted ones.
        std::vector<ECS::Entity> characters;
        /// The predicted bodies to solve in them.
        std::vector<ECS::Entity> bodies;
        /// The tick the predicted set was put back to.
        std::uint64_t from = 0;
    };

    /// @brief How often one section of a predicted entity was compared with the
    /// host's word and how often the host's word corrected it, with the first
    /// field that differed the last time it did, and both its values. The
    /// physics frame is corrected by a rollback, a component by taking the
    /// host's value.
    ///
    /// A correction at a tick the host stepped without this client's command
    /// is counted apart: the host stepped this client's player on what it held,
    /// so the difference is the connection's doing rather than a misprediction.
    struct PredictionCounts
    {
        std::string firstDifference;
        std::uint64_t compares = 0;
        std::uint64_t corrections = 0;        ///< at ticks the host had this client's command for
        std::uint64_t starvedCorrections = 0; ///< at ticks the host stepped without it
    };

    /// @brief The counts for one predicted entity: its physics frame, and each
    /// ACOMP(predicted) component apart.
    struct EntityPredictionCounts
    {
        std::unordered_map<Core::Reflect::ComponentId, PredictionCounts> components;
        PredictionCounts physics;
    };

    /// @brief Remember @p command, and every predicted entity as the step it is
    /// stamped for left it. Call once per live fixed step, after the step.
    void RecordStep(const InputCommand &command);

    /// @brief Remember what the replayed step of @p tick stepped of every
    /// predicted entity, its physics frame and its Transform, in place of what
    /// the live step left. Its other predicted components stay as the live step
    /// recorded them.
    void RecordHistory(std::uint64_t tick);

    /// @brief Puts every predicted entity back to the tick the host's word
    /// last disagreed at, and starts the scene replaying them. Empty when
    /// nothing disagreed. Each EndRollback answers one that was not empty.
    [[nodiscard]] std::optional<Replay> BeginRollback();

    /// @brief What the host stepped this client's player on for @p tick, as far
    /// as it has said: the command this client sent for it when the host had it
    /// or has not said yet.
    [[nodiscard]] AppliedCommand CommandAppliedAt(std::uint64_t tick) const;

    /// @brief How every kinematic body moved through the step of @p tick, for
    /// that step stepped again, so a platform carries its riders where it did.
    /// In the sync test, every other character too, so it is met where it was.
    [[nodiscard]] std::vector<Physics::FollowerStep> FollowersAt(std::uint64_t tick) const;

    /// @brief Ends the rollback BeginRollback started: the scene steps
    /// everything again, and the difference the replay made to each predicted
    /// character's pose is eased in on screen rather than shown at once.
    void EndRollback();

    /// @brief How the host's word about the predicted entity @p netId compared
    /// with this client's, or null for one it does not predict.
    [[nodiscard]] const EntityPredictionCounts *PredictionCountsOf(NetId netId) const;

    /// @brief Every entity this client predicts, lowest NetId first.
    [[nodiscard]] std::vector<NetId> PredictedEntities() const;

    // --- Sync test ---------------------------------------------------------------
    //
    // A check that stepping a tick again lands where stepping it live did. With
    // it on, a step that has no rollback to run puts every predicted entity back
    // one tick, by its own record, and steps that tick again; every recorded
    // field that comes out different from the live step is logged and counted.
    // Any is a bug: something a replay does not put back, or a system that runs
    // differently the second time.

    void SetSyncTest(bool enabled) { _syncTest = enabled; }
    [[nodiscard]] bool SyncTest() const { return _syncTest; }

    /// @brief With the sync test on and no rollback waiting, puts every
    /// predicted entity back to the tick before the newest one recorded and
    /// starts the scene replaying them, as BeginRollback does. Empty otherwise.
    /// Answered by EndRollback and then EndSyncTest.
    [[nodiscard]] std::optional<Replay> BeginSyncTest();

    /// @brief Compares what the sync test's replay recorded with what the live
    /// step had, logging and counting each field that differs.
    void EndSyncTest();

    /// @brief Ticks the sync test stepped again, and fields that came out
    /// different.
    struct SyncTestCounts
    {
        std::uint64_t replays = 0;
        std::uint64_t differences = 0;
    };
    [[nodiscard]] const SyncTestCounts &SyncTestResults() const { return _syncTestCounts; }

    /// @brief Mirrors that were destroyed locally and had to be recreated when
    /// the server next mentioned them.
    ///
    /// Counted rather than silent: a client-side cleanup system running over
    /// mirrors (a kill-Z volume, a timed despawner) otherwise churns destroy and
    /// respawn forever with no signal anywhere.
    [[nodiscard]] std::uint64_t MirrorsResurrected() const { return _mirrorsResurrected; }

    /// @brief Drop every replicated entity and forget the session. v1's
    /// reconnect is a full rejoin, which starts here.
    ///
    /// The session is the mirrors and their bodies, the instance records, the
    /// id translations either way round, the handshake, and the join gates.
    /// Expanded instances are collapsed on the way out, so an expander is told
    /// its rows are finished.
    ///
    /// What this machine supplied survives: the expander, the deferred-handshake
    /// setting, the content-set hash, and `_structureRevision`. The object is
    /// ready to join again with no further setup.
    void Reset();

private:
    bool ApplySnapshot(Core::BitReader &reader);

    /// Applies the host's @p state for one body. A character this client
    /// predicts is left as it is: its state is in its confirmed copy, for
    /// ComparePredicted.
    void ApplyBodyState(const BodyState &state);

    /// One predicted entity as one step left it.
    struct PredictedRecord
    {
        /// Each predicted component it had, at its offset in
        /// `_predictedOffsets`.
        std::vector<std::byte> components;
        Physics::CharacterFrame character;
        /// A predicted body's pose and motion, as the host's body record says
        /// them.
        BodyState body;
        std::uint64_t tick = 0;    ///< The command tick of the step; 0 for none yet.
        std::uint64_t present = 0; ///< Bit i: it had `_predictedComponents[i]`.
        bool hasCharacter = false;
        bool hasBody = false;
    };

    /// What this client predicted of one entity, a ring of records by tick,
    /// and how its predictions have fared.
    struct PredictionHistory
    {
        std::array<PredictedRecord, kPredictionHistoryTicks> records;
        EntityPredictionCounts counts;
        /// No rollback starts for it before the client reaches this tick, so a
        /// predicted value the host disagrees with every snapshot costs one
        /// replay a snapshot interval.
        std::uint64_t cooldownUntil = 0;
        /// The host's tick it was last found wrong at and not yet put back
        /// for, or 0.
        std::uint64_t mispredictedAt = 0;
    };

    /// Whether @p entity, @p netId's mirror, is a character this client
    /// predicts and steps again.
    [[nodiscard]] bool PredictsCharacter(NetId netId, ECS::Entity entity) const;

    /// Whether @p entity, @p netId's mirror, is a body other than a character
    /// that this client predicts and solves again.
    [[nodiscard]] bool PredictsBody(NetId netId, ECS::Entity entity) const;

    /// Where this client simulates @p entity, a predicted character or body:
    /// a character's feet and the way its Transform faces, a body's pose.
    [[nodiscard]] Physics::Pose SimulatedPose(ECS::Entity entity) const;

    /// @p entity's body as @p netId's body record would say it.
    [[nodiscard]] BodyState CaptureBody(NetId netId, ECS::Entity entity) const;

    /// Puts @p entity's body where @p host has it, moving and sleeping as it says.
    void PlaceBody(ECS::Entity entity, const BodyState &host);

    /// One follower as one step left it.
    struct FollowerRecord
    {
        /// Where the step left it: what a character stepped again meets.
        Physics::FollowerFrame frame;
        /// Where the next step starts it: the same, unless a correction has
        /// moved it since.
        Physics::FollowerFrame start;
        ECS::Entity entity{ECS::NullEntity};
    };

    /// Records every follower of @p tick: each kinematic body and character,
    /// and each body this client simulates. In a replay, one the replay does
    /// not step keeps the tick's live record.
    void RecordFollowers(std::uint64_t tick);

    /// Records where the step of @p tick left each body this client
    /// simulates, for the host.
    void RecordReports(std::uint64_t tick);

    /// Records where @p entity is now as where the step after the newest tick
    /// starts it, for a correction that moved it after that tick was recorded.
    void RecordFollowerNow(ECS::Entity entity);

    /// Puts every follower where the step after the newest tick starts it: a
    /// replay leaves each where its last step left it.
    void PlaceFollowersNow();

    /// What Record takes of each predicted entity.
    enum class RecordedPart : std::uint8_t
    {
        Everything, ///< A live step: the physics frame and every predicted component.
        Stepped,    ///< A replayed step: the physics frame and the Transform, which it steps again.
        Count_,
    };

    /// Records @p part of @p tick for every predicted entity; RecordStep and
    /// RecordHistory both come here.
    void Record(std::uint64_t tick, RecordedPart part);

    /// Compares every predicted entity the snapshot of @p serverTick spoke of
    /// with what this client recorded for that tick.
    void ComparePredicted(std::uint64_t serverTick);

    /// Compares @p confirmed, the host's word about @p entity at @p serverTick,
    /// with what @p history recorded for that tick, counting into it. Appends
    /// each section a rollback corrects that disagrees to @p differing, and
    /// writes the host's value over each other predicted component that does.
    /// @return Whether it wrote any.
    bool CompareRecord(ECS::Entity entity, const Confirmed &confirmed, std::uint64_t serverTick,
                       PredictionHistory &history, std::vector<PredictionCounts *> &differing);

    /// What differs between a character's physics frame as the host had it and
    /// as this client did, beyond the tolerances; empty when nothing does.
    [[nodiscard]] std::string PhysicsDifference(const BodyState &host, const PredictedRecord &record) const;

    /// Puts @p entity where the host had it, with nothing replayed on top: the
    /// host's word is older than anything this client kept.
    void HardSnap(NetId netId, ECS::Entity entity, const Confirmed &confirmed);

    /// Puts what a replay steps of @p entity, its Transform and its physics
    /// frame, back as @p record has it, with the host's word in @p host taking
    /// the place of each it carries for that tick.
    void Restore(ECS::Entity entity, const PredictedRecord &record, const Confirmed *host);

    /// Writes the host's copy of each predicted component in @p confirmed over
    /// @p entity's own, all but its Transform when @p skipTransform.
    void WriteHostComponents(ECS::Entity entity, const Confirmed &confirmed, bool skipTransform);

    /// Writes the host's copy of the component @p componentId in @p confirmed
    /// over @p entity's own, when it carries one.
    void WriteHostComponent(ECS::Entity entity, const Confirmed &confirmed, Core::Reflect::ComponentId componentId);

    /// Puts @p entity's character in @p frame, but where, how fast, how
    /// standing and facing which way @p host says.
    void PlaceCharacter(ECS::Entity entity, Physics::CharacterFrame frame, const BodyState &host);

    /// Where the Transform sits in a record, or nothing when Transform is not
    /// predicted in this build.
    [[nodiscard]] std::optional<std::size_t> TransformRecordOffset() const;

    /// Ticks between two snapshots, as the host announced them.
    [[nodiscard]] std::uint64_t SnapshotIntervalTicks() const;

    /// Puts every predicted entity back to @p from, the ones marked mispredicted
    /// at it to the host's word, and starts the scene replaying them. Empty
    /// when @p from is no longer kept.
    [[nodiscard]] std::optional<Replay> RollBackTo(std::uint64_t from);

    /// Each field in which @p replayed, a predicted entity's record after the
    /// sync test stepped its tick again, differs from @p live, the record the
    /// live step left; empty when none does.
    [[nodiscard]] std::vector<std::string> SyncDifferences(const PredictedRecord &live,
                                                           const PredictedRecord &replayed) const;

    /// Mark one mirror as body-corrected once the components just applied to it
    /// describe a body. The physics world builds the body itself from those
    /// components, and moves a static one to wherever its Transform now says;
    /// what is kept here is only that the mirror is simulated rather than
    /// interpolated.
    void SyncMirrorBody(NetId netId, ECS::Entity entity);

    /// Steps @p entity's character, if it is one, in @p netId's place.
    void KeyCharacterOrder(ECS::Entity entity, NetId netId);

    /// Works out which mirrors this client predicts, as of the snapshot of
    /// @p serverTick. One that has become predicted starts its confirmed copy
    /// from its live components, which are the host's word until now; one that
    /// has stopped takes its confirmed components back, the host's word
    /// standing again.
    void RecomputePredictedSet(std::uint64_t serverTick);

    /// Notes, at @p serverTick, every dynamic body one of this client's
    /// characters touches, and forgets one untouched for too long.
    void NoteTouchedBodies(std::uint64_t serverTick);

    /// Reads one ACOMP(predicted) component of a predicted entity off the wire
    /// into @p confirmed rather than into the entity. False if the block is
    /// malformed.
    bool ReadPredictedComponent(ECS::Entity entity, const Core::Reflect::ComponentMeta &meta, Confirmed &confirmed,
                                Core::BitReader &reader, const Core::Reflect::CodecContext &context);
    void SendAck(std::uint64_t serverTick);
    void SendHello();

    /// A reference to a NetId that had not arrived when it was decoded. Component
    /// data may legitimately name an entity whose spawn is in a later block or a
    /// later snapshot, so the reference is patched when the target appears rather
    /// than resolved to nothing.
    struct PendingRef
    {
        ECS::Entity entity;
        Core::Reflect::ComponentId component = Core::Reflect::kInvalidComponentId;
        std::size_t fieldOffset = 0;
        NetId target      = InvalidNetId;
    };

    void ResolvePendingRefs();

    /// @brief The hooks every client-side *encode* installs, in one place.
    ///
    /// Both reference kinds or neither — see ReplicationServer::EncodeContext,
    /// which is the same rule from the other end. A mirror's NetId and the base
    /// its instance was expanded from are the only two numbers the server can
    /// read; the local handle and the local instance id are this machine's alone.
    [[nodiscard]] Core::Reflect::CodecContext EncodeContext() const;

    /// @brief The hooks every client-side *decode* installs — the inverse.
    ///
    /// A NetId with no mirror yet decodes to the null entity, and a base this
    /// client never expanded to InstanceId{0}. The entity-block path replaces
    /// `entityFromWire` with a variant that also records the unresolved ones for
    /// patching, and keeps the instance hook this installs.
    [[nodiscard]] Core::Reflect::CodecContext DecodeContext() const;

    /// The type-erased half of SendIntent, so the template stays a static_assert
    /// and everything needing a .cpp lives in one.
    bool SendIntentBytes(const void *intent, std::type_index type, std::uint64_t clientTick, bool reliable);

    /// Read and dispatch the snapshot's message section.
    ///
    /// Must be called *after* the packet's entity blocks and body states are
    /// applied: that is the whole ordering guarantee, and it is what lets a
    /// handler for an event about an entity spawned in this same packet find
    /// that entity already there.
    bool ApplyEventSection(Core::BitReader &reader);

    /// One reliable announcement, held until the world is new enough for it.
    struct DeferredAnnouncement
    {
        Core::Reflect::MessageId messageId = Core::Reflect::kInvalidMessageId;
        std::uint64_t serverTick = 0;            ///< Applied tick this needs before it means anything.
        NetId subject    = InvalidNetId;
        std::vector<std::byte>   bytes;
    };

    void HandleAnnouncement(Core::BitReader &reader);

    /// Dispatch every announcement whose moment has arrived.
    ///
    /// The control lane is not the snapshot lane, so an announcement can and does
    /// overtake the state it is about. Holding it until the applied tick reaches
    /// its stamp *and* the entity it names exists is what the snapshot section
    /// gets for free from framing.
    void DrainAnnouncements();

    /// Decode one message body and hand it to its handler. Shared by the snapshot
    /// section and the announcement path so both translate NetIds the same way
    /// and count the same drops.
    void DispatchEvent(const Core::Reflect::MessageMeta &meta, Core::BitReader &reader);

    std::vector<DeferredAnnouncement> _deferredAnnouncements;
    std::uint64_t _eventsDispatched = 0;
    std::uint64_t _eventsUnhandled  = 0;

    /// One entity's pose at one server tick. Only the transform is buffered: a
    /// health value or a state enum has no halfway point worth showing.
    struct TransformSample
    {
        std::uint64_t serverTick = 0;
        glm::vec3 position{};
        glm::quat rotation{1.f, 0.f, 0.f, 0.f};
        glm::vec3 scale{1.f, 1.f, 1.f};
    };

    /// Capture the current pose of every mirrored entity at @p serverTick. Call
    /// after a snapshot is applied and **before** Interpolate() can overwrite
    /// anything, so the buffer holds authoritative poses and not interpolated
    /// ones fed back into themselves.
    void CaptureTransforms(std::uint64_t serverTick);

    /// The last authoritative verdict about one mirrored body.
    ///
    /// The rest pose is kept, not just the sleep bit: enforcing sleep means
    /// putting the body back where the server left it, and by the time a spurious
    /// local contact has woken it, where it *is* is no longer that.
    struct MirrorBody
    {
        bool asleep = false;
        glm::vec3 restPosition{};
        glm::quat restRotation{1.f, 0.f, 0.f, 0.f};

        /// The visual offset that hides the last correction, as it stands this
        /// frame. Added on top of the physics writeback's pose every rendered
        /// frame, keeping the simulation honest and the screen smooth — two jobs
        /// a single blended pose cannot do.
        glm::vec3 positionError{0.f};
        glm::quat rotationError{1.f, 0.f, 0.f, 0.f};

        /// ...and what drives it to zero: the offset as it was when the
        /// correction landed, plus how far through its convergence window we are.
        /// Linear in that window, so the offset is gone by the deadline and moves
        /// at a *constant* on-screen speed while it lasts.
        ///
        /// Keep it linear. A multiplicative decay is frame-rate dependent and
        /// spends most of the offset in the first frame or two — the very pop
        /// this exists to avoid.
        glm::vec3 positionErrorStart{0.f};
        glm::quat rotationErrorStart{1.f, 0.f, 0.f, 0.f};
        float smoothingElapsed = 0.f;
        float smoothingWindow  = 0.f;     ///< Seconds; 0 = nothing to smooth.
    };

    /// Forget that a mirror is body-corrected. The body itself goes with the
    /// entity or its Collider, on the physics world's next reconcile. Keyed by
    /// NetId because two of its three callers — a despawn and a locally-destroyed
    /// mirror — have already lost the entity by then.
    void DestroyMirrorBody(NetId netId);

    /// Starts hiding a correction to @p record's mirror: the picture stays at
    /// @p renderedPosition and @p renderedRotation this instant, and eases to
    /// the corrected pose.
    static void AbsorbCorrection(MirrorBody &record, const Physics::Pose &rendered, const Physics::Pose &corrected);

    /// Where one predicted character was drawn before a rollback moved it.
    struct RenderedBeforeReplay
    {
        Physics::Pose pose;
        NetId netId = InvalidNetId;
    };

    Net::NetTransport &_transport;
    ECS::Scene &_scene;
    Physics::PhysicsWorld *_physics = nullptr;
    NetSession *_session = nullptr;            ///< For handler contexts; null in direct-drive tests.
    Net::ConnectionId _connection;

    /// Resolved once, because the removal path sees only ids, never types.
    /// Removing either is the one component removal with a side effect here:
    /// the mirror stops being body-corrected.
    Core::Reflect::ComponentId _colliderComponentId = Core::Reflect::kInvalidComponentId;
    Core::Reflect::ComponentId _characterComponentId = Core::Reflect::kInvalidComponentId;
    Core::Reflect::ComponentId _transformComponentId = Core::Reflect::kInvalidComponentId;

    std::unordered_map<NetId, ECS::Entity>               _entityByNetId;

    /// Instances named by the server, keyed by base NetId. The same record
    /// arrives repeatedly until acked and only the first one expands. A record
    /// outlives the packet that carried it, and is dropped only when a despawn
    /// leaves its instance with no member bound, or when its expansion failed.
    std::unordered_map<NetId, InstanceRecord>            _instanceRecords;

    std::unique_ptr<InstanceExpander>                    _instanceExpander;

    /// Base NetId → the local instance id the expander made for it. Read by
    /// `instanceFromWire`, so a replicated BlueprintMember tag names this
    /// machine's instance rather than the server's.
    std::unordered_map<NetId, ECS::InstanceId>           _instanceIdByBase;

    /// The same pairs the other way round, for `instanceToWire`: an instance id
    /// this client holds is only sayable to the server as the base it was
    /// expanded from. An instance of purely local making has no entry, and
    /// encodes as zero — it names nothing over there, because it is nothing over
    /// there. Written only alongside the map above.
    std::unordered_map<ECS::InstanceId, NetId>           _baseByInstanceId;
    std::unordered_map<NetId, MirrorBody>                _bodies;
    std::unordered_map<NetId, std::deque<TransformSample>> _transformHistory;

    /// For each entity this client predicts.
    std::unordered_map<NetId, PredictionHistory> _history;

    /// The commands this client sent, by tick, and which ticks it recorded:
    /// the same ring as each entity's records.
    std::array<InputCommand, kPredictionHistoryTicks> _sentCommands{};
    std::array<std::uint64_t, kPredictionHistoryTicks> _recordedTicks{};
    /// Every follower each recorded tick left, on the same ring.
    std::array<std::vector<FollowerRecord>, kPredictionHistoryTicks> _followers{};
    /// Each live step's report of the bodies this client simulates, on the
    /// same ring, and the ones SendInput sends.
    std::array<BodyReport, kPredictionHistoryTicks> _reports{};
    std::vector<const BodyReport *> _reportsToSend;

    /// The bodies this client simulates; see Simulates.
    std::unordered_set<NetId> _simulated;
    /// Each dynamic body one of this client's characters touched lately, and
    /// the snapshot's tick it last did, for RecomputePredictedSet.
    std::unordered_map<NetId, std::uint64_t> _touchedAt;
    /// Scratch for RecordFollowers, kept to keep its storage.
    std::vector<ECS::Entity> _followerEntities;
    /// Scratch for RecordFollowers: the record it replaces.
    std::vector<FollowerRecord> _replacedFollowers;

    /// Where each ACOMP(predicted) component sits in a record, by its index in
    /// `_predictedComponents`, and a record's size.
    std::vector<std::size_t> _predictedOffsets;
    std::size_t _predictedBytes = 0;

    /// Filled by BeginRollback, read by EndRollback.
    std::vector<RenderedBeforeReplay> _renderedBeforeReplay;

    /// What the live step of the sync test's tick recorded, by entity, until
    /// EndSyncTest compares the replay with it.
    std::vector<std::pair<NetId, PredictedRecord>> _syncLive;
    SyncTestCounts _syncTestCounts;
    std::uint64_t _syncTick = 0; ///< The tick the sync test stepped again.

    /// The host's word about every entity this client predicts, which is
    /// exactly the predicted set.
    std::unordered_map<NetId, Confirmed> _confirmed;

    /// Every ACOMP(predicted) component type, resolved once.
    std::vector<const Core::Reflect::ComponentMeta *> _predictedComponents;

    /// The mirrors whose Transform the server sends. Only these are
    /// interpolated: one whose Transform is kept back is the client's own to
    /// move, and interpolating it would write an old copy of the client's own
    /// pose back over it every frame.
    std::unordered_set<NetId> _sentTransforms;
    std::vector<PendingRef>                              _pendingRefs;

    /// Samples kept per entity. Three straddles the render time with one spare
    /// for a late snapshot; more only delays noticing a stall.
    static constexpr std::size_t kMaxSamples = 3;

    double _interpolationDelayTicks = 6.0; ///< Replaced at handshake from snapshotHz.

    ClockFeedback _feedback;
    CommandEcho _echo;
    ServerHello _handshake;
    CorrectionStats _corrections;
    std::string _rejectMessage;

    std::uint64_t _lastAppliedTick    = 0;
    std::uint64_t _snapshotsApplied   = 0;
    std::uint64_t _snapshotsRejected  = 0;
    std::uint64_t _structureRevision  = 0;
    std::uint64_t _mirrorsResurrected = 0;
    std::uint64_t _newestRecordedTick = 0; ///< The newest tick Record wrote.
    std::uint32_t _tickRateHz         = 60;
    std::uint32_t _snapshotHz         = 20;
    bool _synchronized       = false;
    bool _worldComplete      = false;
    bool _deferHandshake     = false;
    bool _awaitingLevel      = false;
    bool _rollingBack        = false; ///< Between BeginRollback and EndRollback.
    bool _syncTesting        = false; ///< Between BeginSyncTest and EndSyncTest.
    bool _syncTest           = false;

    /// The content-set hash and whether it has been supplied. A separate flag
    /// rather than a sentinel: 0 is a legitimate hash (an empty content set), and
    /// "not yet" must not be confusable with "nothing to hash".
    std::uint64_t _contentSetHash      = 0;
    bool _contentSetHashReady = false;
    /// True once ConfirmLevelReady has been told the world is built, so a hash
    /// arriving afterwards can complete the join on its own.
    bool _levelReady = false;
};

} // namespace Assisi::NetSync
