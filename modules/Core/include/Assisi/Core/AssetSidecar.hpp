/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AssetSidecar.hpp
/// @brief The `.aast` sidecar payload — an asset's stable identity (and, later,
///        its import settings and sub-asset manifest).
///
/// Every file under the asset root gets a `<file>.aast` sidecar carrying that
/// file's `AssetId` (the Unity `.meta` model). The reconcile pass generates
/// missing sidecars; the database reads their ids to build the GUID→path map.
/// See docs/asset-database-architecture.md §2.
///
/// S1 stored only the id. S3 adds the **composite manifest** — a glTF's
/// `slot → material GUID` list (`subAssets`) written by the material-explosion
/// pass. A leaf asset (texture, `.amat`, level) has an empty manifest. Import
/// settings (texture color space, compression) and a `sourceHash` for
/// stale-detection (D5/S4) are named in the doc and land later; the envelope
/// (`version`/`type`) is forward-compatible, so older sidecars keep loading as
/// fields are added.
///
/// The `.aast` file is an **editor source artifact** — it does not ship (the
/// cooker consumes it into a baked pak index, S5). The *reader* below ships
/// (it deserializes ids); the *writer* is editor-only (only the reconcile pass
/// mints and writes sidecars).

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <Assisi/Core/AssetId.hpp>

namespace Assisi::Core
{

/// @brief One entry of a composite asset's manifest: the material a mesh slot
///        binds to by default. Written by the glTF material-explosion pass (S3),
///        read back as the default mesh→material binding (retiring the live
///        `AssetCache::MeshDefaultMaterial` derivation — D4).
struct AssetSubAsset
{
    std::uint32_t slot = 0;  ///< Dense material-slot index in the mesh.
    AssetId       material;  ///< The `.amat` GUID exploded for that slot.
};

/// @brief The deserialized contents of a `.aast` sidecar.
struct AssetSidecar
{
    AssetId guid; ///< The asset's stable identity.

    /// @brief Composite manifest: `slot → material GUID`. Empty for a leaf
    ///        asset. Order is not significant — each entry names its own slot.
    std::vector<AssetSubAsset> subAssets;

    /// @brief Content hash of the composite's *source* (the `.gltf`/`.glb`) at
    ///        the time its materials were exploded (S4/D5). Absent on a leaf
    ///        asset, and on a composite sidecar written before S4. A mismatch
    ///        against the current source marks the composite stale.
    std::optional<std::uint64_t> sourceHash;
};

/// @brief Mint a fresh random UUIDv4. Editor-only (asset authoring). The version
///        and variant nibbles are set per RFC 4122, so a minted id can never
///        collide with the reserved built-in range. Declared here (beside the
///        sidecar writer) so the material-explosion pass in Geometry can mint
///        child-`.amat` ids without depending on the editor-only AssetDatabase.
[[nodiscard]] AssetId MintAssetId();

/// @brief Why reading a `.aast` sidecar failed.
enum class AssetSidecarError
{
    ParseFailed, ///< Not valid JSON, or not a JSON object.
    WrongType,   ///< The envelope `type` is not the sidecar type.
    MissingGuid, ///< No `guid` field, or it is not a parseable UUID string.
};

/// @brief Human-readable description of a sidecar error (for logs).
[[nodiscard]] std::string_view ToString(AssetSidecarError error) noexcept;

/// @brief The envelope `type` string used by `.aast` files.
inline constexpr std::string_view kAssetSidecarType = "AssetSidecar";

/// @brief The current `.aast` file format version.
inline constexpr std::int32_t kAssetSidecarVersion = 1;

/// @brief Upper bound on a manifest entry's material-slot index. A `subAssets`
///        entry at or above this is rejected at deserialize — no real mesh has
///        this many material slots, and it caps the dense slot vector the
///        database allocates, so a corrupt or hostile `slot` (including a
///        negative literal that would wrap to ~0u) can't drive a huge allocation
///        or an out-of-bounds resize.
inline constexpr std::uint32_t kMaxMaterialSlots = 4096;

/// @brief Serialize a sidecar to `.aast` JSON text (envelope + fields).
///        Editor-only in spirit (only the reconcile pass writes sidecars).
[[nodiscard]] std::string SerializeSidecar(const AssetSidecar &sidecar);

/// @brief Parse `.aast` JSON text into a sidecar. Ships in every build.
[[nodiscard]] std::expected<AssetSidecar, AssetSidecarError> DeserializeSidecar(std::string_view jsonText);

} // namespace Assisi::Core
