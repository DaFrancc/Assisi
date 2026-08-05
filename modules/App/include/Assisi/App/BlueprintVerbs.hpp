/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file BlueprintVerbs.hpp
/// @brief The four calls the game knows blueprints through, and the two queries.
///
/// Each is thin, none runs per frame, and all of them are engine-provided
/// precisely because they are what everyone would otherwise hand-roll — usually
/// as a stored member list that goes stale (docs/blueprint-system-concept.md §7).
///
/// ## The rule these live under
///
/// **A system reads components. It must never branch on whether an entity came
/// from a blueprint.** The instance id is an argument you hand back to the API,
/// never something a system searches on, and the caller should be tooling rather
/// than gameplay: the editor selecting an instance, or a debug command destroying
/// one, reads the tag legitimately, because neither has to work on a car somebody
/// assembled by hand.
///
/// A death handler calling `DestroyInstance(member.instanceId)` looks like the
/// same thing and is not. Reading the tag to get the id *is* branching on
/// membership: the handler destroys the whole group on a placed car and does
/// nothing at all on a hand-built one. A handler that wants to take a vehicle
/// apart reads authored gameplay references, which work either way.
///
/// Destruction is by explicit verb. `Scene::Destroy` on a wheel destroys the
/// wheel and nothing else — no existing destroy path anywhere in the engine
/// acquires blueprint semantics, which matches how Runtime::GatherSubtree already
/// works: hierarchy-aware destruction is opt-in at the call site.
///
/// There is **no adopt**. Entities may be created at runtime freely; they never
/// join an instance. Adopting would break the subset invariant, and with it
/// validation, typed-view generation, and safe orphan handling.

#include <Assisi/App/World.hpp>
#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Runtime/Blueprint.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace Assisi::App
{

/// @brief Creates one instance of @p source at @p placement, and makes it
/// runnable: assets resolved, physics bodies built.
///
/// All or nothing — a missing nested file three members in leaves nothing behind.
///
/// `std::optional`, not a sentinel: an earlier draft returned a struct whose
/// `instanceId == 0` meant failure, which is ignorable in a way `if (auto car =
/// …)` is not.
///
/// **The id is what you may keep; entity handles are for right now.** A handle
/// goes stale when its entity dies, but the id survives members dying, because
/// FindMember re-resolves and DestroyInstance finds whatever is left. Storing the
/// handles is how the stored-member-list problem comes back in disguise.
///
/// There are no runtime overrides. A caller that wants a red car writes the
/// component after spawning — typed, direct, no strings, and it expresses things
/// overrides cannot. Overrides are an authoring concept for files, and there is no
/// file to write at runtime.
[[nodiscard]] std::optional<uint32_t> SpawnBlueprint(World &world, std::string_view source,
                                                     const ECS::Transform &placement);

/// @brief Destroys every live member of @p instanceId and drops its table row.
///
/// Scans the tag pool for the id; a member already destroyed simply is not found,
/// because there is no list to go stale. Each member's physics body is torn down
/// first — destroying an entity drops its RigidBody *component* but leaves the
/// Jolt body it referenced behind, still colliding.
///
/// Safe from inside a system: Scene::Destroy is already deferred to
/// FlushDestroyed.
///
/// @return false if no such instance is live.
bool DestroyInstance(World &world, uint32_t instanceId);

/// @brief One member leaves its instance; the entity lives on, unchanged in every
/// other way.
///
/// @return false if @p entity was not a member of anything.
bool PruneFromInstance(World &world, ECS::Entity entity);

/// @brief Every member leaves; the instance ceases to exist and nothing is
/// destroyed.
///
/// Prune applied to every member, which is why it does not need to be
/// all-or-nothing: a partial instance is not a broken state, because membership is
/// a query.
///
/// @return false if no such instance is live.
bool ExplodeInstance(World &world, uint32_t instanceId);

/// @brief The entity for member @p name of @p instanceId, or NullEntity.
///
/// Returns a bare handle rather than an optional, which is the established
/// entity-returning convention in this engine — the one-failure-convention rule
/// scopes to the spawn and find-instance calls above.
[[nodiscard]] ECS::Entity FindMember(World &world, uint32_t instanceId, std::string_view name);

/// @brief The table row for @p instanceId, optionally confirming its source.
///
/// The untyped half of what §7's `FindInstance<T>` will be: the source check is
/// the part that has to exist either way, because building a typed view over an
/// instance without it would happily produce a `Car` over a crate's members.
[[nodiscard]] const Runtime::BlueprintInstance *FindInstance(World &world, uint32_t instanceId,
                                                             std::string_view expectedSource = {});

} // namespace Assisi::App
