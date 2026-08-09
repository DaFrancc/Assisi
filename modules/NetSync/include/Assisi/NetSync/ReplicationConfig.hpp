/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ReplicationConfig.hpp
/// @brief Relevancy and replication configuration, and the config loader.
///
/// The two halves of this protocol are one design and must be read together:
/// a change to either one's wire handling is a change to both.

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

} // namespace Assisi::NetSync
