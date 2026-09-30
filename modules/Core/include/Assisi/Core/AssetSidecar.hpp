/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AssetSidecar.hpp
/// @brief The `.aast` sidecar payload — an asset's stable identity, the kind of
///        asset it is, and a composite's sub-asset manifest.
///
/// Every file under the asset root gets a `<file>.aast` sidecar carrying that
/// file's `AssetId` (the Unity `.meta` model). The reconcile pass generates
/// missing sidecars; the database reads their ids to build the GUID→path map.
///
/// A composite (a glTF) also carries the **manifest** — its `slot → material
/// GUID` list (`subAssets`) written by the material-explosion pass (S3) — and
/// the `sourceHash` of the source it was exploded from (S4/D5). A leaf asset
/// (texture, `.amat`, level) has neither. Import settings (texture color space,
/// compression) land later; the envelope (`version`/`type`) is
/// forward-compatible, so older sidecars keep loading as fields are added.
///
/// The `.aast` file is an **editor source artifact** — it does not ship (the
/// cooker consumes it into a baked pak index, S5). The *reader* below ships
/// (it deserializes ids); the *writer* is editor-only (only the reconcile pass
/// mints and writes sidecars).

#include <cstddef>
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
///        read back as the default mesh→material binding — the stored form the
///        renderer uses instead of deriving it live (D4).
struct AssetSubAsset
{
    std::uint32_t slot = 0;  ///< Dense material-slot index in the mesh.
    AssetId material;        ///< The `.amat` GUID exploded for that slot.
};

/// @brief One use of a file: the kind of asset it is.
struct AssetUse
{
    /// The kind's registered name, such as "texture" or "sound".
    std::string kind;
};

/// @brief How many uses a file may have. One today; the sidecar keeps them in a
///        list so a file can gain a second later without rewriting sidecars.
inline constexpr std::size_t kMaxAssetUses = 1;

/// @brief The deserialized contents of a `.aast` sidecar.
struct AssetSidecar
{
    AssetId guid; ///< The asset's stable identity.

    /// @brief What the file is used as. Every file with a sidecar states its
    ///        kind here; nothing infers one from the extension. Empty only on a
    ///        sidecar from before kinds were written, or one whose format no
    ///        kind reads.
    std::vector<AssetUse> uses;

    /// @brief Composite manifest: `slot → material GUID`. Empty for a leaf
    ///        asset. Order is not significant — each entry names its own slot.
    std::vector<AssetSubAsset> subAssets;

    /// @brief Content hash of the composite's *source* (the `.gltf`/`.glb`) at
    ///        the time its materials were exploded (S4/D5). Absent on a leaf
    ///        asset, and on any sidecar written before hashing existed. A
    ///        mismatch against the current source marks the composite stale.
    std::optional<std::uint64_t> sourceHash;

    /// @brief A leaf asset's sidecar: identity only, no manifest and no source
    ///        hash. Every mint site wants this; spelling it
    ///        `AssetSidecar{.guid = id}` instead draws
    ///        -Wmissing-field-initializers.
    [[nodiscard]] static AssetSidecar Leaf(AssetId id)
    {
        AssetSidecar sidecar;
        sidecar.guid = id;
        return sidecar;
    }
};

/// @brief The sidecar a new file at @p vpath is written with: @p id, and the
///        kind AssetKindRegistry gives new files of its format, written out so
///        a later change to that choice leaves this file alone. No use when no
///        kind reads the format.
[[nodiscard]] AssetSidecar NewFileSidecar(AssetId id, std::string_view vpath);

/// @brief Mint a fresh random UUIDv4. Editor-only (asset authoring). The version
///        and variant nibbles are set per RFC 4122, so a minted id can never
///        collide with the reserved built-in range. Declared here (beside the
///        sidecar writer) so the material-explosion pass in Geometry can mint
///        child-`.amat` ids without depending on the editor-only AssetDatabase.
[[nodiscard]] AssetId MintAssetId();

/// @brief Why reading a `.aast` sidecar failed.
enum class AssetSidecarError : std::uint8_t
{
    ParseFailed, ///< Not valid JSON, or not a JSON object.
    WrongType,   ///< The envelope `type` is not the sidecar type.
    MissingGuid, ///< No `guid` field, or it is not a parseable UUID string.
    TooManyUses, ///< More uses than kMaxAssetUses.
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
