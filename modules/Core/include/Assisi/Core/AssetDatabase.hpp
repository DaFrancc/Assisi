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
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/AssetSidecar.hpp> // MintAssetId; AssetSubAsset (manifest entries).
#include <Assisi/Core/Errors.hpp>

namespace Assisi::Core
{

/// @brief Whether a Rebuild() may write to the asset tree.
enum class RebuildMode : std::uint8_t
{
    /// The authoring default: mint and write a sidecar for any asset lacking
    /// one, and re-mint on an id collision, so every file ends up addressable.
    Reconcile,

    /// Never write anything. An asset with no sidecar (or a colliding id) is
    /// skipped rather than fixed, so the index is only as complete as the tree
    /// already was.
    ///
    /// For a second process sharing one asset tree with the first — a
    /// play-in-editor client. Two processes minting into the same directory is
    /// a race whose loser silently gets a different id for the same file, which
    /// is exactly the kind of corruption that shows up a week later as a level
    /// referencing an asset that no longer exists.
    ReadOnly,
};

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
    /// @p mode selects whether the reconcile may write; see RebuildMode.
    ///
    /// @return The number of asset files registered (excluding built-ins), or
    ///         AssetError::NotInitialized if the asset root is not set.
    std::expected<std::size_t, AssetError> Rebuild(RebuildMode mode = RebuildMode::Reconcile);

    /// @brief The current virtual path for an id (built-ins included), or
    ///        nullopt if the id is unknown.
    [[nodiscard]] std::optional<std::string> PathFor(AssetId id) const;

    /// @brief The id currently registered for a virtual path, or nullopt.
    ///        The path must be in the same generic ('/') form Rebuild() stores.
    [[nodiscard]] std::optional<AssetId> IdFor(std::string_view virtualPath) const;

    /// @brief Number of registered ids, including the reserved built-ins.
    [[nodiscard]] std::size_t Count() const noexcept;

    /// @brief Every registered file asset (id, path) — excludes the reserved
    ///        built-ins, which are not files. The editor iterates this to find
    ///        importable composites (glTFs) to explode. Order is unspecified.
    ///        Cold path (once per reimport); paths stay std::string to match the
    ///        DB's storage and PathFor(), so long scan paths never truncate.
    [[nodiscard]] std::vector<std::pair<AssetId, std::string>> Assets() const;

    /// @brief Whether @p meshId carries a composite manifest — i.e. its glTF
    ///        materials have already been exploded into `.amat` children. The
    ///        explosion pass skips a mesh that already has one (reconcile).
    [[nodiscard]] bool HasManifest(AssetId meshId) const;

    /// @brief The default material bound to @p slot of composite @p meshId,
    ///        from the mesh's `.aast` manifest — the stored replacement for the
    ///        retired live `MeshDefaultMaterial` derivation (D4). Nil when the
    ///        mesh has no manifest, or the slot is unlisted.
    [[nodiscard]] AssetId SlotMaterial(AssetId meshId, std::uint32_t slot) const;

private:
    std::unordered_map<AssetId, std::string> _idToPath;
    std::unordered_map<std::string, AssetId> _pathToId;

    // Composite manifests: mesh id → slot-indexed default material ids (index =
    // slot; nil fills any gap). Read from each sidecar's `subAssets` on Rebuild.
    std::unordered_map<AssetId, std::vector<AssetId>> _manifests;
};

} // namespace Assisi::Core
