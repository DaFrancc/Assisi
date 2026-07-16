/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AssetCache.hpp
/// @brief Cache resolving stable asset ids (and, for thumbnails, paths) to shared
///        GPU meshes, textures, and materials.
///
/// The game holds stable GUIDs (`AssetId`) on its components (see MeshRenderer);
/// the AssetCache turns those ids into concrete GPU resources, deduplicating so
/// entities that name the same asset share one upload. It owns every mesh,
/// texture, and material it hands out, so the pointers it returns stay valid
/// until Clear().
///
/// Ids are mapped to virtual paths through resolvers the editor installs from its
/// AssetDatabase (SetAssetResolvers); reserved built-in ids (the `prim://`
/// primitives) are handled internally without a resolver. Internally the caches
/// stay keyed by path — the id is translated once at the boundary — which keeps
/// deduplication and the primitive tables unchanged. The texture path API
/// (ResolveTexture) stays public and path-based for the ImGui thumbnail cache,
/// which browses files directly.
///
/// Mesh paths resolve the built-in `prim://` primitives or a mesh file loaded
/// through Geometry::ImportMesh (glTF today); an empty path, or one that fails to
/// load, falls back to the unit cube so the entity still renders. Texture paths
/// are virtual asset paths resolved through AssetSystem (see Texture::LoadFromAssets),
/// plus a handful of `prim://` solid-colour texture primitives that stand in for
/// a material's empty channels (white, linear white, flat normal) — engine-
/// generated assets that live in this same cache rather than a separate static,
/// so a device rebuild regenerates them uniformly. Material paths resolve `.amat`
/// files (empty/failed -> a neutral fallback material).

#include <cstdint>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <nvrhi/nvrhi.h>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/AssetPath.hpp>
#include <Assisi/Geometry/MaterialData.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Render/GeometryArena.hpp>
#include <Assisi/Render/Material.hpp>
#include <Assisi/Render/MeshBuffer.hpp>
#include <Assisi/Render/Texture.hpp>

namespace Assisi::Render
{
class AssetCache
{
  public:
    AssetCache() = default;

    /// @brief Bind to a device, register the built-in `prim://` mesh and texture
    /// primitives, and build the fallback material. Must be called before any
    /// Resolve* call. @p textureColorSpace is the default colour space for
    /// ResolveTexture calls that don't specify one — Srgb for scene albedo, Linear
    /// for textures shown straight through ImGui (e.g. asset-browser thumbnails).
    void Initialize(nvrhi::IDevice *device, ColorSpace textureColorSpace = ColorSpace::Srgb);

    /// @brief Id↔path translators supplied by the editor (from the AssetDatabase).
    using IdToPathFn = std::function<Core::AssetPath(const Core::AssetId &)>;
    using PathToIdFn = std::function<Core::AssetId(std::string_view)>;

    /// @brief Install the id↔path resolvers. Reserved built-in ids resolve without
    /// them; real files need them. `pathToId` is handed to the mesh importer so a
    /// glTF's texture channels are stored as ids. Unset resolvers mean real-file
    /// ids resolve to nothing (fallback) — the state before an editor wires them.
    void SetAssetResolvers(IdToPathFn idToPath, PathToIdFn pathToId)
    {
        _idToPath = std::move(idToPath);
        _pathToId = std::move(pathToId);
    }

    /// @brief Resolves a mesh id to a cached MeshBuffer, uploading on first use.
    /// Recognises the reserved built-in primitives (e.g. `prim://cube`) and mesh
    /// files (loaded via Geometry::ImportMesh). A nil/unknown id, or a file that
    /// fails to load, falls back to the unit cube, so the return is never null. A
    /// failed file load is attempted once, then remembered. The pointer stays
    /// valid until Clear().
    const MeshBuffer *ResolveMesh(const Core::AssetId &id);

    /// @brief Resolves a texture path to a cached Texture, loading on first use,
    /// in the cache's default colour space (see Initialize).
    const Texture *ResolveTexture(const Core::AssetPath &path)
    {
        return ResolveTexture(path, _textureColorSpace);
    }

    /// @brief Resolves a texture path in an explicit colour space. The cache is
    /// keyed on (path, colour space), so the same file can be resident as both an
    /// sRGB colour map and a linear data map without one clobbering the other.
    /// An empty path returns null; a non-empty path that fails to load also
    /// returns null (after a one-time warning). `prim://` texture primitives
    /// (white / white-linear / flat-normal) resolve here too, in their own fixed
    /// colour space. A successful pointer stays valid until Clear().
    const Texture *ResolveTexture(const Core::AssetPath &path, ColorSpace colorSpace);

    /// @brief Resolves a `.amat` material id to a cached Material, loading on
    /// first use. A nil/unknown id, or a file that fails to load/parse, returns
    /// the neutral fallback material — so the return is never null. The pointer
    /// stays valid until Clear().
    const Material *ResolveMaterial(const Core::AssetId &id);

    /// @brief The neutral fallback material (id 0): white albedo, metallic 0,
    /// roughness 0.6 — the engine's pre-material look. Never null.
    const Material *FallbackMaterial() const { return &_fallbackMaterial; }

    /// @brief The bindless material-texture descriptor table and its layout
    /// (GPU-driven stage D). Every resolved texture holds a slot here; materials
    /// reference channels by index. The MeshPass adds the layout to its pipeline
    /// (register space 1) and binds the table each draw. Stable across Clear()
    /// (the contents reset, the handles do not), so consumers bind once.
    nvrhi::IBindingLayout *BindlessLayout() const { return _bindlessLayout; }
    nvrhi::IDescriptorTable *BindlessTable() const { return _bindlessTable; }

