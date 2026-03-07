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
};

} // namespace Assisi::Core::Reflect