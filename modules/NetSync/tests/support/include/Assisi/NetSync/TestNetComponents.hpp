/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TestNetComponents.hpp
/// @brief Test-only reflected components for the replication suite.
///
/// The engine's own replicable component set is currently Transform plus the
/// Replicated marker, and removing the marker means despawn rather than
/// component removal — so proving that a *component* can be removed and
/// replicated away needs a second, ordinary component that exists only here.

#include <Assisi/Prelude.hpp>

#include <cstdint>

namespace Assisi::NetSync::Test
{

/// @brief An ordinary replicable component: tracked, so change detection
/// reports writes to it, and removable without ending the entity.
ACOMP(tracked)
struct Health
{
    AFIELD() int32_t value = 100;
};

} // namespace Assisi::NetSync::Test