    /// @brief Drops every cached mesh, texture, and material, freeing their GPU
    /// resources, and rebuilds the fallback material. Any binding sets referencing
    /// those resources (see MeshPass) must be invalidated in the same breath.
    /// Stable material ids are NOT reused across Clear().
    void Clear();

  private:
    /// @brief The virtual path an id currently maps to. Reserved built-ins resolve
    /// from the static table (no resolver needed); other ids go through the
    /// installed id→path resolver. Nil/unknown ids return an empty path.
    Core::AssetPath PathForId(const Core::AssetId &id) const;

    /// @brief Path-keyed mesh resolve (built-in or file). Backs ResolveMesh after
    /// it translates an id to a path.
    const MeshBuffer *ResolveMeshPath(const Core::AssetPath &path);

    /// @brief Path-keyed `.amat` resolve. Backs ResolveMaterial after translation.
    const Material *ResolveMaterialPath(const Core::AssetPath &path);

    /// @brief Returns the cached buffer for a registered primitive path, uploading
    /// it on first use, or null if @p path names no known primitive.
    const MeshBuffer *ResolvePrimitive(const Core::AssetPath &path);

    /// @brief Resolves channel id @p channelId (or, if nil/failed, @p
    /// fallbackPrimitive) for one material channel, returning the texture's slot
    /// in the bindless descriptor table. Sets @p *outPresent to whether the real
    /// (non-fallback) texture was used.
    uint32_t ResolveChannel(const Core::AssetId &channelId, ColorSpace space,
                            const Core::AssetPath &fallbackPrimitive, bool *outPresent = nullptr);

    /// @brief Ensures @p texture has a slot in the bindless descriptor table,
    /// assigning and writing one on first call. Returns the slot. Growing the
    /// table past its capacity resizes it (keeping existing entries).
    uint32_t RegisterBindlessTexture(Texture &texture);

    /// @brief Populates @p material's textures + constants from @p data, assigning
    /// it @p id.
    void BuildMaterial(Material &material, const Geometry::MaterialData &data, uint32_t id);

    /// @brief (Re)build the id-0 fallback material from the engine defaults.
    void BuildFallbackMaterial();

    // A texture cache key: the same file may be resident once per colour space.
    struct TextureKey
    {
        Core::AssetPath path;
        ColorSpace      space;
        bool operator==(const TextureKey &other) const { return space == other.space && path == other.path; }
    };
    struct TextureKeyHash
    {
        std::size_t operator()(const TextureKey &key) const
        {
            return std::hash<Core::AssetPath>{}(key.path) ^ (static_cast<std::size_t>(key.space) * 0x9e3779b9u);
        }
    };

    // A `prim://` solid-colour texture primitive descriptor.
    struct SolidColor
    {
        unsigned char r, g, b, a;
        ColorSpace    space;
    };

    nvrhi::IDevice *_device = nullptr;
    ColorSpace      _textureColorSpace = ColorSpace::Srgb;

    // Editor-installed id↔path translators (see SetAssetResolvers). Empty until an
    // editor wires them; reserved built-ins resolve without them.
    IdToPathFn _idToPath;
    PathToIdFn _pathToId;

    // Registered at Initialize(): `prim://` mesh factories and solid-colour
    // texture primitives (white / white-linear / flat-normal).
    std::unordered_map<Core::AssetPath, std::function<Geometry::MeshData()>> _primitiveFactories;
    std::unordered_map<Core::AssetPath, SolidColor>                          _texturePrimitives;

    // One shared vertex/index buffer every mesh sub-allocates a range from
    // (GPU-driven stage C). MeshBuffers hold ranges into this, not their own
    // buffers; Clear() resets it. Reset by Clear() (wholesale free).
    GeometryArena _arena;

    // Bindless material-texture table (GPU-driven stage D). Every resolved
    // Texture is written here once and keys materials by index. The handles are
    // created at Initialize and stay stable across Clear() (only _nextBindlessSlot
    // resets and the default textures re-register), so MeshPass binds them once.
    nvrhi::BindingLayoutHandle   _bindlessLayout;
    nvrhi::DescriptorTableHandle _bindlessTable;
    uint32_t                     _bindlessCapacity = 0;
    uint32_t                     _nextBindlessSlot = 0;

    std::unordered_map<Core::AssetPath, MeshBuffer>         _meshes;
    std::unordered_map<TextureKey, Texture, TextureKeyHash> _textures;
    std::unordered_map<Core::AssetPath, Material>           _materials; // .amat files

    // The id-0 fallback; rebuilt on Clear (its texture pointers would dangle).
    Material _fallbackMaterial;

    // Monotonic, process-lifetime mesh id, assigned to each MeshBuffer at upload.
    // NOT reset by Clear() (symmetric with _nextMaterialId): a re-resolved mesh
    // gets a fresh id, an old id is never reused — so the draw sort key stays
    // unambiguous across a level reload.
    uint32_t _nextMeshId = 1;

    // Monotonic, process-lifetime material id. NOT reset by Clear(), so an id is
    // never reused — a stale binding-set entry keyed on it is dead, never wrong.
    // Starts at 1; id 0 is reserved for the fallback material.
    uint32_t _nextMaterialId = 1;

    // Mesh paths that failed to load (see ResolveMesh): warn once, and don't
    // re-parse a broken path every frame. Reset by Clear().
    std::unordered_set<Core::AssetPath> _missingMeshWarned;
    // Material paths that failed to load/parse: same rationale.
    std::unordered_set<Core::AssetPath> _missingMaterialWarned;
};
} /* namespace Assisi::Render */
