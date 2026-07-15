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
/// S1 stores only the id. Import settings (texture color space, compression) and
/// the composite manifest (a glTF's `slot → material GUID`) are named in the doc
/// and land in later stages; the envelope (`version`/`type`) is forward-
/// compatible, so older sidecars keep loading as fields are added.
///
/// The `.aast` file is an **editor source artifact** — it does not ship (the
/// cooker consumes it into a baked pak index, S5). The *reader* below ships
/// (it deserializes ids); the *writer* is editor-only (only the reconcile pass
/// mints and writes sidecars).

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include <Assisi/Core/AssetId.hpp>

namespace Assisi::Core
{

/// @brief The deserialized contents of a `.aast` sidecar.
struct AssetSidecar
{
    AssetId guid; ///< The asset's stable identity.
};

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

/// @brief Serialize a sidecar to `.aast` JSON text (envelope + fields).
///        Editor-only in spirit (only the reconcile pass writes sidecars).
[[nodiscard]] std::string SerializeSidecar(const AssetSidecar &sidecar);

/// @brief Parse `.aast` JSON text into a sidecar. Ships in every build.
[[nodiscard]] std::expected<AssetSidecar, AssetSidecarError> DeserializeSidecar(std::string_view jsonText);

} // namespace Assisi::Core
