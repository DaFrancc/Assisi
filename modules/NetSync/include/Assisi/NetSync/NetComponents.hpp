/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file NetComponents.hpp
/// @brief ECS components that mark what participates in replication.

#include <Assisi/Prelude.hpp>

#include <cstdint>

namespace Assisi::NetSync
{

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
ACOMP()
struct Replicated
{
    /// @brief Relative send priority. Higher means "prefer this when a snapshot
    /// does not fit in one packet".
    ///
    /// Unused in v1 — every changed entity is sent every snapshot — but the
    /// send loop is already shaped as a sortable list, so the Tribes-style
    /// accumulator drops in later with no protocol change. Authored now so that
    /// change, when it comes, needs no level-file migration.
    AFIELD(min = 0.0, max = 100.0) float priority = 1.f;
};

} // namespace Assisi::NetSync
