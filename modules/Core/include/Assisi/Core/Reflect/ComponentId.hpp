/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Reflect/ComponentId.hpp
/// @brief Stable per-component-type integer identity and its lookup.
///
/// Split out from ComponentRegistry.hpp so hot-path consumers (e.g. ECS::Scene,
/// which indexes its component pools by id) can resolve an id without pulling in
/// ComponentMeta and its nlohmann/json dependency.

#include <Assisi/Core/StrongId.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
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
///
/// An aggregate, matching NetSync::NetId/ClientId/ConnectionId — aggregate
/// initialization (`ComponentId{7}`) is the only way in, which is what blocks
/// the implicit conversion in both directions. Deliberately no arithmetic: an
/// ordinal is allocated, indexed, and packed into composite keys, never added
/// to or subtracted from.
///
/// **Unlike NetId/InstanceId/ConnectionId, zero is a genuine id here.** Those
/// types use zero as their "none" sentinel because a counter that starts
/// counting from 1 never produces it; a ComponentId is not a counter, it is an
/// alphabetical *ordinal*, and the alphabetically-first reflected component
/// gets id 0. So IsValid() cannot be "nonzero" — it has to compare against
/// kInvalidComponentId (all-ones, defined below the struct). Do not "fix" this
/// to `value != 0` to match the other three id types: that would silently
/// declare the first component invalid.
struct ComponentId
{
    std::uint32_t value = 0;

    [[nodiscard]] constexpr bool IsValid() const { return value != ~std::uint32_t{0}; }

    friend constexpr bool operator==(ComponentId, ComponentId)  = default;
    friend constexpr auto operator<=>(ComponentId, ComponentId) = default;
};

/// @brief Sentinel for "no such component" / "type is not reflected". All-ones,
/// not zero — zero is the alphabetically-first component's genuine id, so it
/// cannot double as "no component" the way it does for NetId/InstanceId/
/// ConnectionId. A default-constructed ComponentId{} is therefore id 0
/// (valid), not "none"; callers that mean "none" must say
/// `kInvalidComponentId` explicitly, as ComponentMeta::id does.
inline constexpr ComponentId kInvalidComponentId{~std::uint32_t{0}};

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

namespace Assisi::Core
{
/// Encodes as a varint on the wire — the per-block prefix every component
/// payload carries.
template <> struct IsStrongId<Reflect::ComponentId> : std::true_type
{
};
static_assert(StrongId<Reflect::ComponentId>);
} // namespace Assisi::Core

/// Prints as the bare number, so a log line reads "component 7" rather than
/// making every call site spell `.value`.
template <> struct std::formatter<Assisi::Core::Reflect::ComponentId> : std::formatter<std::uint32_t>
{
    auto format(Assisi::Core::Reflect::ComponentId id, std::format_context &ctx) const
    {
        return std::formatter<std::uint32_t>::format(id.value, ctx);
    }
};

template <> struct std::hash<Assisi::Core::Reflect::ComponentId>
{
    [[nodiscard]] std::size_t operator()(Assisi::Core::Reflect::ComponentId id) const noexcept
    {
        return std::hash<std::uint32_t>{}(id.value);
    }
};
