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
///
/// Deliberately *not* ACOMP(replicated) itself: it says only *that* an entity
/// replicates, which the client learns from the spawn, so putting it on the wire
/// would be pure overhead. The client adds its own copy to every mirror it
/// creates.
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
