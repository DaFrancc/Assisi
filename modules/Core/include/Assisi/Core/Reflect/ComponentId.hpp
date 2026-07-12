/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Reflect/ComponentId.hpp
/// @brief Stable per-component-type integer identity and its lookup.
///
/// Split out from ComponentRegistry.hpp so hot-path consumers (e.g. ECS::Scene,
/// which indexes its component pools by id) can resolve an id without pulling in
/// ComponentMeta and its nlohmann/json dependency.

#include <cstdint>
#include <typeindex>

namespace Assisi::Core::Reflect
{

/// @brief Dense, stable integer identity for a reflected component type.
///
/// Assigned by ComponentRegistry once, after startup registration completes, by
/// sorting all components alphabetically by name (so 'A' names get lower ids,
/// 'Z' names higher) and numbering them from zero. Stable for the life of the
/// process — suitable as a save-file / network identity and for indexing a flat
/// per-component array in place of a std::type_index hash lookup.
using ComponentId = std::uint32_t;

/// @brief Sentinel for "no such component" / "type is not reflected".
inline constexpr ComponentId kInvalidComponentId = ~ComponentId{0};

/// @brief The alphabetical id of a component by its C++ type, or
/// kInvalidComponentId if that type is not registered with the reflection
/// system.
///
/// Defined out-of-line (ComponentRegistry.cpp) so this header stays free of the
/// registry and JSON includes. Same runtime-only caveat as
/// ComponentRegistry::IdOf: call after startup registration completes.
ComponentId ComponentIdOfType(std::type_index type);

/// @brief The alphabetical id of a reflected component type T.
///
/// Caches the id in a function-local static on first call, so subsequent calls
/// are a plain load. The first call must happen after startup registration
/// completes, which it always does when T's id is used from live engine code
/// rather than a static initializer.
template <typename T> ComponentId ComponentIdOf()
{
    static const ComponentId id = ComponentIdOfType(std::type_index(typeid(T)));
    return id;
}

} // namespace Assisi::Core::Reflect
