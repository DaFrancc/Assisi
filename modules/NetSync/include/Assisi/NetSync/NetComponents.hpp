/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file NetComponents.hpp
/// @brief ECS components that mark what participates in replication.

#include <Assisi/Core/Reflect/ComponentMask.hpp>
#include <Assisi/Prelude.hpp>

#include <cstdint>

namespace Assisi::NetSync
{

/// @brief How relevancy treats an entity that carries `Replicated`.
///
/// The complete standard set, present in every system surveyed from Quake 3's
/// `SVF_*` flags to Iris filters: let the provider decide, always, only to the
/// controller, or not at all (which is `Replicated`'s own absence). Resist
/// inventing a fifth — every one of them is expressible as a provider plus a
/// grant.
AENUM()
enum class Relevance : uint8_t
{
    /// The provider decides. The default, and the only value that costs
    /// nothing to evaluate.
    Default = 0,

    /// Every connection, whatever the provider says. The correct setting for
    /// anything plot-critical: a radius is a bandwidth tool, not a correctness
    /// tool, and an objective marker that vanishes at 60 metres is a bug the
    /// bandwidth saving does not pay for.
    Always = 1,

    /// Only the connection named by `ControlledBy`, and nobody at all while
    /// uncontrolled. For entities that are one player's private business — an
    /// inventory proxy, a personal waypoint.
    ControllerOnly = 2,
};

/// @brief Marks an entity as replicated: the server sends it to clients, and a
/// client creates a local mirror of it.
///
/// An opt-in marker, not a default. Most entities in a level are static
/// scenery that both sides already have from the level file, and replicating
/// them would spend bandwidth restating what nobody is changing. Authored in
/// the level (so it is a plain serialized ACOMP, not transient) and also added
/// at runtime for spawned entities.
///
/// Carries no id: the NetId↔Entity mapping is per-session runtime state owned
/// by ReplicationServer / ReplicationClient. Baking a session id into a level
/// file would be meaningless the next time it loaded.
///
/// Deliberately *not* ACOMP(replicable) itself: it says only *that* an entity
/// replicates, which the client learns from the spawn, so putting it on the wire
/// would be pure overhead. The client adds its own copy to every mirror it
/// creates.
ACOMP()
struct Replicated
{
    /// @brief Relative send priority. Higher means "prefer this when a snapshot
    /// does not fit in one packet".
    ///
    /// Drives the Tribes-style accumulator in the send loop: every live entity
    /// gains max(priority, eps) each snapshot tick, entities drain highest-first
    /// into the byte budget, and only the ones that actually went reset — so the
    /// ones that missed keep climbing and cannot starve.
    AFIELD(min = 0.0, max = 100.0) float priority = 1.f;

    /// @brief Capable component types this entity declines to send.
    ///
    /// **The policy half of the split.** `ACOMP(replicable)` on a type grants a
    /// capability — this *can* cross the wire — and this decides, per entity,
    /// which of those capabilities are actually used. An engine module can no
    /// longer set network policy for a game by editing one of its own headers,
    /// which is the whole point.
    ///
    /// Empty by default, meaning "send every capable component present". That
    /// polarity is Unity's, and it is deliberate: `Transform`, `Name`,
    /// `MeshRenderer`, and `RigidBodyDescriptor` are wanted on essentially every
    /// replicated entity, so requiring each level author to restate them would
    /// manufacture boilerplate and, worse, silent under-replication when someone
    /// forgets.
    ///
    /// Read live by the server every snapshot — there is no cache to invalidate,
    /// which is why this component does not need `tracked`. Edit it through any
    /// path; the next snapshot sees it.
    AFIELD() Assisi::Core::Reflect::ComponentMask excluded;

    /// @brief Whether relevancy filtering applies to this entity, and to whom.
    ///
    /// The escape hatch from whatever provider a game installs. `Default` — the
    /// provider decides — is what almost everything wants; the other two exist
    /// because a distance radius is the wrong tool for correctness and for
    /// privacy respectively. See Relevance.
    ///
    /// Read live by the server every snapshot, like `excluded`, so there is
    /// nothing to invalidate.
    AFIELD() Relevance relevance = Relevance::Default;
};

/// @brief Which connection's player this entity is.
///
/// Absent on everything uncontrolled — AI, props, the world — which is the
/// default and costs nothing. Deliberately named for what it *is* rather than
/// "Owner": the survey behind this design found that "ownership" in shipping
/// engines is up to five unrelated jobs fused onto one pointer, and
/// the systems with the fewest ownership bugs are the ones whose names refuse
/// to say the word. This component does exactly three of those jobs — input
/// binding, directed-message addressing, and disconnect cleanup — and state
/// authority is not one of them. The server writes everything, always.
///
/// **Written only by the server's session layer, never authored.** A client id
/// is session-scoped: baking `client = 3` into a level file would bind that
/// entity to whoever happens to draw the id 3 in some future session, which is
/// a bug that only shows up with three players in the room. The server strips
/// every loaded instance when a session starts (ReplicationServer's
/// constructor) and the inspector does not offer authoring it. A stale instance
/// in a sessionless world is inert — nothing reads it without a session.
///
/// Replicates to everyone rather than to its controller alone: clients
/// legitimately want to know who controls what (name tags, team colours), the
/// payload is five bytes, and a per-connection field condition would be a whole
/// new mechanism with one consumer. A game that must hide it has
/// `Replicated::excluded`, per entity, already.
ACOMP(replicable)
struct ControlledBy
{
    /// @brief The controlling client's session id — a `NetSync::ClientId`
    /// value. 0 claims nothing, 1 is the host, 2+ are remote clients.
    ///
    /// A plain integer rather than the wrapper type because reflection fields
    /// are described by their storage, and the wire form of the wrapper *is*
    /// this integer. Compare against `ClientId::value`.
    AFIELD() uint32_t client = 0;

    /// @brief What becomes of this entity when that client disconnects:
    /// despawn (the player-spawned default) or merely lose the component — a
    /// world object someone was temporarily driving, which should still be
    /// there afterwards.
    AFIELD() bool despawnOnDisconnect = true;
};

/// @brief Marks a locally-created *mirror* of a remote entity: this machine
/// receives it, it does not own it.
///
/// The counterpart of Replicated, and the exact inverse of who may write:
/// Replicated says "the server sends this"; Mirrored says "the server sends me
/// this". Everything a client shows in a session carries both — the marker
/// because it arrived through replication, this because it is not authoritative
/// here.
///
/// ACOMP(transient) by construction. A mirror is session state: it exists
/// because a connection is up, and saving one into a level file would bake a
/// stranger's entity into the level. It is also why the editor's read-only guard
/// can key off it — the tag cannot survive a save and reappear as authorable
/// data.
ACOMP(transient)
struct Mirrored
{
};

} // namespace Assisi::NetSync
