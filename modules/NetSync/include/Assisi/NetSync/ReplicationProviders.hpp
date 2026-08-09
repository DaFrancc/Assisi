/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ReplicationProviders.hpp
/// @brief The interfaces a game plugs into replication, and per-connection diagnostics.
///
/// The two halves of this protocol are one design and must be read together:
/// a change to either one's wire handling is a change to both.

#include <Assisi/NetSync/ReplicationConfig.hpp>
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

/// @brief What the server needs to know about one blueprint instance to name it
/// on the wire instead of sending its members as unrelated entities.
///
/// The instance table that holds this lives in `App::World`, which NetSync does
/// not depend on and must not — so it arrives through a provider, the same shape
/// and the same reason as RelevancyProvider above.
struct InstanceInfo
{
    /// Index into the sorted content-set manifest both sides agreed on at the
    /// handshake. An index rather than a path because the join already refuses
    /// on a content-set hash mismatch, so both sides have the same list in the
    /// same order — and a path per spawn would be the largest field in the
    /// record by an order of magnitude.
    std::uint32_t blueprintIndex = 0;

    /// Where the instance was placed. The client composes its members from this
    /// and the blueprint, so it must be the same transform the server expanded
    /// with, not the root's current pose — there is no root.
    ECS::Transform placement;

    /// How many members the blueprint declares. Fixes the width of the NetId
    /// block, so it must be the flattened count *after* authored removals, which
    /// is what `BlueprintDefinition::members.size()` is.
    std::uint32_t memberCount = 0;
};

/// @brief Answers "what is instance 7?" for the server.
///
/// Installed by App, which owns the instance table. Absent, the server has no
/// way to describe an instance and every member replicates as an ordinary
/// entity — correct, just larger, and the state blueprints replication exists
/// to improve on.
class InstanceInfoProvider
{
  public:
    virtual ~InstanceInfoProvider() = default;

    InstanceInfoProvider()                                        = default;
    InstanceInfoProvider(const InstanceInfoProvider &)            = delete;
    InstanceInfoProvider &operator=(const InstanceInfoProvider &) = delete;

    /// @brief Fill @p out for @p instanceId, or return false if it names nothing.
    ///
    /// False is a complete answer, not an error: an instance spawned from a
    /// blueprint outside the content set cannot be named by index, and its
    /// members must fall back to replicating individually rather than being
    /// described by an index the client would resolve to a different file.
    [[nodiscard]] virtual bool Describe(ECS::InstanceId instanceId, InstanceInfo &out) = 0;

    /// @brief Does @p component still hold what the blueprint says it should?
    ///
    /// This is the saving. A client expands the instance from the same file, so
    /// a member's untouched components are already correct over there and
    /// sending them is pure waste — a parking lot of a hundred identical cars
    /// should cost one record each, not three components per wheel.
    ///
    /// Asked only on the empty baseline, so it is a spawn-time cost, never a
    /// per-tick one. **Compared by value, not by change tick**: a tick says when
    /// something was written, and a component written back to its authored value
    /// — or written in the same tick the instance was created — would be skipped
    /// or sent wrongly. A wrongly skipped component stays wrong until the next
    /// keyframe sweep, and a wrongly skipped *unchanging* one stays wrong for
    /// ever.
    ///
    /// Default false: without an implementation nothing is elided and every
    /// member sends full state, which is the pre-blueprint behaviour.
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

} // namespace Assisi::NetSync
