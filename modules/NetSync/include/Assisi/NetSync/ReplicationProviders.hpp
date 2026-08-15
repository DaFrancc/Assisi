/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ReplicationProviders.hpp
/// @brief The interfaces a game plugs into replication, and per-connection
///        diagnostics.

#include <Assisi/NetSync/ReplicationConfig.hpp>
#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/InstanceId.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/NetSync/InputCommand.hpp>
#include <Assisi/NetSync/NetProtocol.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace Assisi::NetSync
{

class ReplicationServer;

/// @brief One connection's situation, handed to a relevancy provider once per
/// snapshot.
///
/// Read-only by design: a provider answers a question, it does not participate
/// in sending.
struct RelevancyQuery
{
    /// Who is asking. Read it if policy differs per participant — a spectator
    /// seeing more than a player.
    ClientId client;

    /// The entities this connection views the world from. Usually one, but
    /// **may be empty**: a connection given no anchors and controlling nothing.
    /// Fail open there — return everything — rather than nothing.
    std::span<const ECS::Entity> anchors;

    /// Every replicated entity, sorted ascending. The candidate set, and the
    /// only NetIds a provider may legitimately name.
    std::span<const NetId> live;

    /// Read-only scene access, for providers that measure something about
    /// entities. Never null.
    const ECS::Scene *scene = nullptr;

    /// For `EntityOf` / `NetIdOf`. Never null.
    const ReplicationServer *server = nullptr;

    /// The tick this snapshot is being built at, for providers keeping per-pair
    /// state over time (hysteresis dwell counters, for instance).
    std::uint64_t simTick = 0;
};

/// @brief Decides which entities one connection is told about at all.
///
/// The provider owns the policy; the engine owns the set and the guarantee that
/// follows from it — an entity outside a connection's set contributes zero
/// bytes to that connection. That guarantee is what makes a game-side
/// information boundary (line of sight, fog of war) implementable as a provider
/// rather than an engine feature.
///
/// Called server-side, once per connection per snapshot. Only the returned set
/// is consumed, never the method used to compute it — event-driven, spatial
/// grid, or game-specific visibility structure are all fine.
///
/// **No provider is the default**, and it is not a provider: a server with none
/// tells everyone about everything and skips the intersection outright, so the
/// mechanism costs nothing when unused. A provider returning the live set
/// verbatim is byte-identical to that path (pinned by test).
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
    /// grants, the implicit controlled-entity grant and `Relevance::Always`
    /// may still add to.
    virtual void Compute(const RelevancyQuery &query, std::vector<NetId> &out) = 0;

    /// @brief Forget whatever per-pair state @p client accumulated. Called when
    /// a connection leaves; without it, dwell counters and last-known distances
    /// leak for the life of the session.
    virtual void ForgetClient(ClientId client) { (void)client; }
};

/// @brief What the server needs to know about one blueprint instance to name it
/// on the wire instead of sending its members as unrelated entities.
///
/// The instance table lives in `App::World`, which NetSync does not depend on,
/// so it arrives through a provider.
struct InstanceInfo
{
    /// Index into the sorted content-set manifest both sides agreed on at the
    /// handshake. Safe as an index because the join refuses on a content-set
    /// hash mismatch, so both sides hold the same list in the same order.
    std::uint32_t blueprintIndex = 0;

    /// Where the instance was placed. **The transform the server expanded
    /// with**, not the root's current pose — there is no root — because the
    /// client composes its members from this plus the blueprint.
    ECS::Transform placement;

    /// How many members the blueprint declares. Fixes the width of the NetId
    /// block, so it must be the flattened count *after* authored removals —
    /// i.e. `BlueprintDefinition::members.size()`.
    std::uint32_t memberCount = 0;
};

/// @brief Answers "what is instance 7?" for the server.
///
/// Installed by App, which owns the instance table. Absent, every member
/// replicates as an ordinary entity — correct, just larger.
class InstanceInfoProvider
{
public:
    virtual ~InstanceInfoProvider() = default;

    InstanceInfoProvider()                                        = default;
    InstanceInfoProvider(const InstanceInfoProvider &)            = delete;
    InstanceInfoProvider &operator=(const InstanceInfoProvider &) = delete;

    /// @brief Fill @p out for @p instanceId, or return false if it names nothing.
    ///
    /// False is a complete answer, not an error: an instance from a blueprint
    /// outside the content set cannot be named by index, so its members fall
    /// back to replicating individually rather than carrying an index the
    /// client would resolve to a different file.
    [[nodiscard]] virtual bool Describe(ECS::InstanceId instanceId, InstanceInfo &out) = 0;

