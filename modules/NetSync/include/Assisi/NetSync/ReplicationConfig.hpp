/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ReplicationConfig.hpp
/// @brief What a game tunes about replication, and the loader that reads it.

#include <Assisi/NetSync/InputCommand.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::NetSync
{

/// @brief Which relevancy provider a game wants, and how to tune it.
///
/// Deliberately **not** in the protocol hash: this changes what is *sent*, never
/// how bytes decode, so two builds differing only here pair fine and the
/// server's numbers govern. Hashing it would make a tuning edit refuse
/// connections for no correctness gain.
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
    /// The gap is the mechanism, not a refinement: with a single radius, an
    /// entity sitting on the boundary despawns and fully respawns on every
    /// crossing. A value at or below `radius` is corrected at load, with a
    /// warning.
    float exitRadius = 75.f;

    /// @brief Ticks an entity must stay beyond `exitRadius` before the revoke
    /// takes effect.
    ///
    /// **Gates revokes only.** Entering is immediate, so an anchor teleport — a
    /// level transition, a spectator jump — shows the world at once rather than
    /// after a beat of emptiness. Making the dwell symmetric is the reflex to
    /// resist.
    std::uint32_t dwellTicks = 30;
};

/// @brief Read the `networking.relevancy` object of a game config.
///
/// An absent key yields the default (everything relevant); a malformed one warns
/// and yields the same. Every loader here fails in that direction on purpose — a
/// config typo must not silently *narrow* what a game sends.
[[nodiscard]] RelevancyConfig LoadRelevancyFromConfig(std::string_view configPath = "game.json");

/// @brief Tuning shared by both halves.
struct ReplicationConfig
{
    /// The simulation's fixed-step rate. Snapshots are timed against it.
    std::uint32_t tickRateHz = 60;

    /// How often state goes out. **Clamped to a divisor of `tickRateHz`** — a
    /// rate that does not divide makes the interval alternate between two
    /// values, which shows up as interpolation judder rather than as an error
    /// anyone would think to look for.
    ///
    /// 20-30 Hz is normal at this scale. Simulation and input stay at the full
    /// tick rate; only state is sent at this one, and interpolation hides the
    /// gap. Halving it roughly halves both diff cost and bandwidth.
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
    /// Insurance, not a pillar: the acked baseline already guarantees delivery.
    /// What it cannot cover is what happens *after* delivery — state that
    /// arrived, was acked, and then went wrong locally. This sweep is the answer
    /// to that.
    ///
    /// **Think twice before disabling it.** It saves almost nothing (~0.6 kB/s
    /// for 64 entities), and for any client-side system that writes replicated
    /// fields on mirrors it turns "wrong until the next sweep" into "wrong
    /// forever" — the delta path never resends a field the server is not
    /// re-stamping.
    std::uint64_t keyframeIntervalTicks = 512;

    /// Ceiling on input packets accepted per connection per second. The excess
    /// is dropped before the codec runs, which is the cheapest place to say no.
    std::uint32_t maxInputPacketsPerSecond = 200;

    /// Ceiling on intents of *one message type* per connection per second — per
    /// type, so a flood of one cannot starve another. Checked before the payload
    /// is decoded, so exceeding it costs a comparison.
    std::uint32_t maxIntentsPerTypePerSecond = 60;

    /// How far behind the server an intent's client tick may be before it is
    /// dropped as stale, and how far ahead before it is dropped as impossible.
    ///
    /// Load-bearing for *unreliable* intents, where out-of-order arrival is
    /// normal: without a window, a late map ping would land in a world that has
    /// moved on.
    std::uint32_t intentStaleWindowTicks = 120; ///< Two seconds at 60 Hz.
    std::uint32_t intentLeadWindowTicks  = 60;  ///< One second of client lead.

    /// Bytes the message section may run *past* `maxSnapshotBytes` by, when
    /// something is waiting to go out.
    ///
    /// An allowance, not a reservation. Holding bytes back up front does not
    /// work: the entity and body passes stop once they are already at the
    /// budget, and the last entity written can overshoot by more than any
    /// reservation, so the floor would evaporate exactly when the world is
    /// busiest. Overrunning a soft cap by a bounded amount, only when events are
    /// pending, is what actually stops a full world starving them.
    std::size_t reservedEventBytes = 96;

    /// How many unreliable events one connection may have waiting for the
    /// entities they are about. Past this the oldest is dropped and counted.
    ///
    /// An unbounded queue means events about entities this connection will never
    /// be told about — a scoping bug in the game rather than a condition to
    /// absorb. The cap makes it show up in the counter instead of in memory.
    std::size_t maxHeldEventsPerConnection = 64;

    /// Bounds a command must satisfy before the simulation sees it.
    InputLimits inputLimits;

    /// Component type names this *game* never replicates, whatever the engine
    /// says they are capable of.
    ///
    /// The third gate, between the type's capability and each entity's exclusion
    /// mask. It exists so a game can veto a component **once**, rather than on
    /// every entity that happens to carry it.
    ///
    /// Names rather than ids: ids are dense and alphabetical, so they would rot
    /// the moment any component is added. Names that resolve to nothing warn and
    /// are ignored.
    ///
    /// Deliberately **not** in the protocol hash — unlike quantization, which is
    /// hashed because it changes how bytes decode. This only changes which
    /// self-describing blocks are sent, and the server's list governs.
    ///
    /// Filled by whoever owns the session, from game.json; the server never
    /// reads the filesystem itself, so a test can set it without a file.
    std::vector<std::string> neverReplicate;

    /// Who is told about what. Defaults to everything, which costs nothing; a
    /// server whose provider is `Distance` installs one at construction. Filled
    /// from game.json on the same terms as `neverReplicate`.
    RelevancyConfig relevancy;
};

/// @brief Read the `networking.neverReplicate` array of a game config.
///
/// An absent key yields an empty list; a malformed one warns and yields the
/// same. Call at session start and put the result in
/// ReplicationConfig::neverReplicate.
///
/// Read per session rather than once per process, so editing it and hosting
/// again takes effect. That is safe precisely because it is not hashed.
[[nodiscard]] std::vector<std::string> LoadNeverReplicateFromConfig(std::string_view configPath = "game.json");

} // namespace Assisi::NetSync
