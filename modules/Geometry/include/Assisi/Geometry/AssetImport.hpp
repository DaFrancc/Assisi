/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AssetImport.hpp
/// @brief Editor-side import pass: explode a glTF's materials into `.amat` files.
///
/// The reconcile pass (Core `AssetDatabase`) gives every file a GUID sidecar but
/// cannot reach into a glTF — it lives in Core, below Geometry. This is the step
/// that needs the importer: for a glTF whose materials have not been materialized
/// yet, it runs the importer, writes one `<model>_<name>.amat` (+ its `.aast`
/// sidecar, with a freshly minted GUID) per material next to the model, and
/// records the `slot → material GUID` binding into the glTF's own `.aast`
/// manifest. After this pass the mesh→material default is a *stored fact* the
/// database reads back (retiring the live `AssetCache::MeshDefaultMaterial`
/// derivation — D4), instead of being re-derived from the glTF every load.
/// See docs/asset-database-architecture.md §4 (step 3), §5, D4.
///
/// Editor-only tooling. It lives in Geometry (not Core) because it needs the
/// glTF importer and `.amat` serializer; when a dedicated editor/tools `Assets`
/// module is stood up (doc §7) it moves there alongside the AssetDatabase.
/// Reconcile-not-clobber governs it: a glTF that already carries a manifest, or
/// a `.amat` that already exists on disk, is left untouched.

#include <cstddef>
#include <expected>
#include <string_view>

#include <Assisi/Geometry/MeshImporter.hpp> // AssetIdResolver, MeshImportError

namespace Assisi::Geometry
{

/// @brief Explode @p gltfVirtualPath's materials into sibling `.amat` files and
///        write its `.aast` slot→material manifest.
///
/// The caller runs this only for a glTF that has no manifest yet (the reconcile
/// pass having already minted its sidecar). Texture channels in the written
/// `.amat`s are resolved to GUIDs through @p resolveTextureId (the editor backs
/// it with its AssetDatabase); an unresolved or embedded texture writes nil
/// (factor-only), exactly as importing the mesh would.
///
/// @param gltfVirtualPath Asset-relative path to the `.gltf`/`.glb`.
/// @param resolveTextureId Maps a sibling texture path to its GUID for the
///        written material channels.
/// @return The number of material slots written into the manifest, or a
///         MeshImportError if the glTF could not be imported or its sidecar
///         (which must already exist) could not be read.
[[nodiscard]] std::expected<std::size_t, MeshImportError>
ExplodeGltfMaterials(std::string_view gltfVirtualPath, const AssetIdResolver &resolveTextureId);

} /* namespace Assisi::Geometry */