    /// @brief Does @p component still hold what the blueprint says it should?
    ///
    /// True elides it: the client expands the same file, so an untouched
    /// component is already correct over there. This is where blueprint
    /// replication earns its saving.
    ///
    /// Asked only on the empty baseline — a spawn-time cost, never per-tick.
    /// **Compare by value, never by change tick.** A tick only says when
    /// something was written, so a component written back to its authored value,
    /// or written in the tick the instance was created, gets the wrong answer.
    /// A wrongly elided component stays wrong until the next keyframe sweep, and
    /// a wrongly elided *unchanging* one stays wrong for ever.
    ///
    /// Default false: nothing is elided and every member sends full state.
    [[nodiscard]] virtual bool MatchesAuthored(ECS::InstanceId instanceId, std::uint32_t memberIndex,
                                               Core::Reflect::ComponentId id, const void *component)
    {
        (void)instanceId;
        (void)memberIndex;
        (void)id;
        (void)component;
        return false;
    }
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
    /// live replicated count; climbing without bound means a retired NetId
    /// never left the map.
    std::uint32_t baselineEntries    = 0;
    /// Re-anchors from the empty baseline, by the keyframe sweep or by a client
    /// asking for one.
    std::uint64_t keyframeSweeps     = 0;
    /// Entities that had something to send and did not fit in the last
    /// snapshot. Zero is normal; staying high means the byte budget is binding
    /// and correction *frequency* is degrading — which the priority accumulator
    /// makes fair, not free.
    std::uint32_t dirtyBacklog       = 0;

    /// Entities this connection is currently told about. Equals the live count
    /// when nothing filters; otherwise, what a radius is actually buying.
    std::uint32_t relevantEntities   = 0;

    /// Ids block escalation pushed while composing the set above, last snapshot.
    /// The cost of "an instance is relevant whole or not at all", and the one
    /// number that says whether it still scales with what is *placed*: it is the
    /// summed size of the escalated blocks, so a relevant car costs its member
    /// count once, no matter how many of its members the provider named. Growing
    /// with the square of that is the shape this counter exists to catch (S10).
    std::uint32_t escalationPushes   = 0;

    /// Entities that have entered and left this connection's set, session
    /// totals. Enters climbing in lockstep with exits is boundary thrash — the
    /// failure mode hysteresis exists to prevent, and invisible otherwise
    /// because it looks like ordinary bandwidth.
    std::uint64_t relevancyEnters    = 0;
    std::uint64_t relevancyExits     = 0;

    // ── Intents ──────────────────────────────────────────────────────────────
    // One counter per way an intent can be refused: "intents dropped" is not a
    // diagnosis. Rate-limited means a misbehaving client, stale means a clock
    // problem, out-of-range means a liar or a build mismatch, unhandled means a
    // missing handler — four different conversations.

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
    /// Rejected because the sender does not control the entity it named. An
    /// honest client hits this during a control transfer, so it is counted
    /// rather than treated as an attack.
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
    /// Reliable announcements sent on the control lane. Separately counted
    /// because these are meant to stay rare, and an unwatched rule is not kept.
    std::uint64_t announcementsSent  = 0;
    /// Events waiting for the entity they are about to arrive. Climbing and
    /// staying high means the game is talking about entities this connection
    /// will never be told about.
    std::uint32_t eventsHeld         = 0;
    /// Held events dropped because their subject despawned before it ever
    /// arrived — the event is now about nothing.
    std::uint64_t eventsEvicted      = 0;
    /// Held events dropped because the queue hit its cap, oldest first.
    std::uint64_t eventsOverflowed   = 0;
    /// Directed events with nobody to direct them at: an uncontrolled entity,
    /// or a client that has left.
    std::uint64_t eventsUndeliverable = 0;
    /// Scoped events whose AFIELD(subject) named nothing, so relevancy had no
    /// entity to scope delivery by and no client could be told. Counted on the
    /// host's diagnostics, since no one connection owns the failure.
    ///
    /// Always a caller bug — the declaration says this event is about an entity
    /// and the send passed none — which is why it is counted rather than
    /// tolerated: the alternative reading, "deliver it to everybody", is the
    /// relevancy boundary failing open.
    std::uint64_t eventsUnscoped     = 0;
};

} // namespace Assisi::NetSync
