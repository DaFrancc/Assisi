/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TestNetComponents.hpp
/// @brief Test-only reflected components for the replication suite.
///
/// The engine's own replicable component set is currently Transform plus the
/// Replicated marker, and removing the marker means despawn rather than
/// component removal — so proving that a *component* can be removed and
/// replicated away needs a second, ordinary component that exists only here.
///
/// The gating milestone added two more jobs: a component that is reflected,
/// serializable, and deliberately *not* marked replicable (so "unmarked types
/// never travel" has something to be true about), and a norep field inside a
/// replicable one (so "saved to disk, never sent" has something to be true
/// about).

#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/InstanceId.hpp>
#include <Assisi/Prelude.hpp>

#include <cstdint>

namespace Assisi::NetSync::Test
{

/// @brief An ordinary replicable component: removable without ending the
/// entity, and carrying one field that must never leave the server.
ACOMP(replicable)
struct Health
{
    AFIELD() int32_t value = 100;

    /// @brief Server-side bookkeeping. Saved with the level like any other
    /// field; excluded from the wire, so a client's copy holds its default no
    /// matter what the server does to it.
    AFIELD(norep) int32_t secret = 0;
};

/// @brief Reflected, serializable, tracked — and deliberately not replicated.
///
/// The negative control for wire gating. Before opt-in, every serializable
/// component travelled, which is how a marked entity could ship a `Camera` whose
/// `isActive` hijacked the receiving client's view.
ACOMP(tracked)
struct LocalOnly
{
    AFIELD() int32_t value = 0;
};

/// @brief One message per cell of the AMSG grammar, so every combination has
/// something to be true about.
///
/// A test build is the only place all four exist together; a real game would
/// declare whichever it needs. They are also what the registry, the codec
/// round-trip, the hash-moves property, and the generated handler table are
/// exercised against — none of which can be tested by a build that registers no
/// messages at all.

/// @brief Client → server, must arrive. The shape of a deliberate action whose
/// loss the player would notice.
AMSG(intent, reliable)
struct TestPlaceMarker
{
    AFIELD() uint32_t target = 0;
    AFIELD() int32_t  slot   = 0;
};

/// @brief Client → server, freshest wins. The spammy case, where a resent stale
/// message is worse than a lost one.
///
/// The bounded fields are the reject-don't-clamp case: an out-of-range value
/// here means the client is lying or the two builds disagree, and clamping it
/// would turn a detectable attack into a silently accepted one.
AMSG(intent, unreliable)
struct TestPing
{
    AFIELD(min = -100.0, max = 100.0) float x = 0.f;
    AFIELD(min = -100.0, max = 100.0) float y = 0.f;
};

/// @brief An intent about an entity the sender claims to control.
///
/// `pawn` is the intent's *subject*, and `target` deliberately is not — a
/// client naming something it does not own is ordinary ("shoot at that"), while
/// a client acting through something it does not own is not.
AMSG(intent, reliable)
struct TestMovePawn
{
    AFIELD(controlled) Assisi::ECS::Entity pawn;
    AFIELD()           Assisi::ECS::Entity target;
    AFIELD()           int32_t             mode = 0;
};

/// @brief Server → client, loss tolerable. The default form: rides the snapshot,
/// so its ordering against the entity it names is free.
///
/// `source` is the entity this is about, and it is what relevancy scopes the
/// message by — a connection that cannot see the entity is not told the event
/// happened either, which is the zero-bytes guarantee covering messages and not
/// only state.
AMSG(event, unreliable)
struct TestBurst
{
    AFIELD() Assisi::ECS::Entity source;
    AFIELD() int32_t             intensity = 1;
};

/// @brief An intent that names a blueprint instance rather than an entity.
///
/// The instance-id counterpart to TestMovePawn's entity ref, and the shape
/// reflectgen actively recommends: an `InstanceView` may not be stored, so
/// "which instance" is spelled as an `ECS::InstanceId` field that outlives the
/// view. The number is a per-world counter, so the codec has to translate it the
/// way it translates an entity ref — otherwise following that recommendation
/// hands the server the *sender's* instance id and the recommendation is a trap.
AMSG(intent, reliable)
struct TestTagInstance
{
    AFIELD() Assisi::ECS::InstanceId instance;
    AFIELD() int32_t                 note = 0;
};

/// @brief The same question in the other direction, naming no entity — so
/// relevancy has nothing to scope it by and the instance id is the only thing
/// the test can be wrong about.
AMSG(event, reliable, independent)
struct TestInstanceNamed
{
    AFIELD() Assisi::ECS::InstanceId instance;
};

/// @brief A registered intent that nothing handles.
///
/// Not an oversight — the negative control. A message nobody handles is a
/// normal state, because the sender's build may care about something this one
/// does not, and the point is that it is *counted* rather than silently
/// swallowed.
AMSG(intent, unreliable)
struct TestUnhandled
{
    AFIELD() int32_t value = 0;
};

/// @brief Server → client, must arrive, and names no entity — so there is
/// nothing for relevancy to scope it by and nothing to hold it for.
AMSG(event, reliable, independent)
struct TestAnnounce
{
    AFIELD() int32_t round = 0;
};

} // namespace Assisi::NetSync::Test
