/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AssetDatabase.hpp
/// @brief The editor-time GUID→path map, and the reconcile pass that builds it.
///
/// The `AssetDatabase` is the mutable index that only exists while authoring: it
/// maps each asset's `AssetId` to the file currently holding it. On `Rebuild()`
/// it walks the asset root, ensures every asset file has a `.aast` sidecar
/// (generating and minting a fresh id for any that is missing — the reconcile
/// pass), and registers `guid → path` from every sidecar, plus the reserved
/// built-in ids. Rename/move robustness falls out for free: a file's `.aast`
/// travels with it, so the next scan re-registers the same id under the new
/// path and every reference still resolves.
///
/// This is **editor-only**. A shipped build never scans an asset tree or mints
/// ids — it consumes a baked pak index (PakProvider, S5). See
/// docs/asset-database-architecture.md §3a, §4. When a shipped/editor build
/// split exists it moves behind that gate; for now it lives in Core, unbuilt in
/// no configuration because nothing ships yet.
///
/// Reconcile-not-clobber (D3): an existing sidecar is never rewritten or
/// validated during the scan; only its id is read. A missing sidecar is
/// generated. Well-formedness of an asset's payload is checked when it is
/// actually loaded, not here.

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/Errors.hpp>

namespace Assisi::Core
{

/// @brief Mint a fresh random UUIDv4. Editor-only (asset authoring). The version
///        and variant nibbles are set per RFC 4122, so a minted id can never
///        collide with the reserved built-in range.
[[nodiscard]] AssetId MintAssetId();

/// @brief Editor-time index of every asset under the asset root, keyed by id.
class AssetDatabase
{
  public:
    /// @brief Scan the asset root, reconcile sidecars, and (re)build the map.
    ///
    /// Clears any previous state, seeds the reserved built-ins, then walks the
    /// asset root: for every non-`.aast` file, reads its sidecar's id (minting +
    /// writing a sidecar first if none exists) and registers `guid → path`. A
    /// sidecar that exists but cannot be parsed is left untouched and its file
    /// is skipped with a warning (never clobbered).
    ///
    /// @return The number of asset files registered (excluding built-ins), or
    ///         AssetError::NotInitialized if the asset root is not set.
    std::expected<std::size_t, AssetError> Rebuild();

    /// @brief The current virtual path for an id (built-ins included), or
    ///        nullopt if the id is unknown.
    [[nodiscard]] std::optional<std::string> PathFor(AssetId id) const;

    /// @brief The id currently registered for a virtual path, or nullopt.
    ///        The path must be in the same generic ('/') form Rebuild() stores.
    [[nodiscard]] std::optional<AssetId> IdFor(std::string_view virtualPath) const;

    /// @brief Number of registered ids, including the reserved built-ins.
    [[nodiscard]] std::size_t Count() const noexcept;

  private:
    std::unordered_map<AssetId, std::string> _idToPath;
    std::unordered_map<std::string, AssetId> _pathToId;
};

} // namespace Assisi::Core
