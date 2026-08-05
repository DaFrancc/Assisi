/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file BlueprintMember.hpp
/// @brief The only thing a blueprint leaves on the entities it expands into.

#include <Assisi/Prelude.hpp>

#include <Assisi/ECS/InstanceId.hpp>

#include <cstdint>

namespace Assisi::ECS
{

/// @brief Says which spawned instance an entity belongs to, and which entry of
/// that blueprint's member list it is.
///
/// Eight bytes, and **no name**. Names are paths once nesting is involved —
/// `parking_lot_a/car_3/wheel_fl` is 26 characters — so a fixed 32-byte inline
/// string overflows at two ordinary levels of nesting, and truncation makes two
/// members indistinguishable, which silently retargets overrides and FindMember.
/// The name is recovered instead by a walk that has to exist anyway: instance
/// table → source path → the cached member list → `[memberIndex].name`. That also
/// makes FindMember *faster*, since the name resolves to an index once and the
/// query then compares integers rather than strings per entity.
///
/// Derived at expansion and **never written to a file** — same tier as NetId:
/// created at load, rebuilt on every load, authored nowhere. There is no root
/// entity, no lineage tree and no stored member list; "the members of instance 7"
/// is a query, computed when asked and discarded. That is the difference from
/// Unity DOTS's LinkedEntityGroup, which stores the list on a root entity and
/// goes stale the moment a member dies or is reparented.
///
/// ## The rule this type exists under
///
/// **A system reads components. It must never branch on whether an entity came
/// from a blueprint.** Anything a blueprint produces, code could produce by hand,
/// and afterwards nothing may tell the difference — a VehicleDrive that works on
/// a placed car must work identically on one assembled in code and on one
/// received over the network.
///
/// So this is an argument you hand back to the API, never something a system
/// searches on, and the caller must be tooling rather than gameplay: the editor
/// selecting an instance, or a debug command destroying one, reads it
/// legitimately because neither has to work on a hand-assembled car. A death
/// handler calling `DestroyInstance(member.instanceId)` looks like the same thing
/// and is not — it destroys the whole group on a placed car and does nothing at
/// all on a hand-built one. Iterating this pool to find an entity's siblings is
/// the same smell, more obviously.
///
/// ## Why it replicates
///
/// Membership is state with a current value, and the house rule is that nothing
/// with a current value becomes an event (docs/replication-plan-v4.md §5).
/// Without it, a host that prunes a member leaves the client believing the wheel
/// still belongs to the car, and no despawn record has a correct reading
/// afterwards. `instanceId` is a per-world counter, so the wire carries the
/// instance's baseNetId and the client maps it to its own local id on receipt —
/// the same translation an EntityRef → NetId already performs. Sent once and not
/// again unless a prune changes it (or a keyframe sweep resends full state, as it
/// does for everything).
///
/// It lives in ECS rather than Runtime for the reason Transform does: NetSync has
/// to read it with types, and NetSync deliberately does not link Runtime.
ACOMP(replicable)
struct BlueprintMember
{
    /// Which spawned copy. A default-constructed id is "none" — the table hands
    /// out from 1 — and its own type because the number is per-world and
    /// per-machine: a server's instance 7 names nothing on a client. The wire
    /// carries the instance's `baseNetId` and each side translates at the codec
    /// boundary, which is what makes this tag mean the same thing on both.
    AFIELD() InstanceId instanceId;
    AFIELD() uint32_t memberIndex = 0; ///< Which entry in the blueprint's flattened member list.
};

} // namespace Assisi::ECS
