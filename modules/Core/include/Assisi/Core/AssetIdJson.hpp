/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AssetIdJson.hpp
/// @brief JSON (de)serialization for `AssetId` references, with the D2 disk-only
///        path hint.
///
/// A stored reference serializes as a small object carrying the authoritative
/// GUID plus an *advisory* last-known virtual path:
///
///     "mesh": { "guid": "…", "path": "prim://cube" }
///
/// - **On load**, only the GUID is read; the hint is discarded. The in-memory
///   `AssetId` stays a pure 16-byte value — runtime and hot paths never see the
///   hint. A bare `"guid"` string (no object) is also accepted, for hand-authored
///   or hint-less files.
/// - **On save**, the hint is regenerated from the current database via an
///   installed resolver, so it self-heals: it can be cosmetically stale in a
///   file not re-saved since a rename, but never functionally wrong (the GUID is
///   always the sole thing that resolves).
///
/// The resolver is installed by the editor (it queries the AssetDatabase). In a
/// shipped build no resolver is set and the hint is simply omitted — saving does
/// not happen there. This mirrors how `EntityRef` reaches the SceneSerializer:
/// generated serialize code (which ships) calls a free function whose context is
/// supplied out-of-band. See docs/asset-database-architecture.md D2.

#include <functional>
#include <string>

#include <nlohmann/json_fwd.hpp>

#include <Assisi/Core/AssetId.hpp>

namespace Assisi::Core
{

/// @brief Supplies the advisory path hint for an id at save time. Returns the
///        current virtual path, or an empty string if unknown (hint omitted).
using AssetIdHintResolver = std::function<std::string(const AssetId &)>;

/// @brief Install the hint resolver (editor-only). Pass an empty function to
///        clear it — serialization then omits the hint.
void SetAssetIdHintResolver(AssetIdHintResolver resolver);

/// @brief Serialize an id to `{ "guid", "path"? }`. The `path` is present only
///        when a hint resolver is installed and returns a non-empty path.
[[nodiscard]] nlohmann::json SerializeAssetId(const AssetId &id);

/// @brief Read an id from the object form (`guid` field) or a bare GUID string.
///        The `path` hint, if present, is ignored. Returns the nil id when the
///        GUID is missing or unparseable.
[[nodiscard]] AssetId DeserializeAssetId(const nlohmann::json &value);

} // namespace Assisi::Core
