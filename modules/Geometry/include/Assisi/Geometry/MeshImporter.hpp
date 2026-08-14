/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file MeshImporter.hpp
/// @brief Extension-routed mesh-file loader: virtual asset path -> CPU MeshData.
///
/// This is the seam the engine loads model files through. Today the only backend
/// is fastgltf (`.gltf` / `.glb`); other extensions return UnsupportedFormat,
/// which is the slot a future multi-format catch-all (Assimp) plugs into without
/// touching any caller.
///
/// Every byte read — the file itself *and* any external glTF buffers it
/// references — goes through Core::AssetSystem, so the asset-root escape
/// protection is never bypassed. The importer produces one MeshData with node
/// world transforms baked in, bucketed into SubMeshes by (LOD, material):
/// same-material primitives within a LOD merge into one submesh; the authored
/// LOD convention is a `*_LOD<n>` node- or mesh-name suffix. Materials are
/// extracted into MeshData::Materials (factors + texture paths resolved
/// relative to the file); embedded images are NOT decoded — unpack .glb to
/// separate files (see docs/mesh-material-architecture.md §3).

#include <cstdint>
#include <expected>
#include <functional>
#include <string_view>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Geometry/MeshData.hpp>

namespace Assisi::Geometry
{

/// @brief Resolves a virtual asset path to a stable GUID. The editor backs this
///        with its AssetDatabase; a shipped build (or a test) may pass an empty
///        resolver, in which case every texture channel imports nil (factor-
///        only). Kept as an injected callback so Geometry never depends on the
///        editor-only database directly.
using AssetIdResolver = std::function<Core::AssetId (std::string_view virtualPath)>;

/// @brief Why an ImportMesh call failed.
enum class MeshImportError : std::uint8_t
{
    UnsupportedFormat,  ///< The extension has no registered backend (e.g. `.fbx`).
    ReadFailed,         ///< AssetSystem could not read the file (missing / escaped root).
    ParseFailed,        ///< The bytes are not a valid glTF document.
    ExternalDataFailed, ///< A referenced external buffer could not be read.
    NoGeometry,         ///< Parsed successfully but produced no triangles.
    Cancelled,          ///< Superseded before the import ran (a newer load epoch); no work done.
};

/// @brief A short human-readable name for a MeshImportError (for logs).
std::string_view ToString(MeshImportError error) noexcept;

/// @brief Loads a mesh file (by virtual asset path) into one merged MeshData.
///
/// @param virtualPath Asset-relative path, e.g. "models/robot.glb". The extension
///        selects the backend; only `.gltf`/`.glb` are supported at present.
/// @param resolveId Maps each resolved texture path to its GUID for the imported
///        materials' channels. Empty → channels import nil (factor-only).
/// @return The merged geometry, or a MeshImportError describing the failure.
std::expected<MeshData, MeshImportError> ImportMesh(std::string_view virtualPath,
                                                    const AssetIdResolver &resolveId = {});

} /* namespace Assisi::Geometry */
