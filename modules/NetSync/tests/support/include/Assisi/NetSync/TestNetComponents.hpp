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
/// serializable, and deliberately *not* marked replicated (so "unmarked types
/// never travel" has something to be true about), and a norep field inside a
/// replicated one (so "saved to disk, never sent" has something to be true
/// about).

#include <Assisi/Prelude.hpp>

#include <cstdint>

namespace Assisi::NetSync::Test
{

/// @brief An ordinary replicable component: removable without ending the
/// entity, and carrying one field that must never leave the server.
ACOMP(replicated)
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

} // namespace Assisi::NetSync::Test
