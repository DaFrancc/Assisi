/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Reflect/ComponentRegistry.hpp
/// @brief Singleton registry of all reflected component types.
///
/// Generated .generated.cpp files register their component types here via
/// static initializers at program startup.
///
/// @note Intentional service-locator: registration happens in static
/// initializers, before main() and any owner object could exist, so this
/// genuinely cannot be an instance threaded through the app. The reflection
/// table is also immutable after startup and read-only at runtime, so shared
/// process-wide state carries no ordering or test-isolation hazard.

#include <span>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include <Assisi/Core/Reflect/ComponentId.hpp>
#include <Assisi/Core/Reflect/ComponentMeta.hpp>

namespace Assisi::Core::Reflect
{

class ComponentRegistry
{
  public:
    static ComponentRegistry &Instance();

    /// @brief Register a component type.  Called by generated code at startup.
    void Register(ComponentMeta meta);

    /// @brief Find a component by its string name, or nullptr if not found.
    const ComponentMeta *Find(std::string_view name) const;

    /// @brief Iterate all registered component types, in ascending id order
    /// (i.e. alphabetically by name).
    std::span<const ComponentMeta> All() const;

    /// @brief Iterate only the serializable components, in the same order.
    ///
    /// Skips ACOMP(transient) id-only registrations. Every ComponentMeta this
    /// yields has non-null serialize/addToScene/iterateEntities/getByEntity
    /// hooks, so a consumer that only handles serialization can iterate these
    /// and call the hooks without a per-item null/serializable check.
    std::span<const ComponentMeta *const> SerializableComponents() const;

    /// @brief Number of registered component types.
    std::size_t Count() const;

    /// @brief The alphabetical id of a component by its C++ type, or
    /// kInvalidComponentId if that type is not reflected.
    ///
    /// @warning Call only at runtime (after main() begins), never from a static
    /// initializer. Ids are finalized lazily once all startup registrations are
    /// in; querying mid-registration would observe a not-yet-final ordering.
    /// This matches the class contract: the table is immutable after startup.
    ComponentId IdOf(std::type_index type) const;

    /// @brief The alphabetical id of a component by name, or kInvalidComponentId.
    ComponentId IdOf(std::string_view name) const;

    /// @brief The component with the given id, or nullptr if id is out of range.
    /// Because ids are dense from zero, this is a direct index.
    const ComponentMeta *ById(ComponentId id) const;

    /// @brief Iterate only the replicable components, in ascending id order.
    ///
    /// The same shape as SerializableComponents(), one predicate further in. A
    /// component's position in *this* sequence is its replicable ordinal, which
    /// is what ComponentMask indexes by.
    std::span<const ComponentMeta *const> ReplicableComponents() const;

    /// @brief Bit index of @p id within a ComponentMask, or kInvalidOrdinal if
    /// that component is not replicable.
    ///
    /// A dense-array lookup, not a search: this runs per component per entity per
    /// snapshot, so the map is built once at finalize.
    ///
    /// @warning Ordinals are **not** stable across builds — adding, renaming, or
    /// flipping the capability of any component reshuffles them. They are a
    /// runtime index and must never be persisted; ComponentMask serializes names
    /// for exactly this reason.
    std::size_t ReplicableOrdinalOf(ComponentId id) const;

    /// @brief Sentinel returned by ReplicableOrdinalOf for a non-replicable
    /// component.
    static constexpr std::size_t kInvalidOrdinal = static_cast<std::size_t>(-1);

  private:
    ComponentRegistry() = default;

    /// @brief Sort by name, assign dense ids, and rebuild the type→id map.
    /// Idempotent and cheap once finalized; re-runs only after a new Register.
    void EnsureFinalized() const;

    // Mutable so the const query methods can finalize lazily. Safe under the
    // class contract: all mutation happens during single-threaded startup
    // registration, and the state is read-only once main() is running.
    mutable std::vector<ComponentMeta>                        _metas;
    mutable std::vector<const ComponentMeta *>                _serializable;
    mutable std::vector<const ComponentMeta *>                _replicable;
    /// Replicable ordinal per ComponentId, kInvalidOrdinal for the rest. Sized to
    /// the whole table so the hot lookup is an index rather than a search.
    mutable std::vector<std::size_t>                          _replicableOrdinal;
    mutable std::unordered_map<std::type_index, ComponentId>  _idByType;
    mutable bool                                              _finalized = false;
};

} // namespace Assisi::Core::Reflect