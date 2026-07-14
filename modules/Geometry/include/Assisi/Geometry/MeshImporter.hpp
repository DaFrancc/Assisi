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
/// protection is never bypassed. The importer produces a single merged MeshData
/// (all primitives across all scene nodes, with node world transforms baked in);
/// materials and textures are not consumed yet.

#include <expected>
#include <string_view>

#include <Assisi/Geometry/MeshData.hpp>

namespace Assisi::Geometry
{

/// @brief Why an ImportMesh call failed.
enum class MeshImportError
{
    UnsupportedFormat,  ///< The extension has no registered backend (e.g. `.fbx`).
    ReadFailed,         ///< AssetSystem could not read the file (missing / escaped root).
    ParseFailed,        ///< The bytes are not a valid glTF document.
    ExternalDataFailed, ///< A referenced external buffer could not be read.
    NoGeometry,         ///< Parsed successfully but produced no triangles.
};

/// @brief A short human-readable name for a MeshImportError (for logs).
std::string_view ToString(MeshImportError error) noexcept;

/// @brief Loads a mesh file (by virtual asset path) into one merged MeshData.
///
/// @param virtualPath Asset-relative path, e.g. "models/robot.glb". The extension
///        selects the backend; only `.gltf`/`.glb` are supported at present.
/// @return The merged geometry, or a MeshImportError describing the failure.
std::expected<MeshData, MeshImportError> ImportMesh(std::string_view virtualPath);

} /* namespace Assisi::Geometry */
