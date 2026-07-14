/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Reflect/AssetTypeRegistry.hpp
/// @brief Singleton registry of all reflected standalone asset types (AASSET).
///
/// Generated .generated.cpp files register their asset types here via static
/// initializers at program startup — the same service-locator pattern as
/// ComponentRegistry, and for the same reason: registration happens before
/// main(), and the table is immutable and read-only once running, so the
/// shared process-wide state carries no ordering or test-isolation hazard.

#include <cstddef>
#include <span>
#include <string_view>
#include <typeindex>
#include <vector>

#include <Assisi/Core/Reflect/AssetTypeMeta.hpp>

namespace Assisi::Core::Reflect
{

class AssetTypeRegistry
{
  public:
    static AssetTypeRegistry &Instance();

    /// @brief Register an asset type. Called by generated code at startup.
    void Register(AssetTypeMeta meta);

    /// @brief Find an asset type by its string name (the AASSET struct name),
    ///        or nullptr if none is registered under that name.
    const AssetTypeMeta *Find(std::string_view name) const;

    /// @brief Find an asset type by its C++ type, or nullptr if not reflected.
    const AssetTypeMeta *Find(std::type_index type) const;

    /// @brief All registered asset types, in registration order.
    std::span<const AssetTypeMeta> All() const;

    /// @brief Number of registered asset types.
    std::size_t Count() const;

  private:
    AssetTypeRegistry() = default;

    std::vector<AssetTypeMeta> _metas;
};

} // namespace Assisi::Core::Reflect
