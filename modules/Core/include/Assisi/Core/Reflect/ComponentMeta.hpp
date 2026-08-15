/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Reflect/ComponentMeta.hpp
/// @brief Runtime descriptor for a reflected component type.
///
/// addToScene uses a fully type-erased signature so Core does not need to
/// depend on ECS.  Generated code in higher-level modules (Runtime, etc.)
/// provides a lambda that casts scene_ptr back to the concrete Scene type.

#include <cstdint>
#include <functional>
#include <string>
#include <typeindex>
#include <vector>

#include <nlohmann/json.hpp>

#include <Assisi/Core/Reflect/ComponentId.hpp>
#include <Assisi/Core/Reflect/FieldMeta.hpp>

namespace Assisi::Core::Reflect
{

struct ComponentMeta
{
    std::string name;
    std::type_index typeIndex;
    std::vector<FieldMeta> fields;

    /// @brief Serialize a component instance to JSON.
    /// @param component_ptr  Pointer to a live component of this type.
    std::function<nlohmann::json(const void *component_ptr)> serialize;

    /// @brief Deserialize a component from JSON and add it to a scene.
    ///
    /// Parameters are type-erased to break the Core → ECS dependency:
    ///   scene_ptr    — pointer to an ECS::Scene, cast to void*.
    ///   entity_index — Entity::index of the target entity.
    ///   entity_gen   — Entity::generation of the target entity.
    ///   j            — JSON object for this component.
    ///
    /// @return false when a field is *present and unreadable* — a string where a
    ///         number goes, an array of the wrong length. Nothing is added to the
    ///         scene in that case: every field lands on a local instance first, so
    ///         the entity never receives a half-applied component. The specific
    ///         component, field and mismatch are logged before it returns.
    ///         An **absent** key is not a failure; it leaves the field at its C++
    ///         default, which is what lets a component gain a field without
    ///         refusing every level saved before it.
    std::function<bool(void *scene_ptr, uint32_t entity_index, uint32_t entity_gen,
                       const nlohmann::json &j)>
    addToScene;

    /// @brief Iterate all entities in a scene that have this component type.
    ///
    /// Type-erased for the same reason as addToScene.
    ///   scene_ptr — pointer to an ECS::Scene, cast to void*.
    ///   cb        — called once per entity: (entity_index, entity_gen, component_ptr).
    std::function<void(void *scene_ptr, std::function<void(uint32_t, uint32_t, const void *)>)>
    iterateEntities;

    /// @brief Direct O(1) lookup of one entity's component, or nullptr if absent.
    ///
    /// Type-erased for the same reason as addToScene. Prefer this over
    /// iterateEntities when resolving a single known entity — iterateEntities
    /// scans the whole pool.
    ///   scene_ptr    — pointer to an ECS::Scene, cast to void*.
    ///   entity_index — Entity::index of the target entity.
    ///   entity_gen   — Entity::generation of the target entity.
    std::function<const void *(void *scene_ptr, uint32_t entity_index, uint32_t entity_gen)>
    getByEntity;

    /// @brief Default-construct this component on an entity and return a
    /// writable pointer to it, replacing any existing one.
    ///
    /// The type-erased counterpart of `Scene::Add<T>` — and it goes through
    /// exactly that, so it **stamps the change tick** for an ACOMP(tracked)
    /// type. This is what a binary consumer (the replication client) uses to
    /// materialize a component before filling it in; without it the only
    /// generic way to create one is `addToScene` with hand-made JSON, which
    /// routes a binary path through the JSON codec for no reason.
    ///
    /// Type-erased for the same reason as addToScene.
    std::function<void *(void *scene_ptr, uint32_t entity_index, uint32_t entity_gen)> construct;

    /// @brief Mutable counterpart of getByEntity: the entity's component, or
    /// nullptr if it does not have one.
    ///
    /// Goes through `Scene::GetMut<T>`, so — unlike getByEntity — it **stamps
    /// the change tick** for an ACOMP(tracked) type. Reach for getByEntity when
    /// reading; reach for this when writing, and expect the write to be
    /// observable through `Changed<T>`.
    std::function<void *(void *scene_ptr, uint32_t entity_index, uint32_t entity_gen)> getMutable;

    /// @brief Whether this component participates in serialization/introspection.
    ///
    /// True for normal ACOMP components. False for ACOMP(transient) components,
    /// which register only to receive a stable ComponentId (so a Scene can store
    /// them) but carry no serialize/addToScene/iterateEntities/getByEntity/
    /// construct/getMutable hooks — those are all null. This is the explicit
    /// gate consumers must check before invoking a hook; do not probe the hooks
    /// for null yourself.
    /// Examples: Physics::RigidBody (wraps a live Jolt handle that must never be
    /// saved), Runtime::DestroyTag (a transient per-frame lifecycle marker).
    bool serializable = true;

    /// @brief Whether this component opts into ECS change detection (ACOMP(tracked)).
    ///
    /// True for components marked ACOMP(tracked). When set, a Scene gives this
    /// type's pool a per-component change tick, stamped on mutable access
    /// (Scene::GetMut / MarkChanged), so systems can process only the instances
    /// that changed since they last ran (e.g. PropagateTransforms skipping
    /// unmoved subtrees). False by default: a component pays nothing for tracking
    /// it does not opt into.
    bool tracksChanges = false;

    /// @brief Whether this component *can* travel over the network
    /// (ACOMP(replicable)).
    ///
    /// A **capability, not a policy**, and the distinction is load-bearing. This
    /// flag says the type has a defined wire form; whether any particular entity
    /// actually sends it is decided elsewhere — by the `Replicated` marker's
    /// exclusion mask (per entity) and the game's `neverReplicate` list (per
    /// game). Keeping them apart is what stops an engine module setting network
    /// policy for every game built on it. See docs/replication-optin-plan-v1.md.
    ///
    /// Opt-in: a type must not acquire a wire form by accident. "Everything
    /// serializable travels" replicates things like a `Camera` whose `isActive`
    /// would hijack the receiving client's view.
    ///
    /// A replicable component is always also tracked — reflectgen implies
    /// `tracked` from `replicable`, because an untracked component's change tick
    /// reads as 0 ("unchanged") and would transmit once at spawn and then never
    /// again. Writing both is legal and not redundant: the implication serves
    /// replication, while an explicit `tracked` records that a *local* system
    /// needs the ticks too, so removing `replicable` later cannot silently strip
    /// tracking from it.
    ///
    /// False for ACOMP(transient) components by construction: reflectgen rejects
    /// `replicable` together with `transient`, since there is nothing to encode.
    bool replicable = false;

    /// @brief Alphabetical dense id, assigned by ComponentRegistry after startup
    /// (see ComponentRegistry::IdOf). kInvalidComponentId until the registry
    /// finalizes. Placed last and defaulted so generated positional aggregate
    /// initialization of the preceding members is unaffected.
    ComponentId id = kInvalidComponentId;
};

} // namespace Assisi::Core::Reflect