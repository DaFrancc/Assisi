/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AssetCache.hpp
/// @brief Path-keyed cache resolving AssetPaths to shared GPU meshes and textures.
///
/// The game holds asset *paths* on its components (see MeshRenderer);
/// the AssetCache turns those paths into concrete GPU resources, deduplicating
/// so entities that name the same asset share one upload. It owns every mesh and
/// texture it hands out, so the pointers it returns stay valid until Clear().
///
/// Mesh paths resolve the built-in `prim://` primitives or a mesh file loaded
/// through Geometry::ImportMesh (glTF today); an empty path, or one that fails to
/// load, falls back to the unit cube so the entity still renders. Texture paths
/// are virtual asset paths resolved through AssetSystem (see Texture::LoadFromAssets).

#include <functional>
#include <unordered_map>
#include <unordered_set>

#include <nvrhi/nvrhi.h>

#include <Assisi/Core/AssetPath.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Render/MeshBuffer.hpp>
#include <Assisi/Render/Texture.hpp>

namespace Assisi::Render
{
class AssetCache
{
  public:
    AssetCache() = default;

    /// @brief Bind to a device and register the built-in `prim://` primitives.
    /// Must be called before any Resolve* call. @p textureColorSpace selects how
    /// this cache's textures are loaded: Srgb for scene albedo (the default —
    /// the mesh shader wants linear-filtered colour), Linear for textures shown
    /// straight through ImGui (e.g. asset-browser thumbnails), which must not be
    /// gamma-decoded by the sampler.
    void Initialize(nvrhi::IDevice *device, ColorSpace textureColorSpace = ColorSpace::Srgb);

    /// @brief Resolves a mesh path to a cached MeshBuffer, uploading on first use.
    /// Recognises the built-in primitives (e.g. `prim://cube`) and mesh files
    /// (e.g. `models/robot.glb`, loaded via Geometry::ImportMesh). An empty path,
    /// or a file that fails to load, falls back to the unit cube, so the return is
    /// never null. A failed file load is attempted once, then remembered so it is
    /// not re-parsed every frame. The pointer stays valid until Clear().
    const MeshBuffer *ResolveMesh(const Core::AssetPath &path);

    /// @brief Resolves a texture path to a cached Texture, loading on first use.
    /// An empty path returns null (the mesh pass then draws its flat-white
    /// fallback); a non-empty path that fails to load also returns null, after a
    /// warning, and the failure is remembered so it isn't retried every frame.
    /// A successful pointer stays valid until Clear().
    const Texture *ResolveTexture(const Core::AssetPath &path);

    /// @brief Drops every cached mesh and texture, freeing their GPU resources.
    /// Any binding sets referencing those textures (see MeshPass) must be
    /// invalidated in the same breath. The registered primitives survive — only
    /// their uploaded buffers are dropped and will re-upload on next resolve.
    void Clear();

  private:
    /// @brief Returns the cached buffer for a registered primitive path, uploading
    /// it on first use, or null if @p path names no known primitive.
    const MeshBuffer *ResolvePrimitive(const Core::AssetPath &path);

    nvrhi::IDevice *_device = nullptr;

    // Colour space every texture in this cache is loaded with (see Initialize()).
    ColorSpace _textureColorSpace = ColorSpace::Srgb;

    // Registered at Initialize(); maps a `prim://` path to its mesh factory.
    std::unordered_map<Core::AssetPath, std::function<Geometry::MeshData()>> _primitiveFactories;

    std::unordered_map<Core::AssetPath, MeshBuffer> _meshes;
    std::unordered_map<Core::AssetPath, Texture>    _textures;

    // Mesh paths that failed to load (unknown primitive, or a file ImportMesh
    // rejected). ResolveMesh runs per entity per frame, so this set serves two
    // ends: it warns only once (no log spam) and it stops a broken path from
    // being re-parsed every frame — the cube fallback is used without retrying.
    // Reset by Clear() alongside the caches themselves.
    std::unordered_set<Core::AssetPath> _missingMeshWarned;
};
} /* namespace Assisi::Render */
