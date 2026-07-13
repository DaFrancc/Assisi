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
    std::string            name;
    std::type_index        typeIndex;
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
    std::function<void(void *scene_ptr, uint32_t entity_index, uint32_t entity_gen,
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

    /// @brief Whether this component participates in serialization/introspection.
    ///
    /// True for normal ACOMP components. False for ACOMP(transient) components,
    /// which register only to receive a stable ComponentId (so a Scene can store
    /// them) but carry no serialize/addToScene/iterateEntities/getByEntity hooks
    /// — those are all null. This is the explicit gate consumers must check
    /// before invoking a hook; do not probe the hooks for null yourself.
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

    /// @brief Alphabetical dense id, assigned by ComponentRegistry after startup
    /// (see ComponentRegistry::IdOf). kInvalidComponentId until the registry
    /// finalizes. Placed last and defaulted so generated positional aggregate
    /// initialization of the preceding members is unaffected.
    ComponentId id = kInvalidComponentId;
};

} // namespace Assisi::Core::Reflect