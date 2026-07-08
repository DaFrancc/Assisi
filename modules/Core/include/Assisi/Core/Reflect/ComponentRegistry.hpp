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
#include <vector>

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

    /// @brief Iterate all registered component types.
    std::span<const ComponentMeta> All() const;

  private:
    ComponentRegistry() = default;
    std::vector<ComponentMeta> _metas;
};

} // namespace Assisi::Core::Reflect