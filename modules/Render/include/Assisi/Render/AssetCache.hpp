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

#include <array>
#include <optional>
#include <expected>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nvrhi/nvrhi.h>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/AssetPath.hpp>
#include <Assisi/Core/JobSystem.hpp>
#include <Assisi/Geometry/MaterialData.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Geometry/MeshImporter.hpp>
#include <Assisi/Render/Buffer.hpp>
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

    /// @brief Bind to a device + job system, register the built-in `prim://` mesh
    /// and texture primitives, and build the fallback material. Must be called
    /// before any Resolve* call. @p jobs drives async mesh/material loading (mesh
    /// import + image decode run on its workers; GPU upload + publish on the main
    /// thread — see ResolveMesh/ResolveMaterial). @p textureColorSpace is the
    /// default colour space for ResolveTexture calls that don't specify one — Srgb
    /// for scene albedo, Linear for textures shown straight through ImGui.
    void Initialize(nvrhi::IDevice *device, Core::JobSystem *jobs, ColorSpace textureColorSpace = ColorSpace::Srgb);

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

    /// @brief Resolves a mesh id to a cached MeshBuffer. Built-in primitives (e.g.
    /// `prim://cube`) resolve synchronously. A mesh **file** imports on a worker
    /// thread: the first resolve kicks the load and returns **nullptr** ("still
    /// loading" — the caller shows a placeholder); once the import finishes and
    /// uploads (on the main thread), subsequent resolves return the buffer. A
    /// nil/unknown id, or a file that fails to load, resolves to the unit cube. The
    /// pointer stays valid until Clear(). Poll again (see HasPendingLoads) until a
    /// loading mesh becomes non-null.
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

    /// @brief Resolves a texture path to an editor thumbnail, decoded on a worker
    /// thread. Unlike ResolveTexture (synchronous, and bindless-registered for the
    /// scene's materials), thumbnails live in their own cache: the first resolve
    /// kicks a worker decode and returns **nullptr** ("still loading" — the browser
    /// shows a placeholder tile); once decoded and uploaded on the main thread a
    /// later resolve returns the Texture. A path that fails to decode returns null
    /// permanently (the failure is remembered, not retried every frame). Thumbnails
    /// are never bindless-registered (they display through ImGui, they aren't
    /// sampled by materials). Always Linear colour space. Falls back to a synchronous
    /// load when no job system is bound (e.g. a headless test).
    const Texture *ResolveThumbnail(const Core::AssetPath &path);

    /// @brief Whether a thumbnail decode for @p path is currently in flight. A tile
    /// that ResolveThumbnail returned null for is either still decoding (true) or
    /// failed to decode (false) — the browser uses this to show a loading indicator
    /// only while the decode is actually pending.
    bool IsThumbnailLoading(const Core::AssetPath &path) const
    {
        return _thumbnailLoading.find(path) != _thumbnailLoading.end();
    }

    /// @brief Invoked once per resident thumbnail about to be freed (see
    /// ClearThumbnails), with the texture still valid, so the caller can drop any
    /// external reference to it — chiefly the ImGui descriptor-set binding the
    /// editor made via DebugUI::GetOrCreateTextureId. Without this, a freed
    /// `nvrhi::ITexture` whose address a later texture reuses would alias a stale
    /// ImGui binding.
    using ThumbnailReleaseFn = std::function<void(nvrhi::ITexture *)>;

    /// @brief Drop every resident thumbnail and cancel in-flight thumbnail decodes
    /// (their publishes become no-ops). Call on asset-browser directory change /
    /// refresh so browsing many folders doesn't grow VRAM without bound. Waits for
    /// the GPU to idle before freeing (a navigation-time stall, not per frame), and
    /// invokes @p onRelease for each texture first so the caller can release its
    /// ImGui binding. Leaves the scene/material texture cache (_textures) untouched.
    void ClearThumbnails(const ThumbnailReleaseFn &onRelease = {});

    /// @brief Resolves a `.amat` material id to a cached Material. The `.amat` is
    /// parsed on the main thread; its **images decode on a worker thread**, so the
    /// first resolve kicks the load and returns the neutral **fallback** material
    /// meanwhile — a model whose material isn't ready yet renders with the fallback
    /// white look. Once the material's images are decoded and uploaded (publish, on
    /// the main thread) the real material is built and later resolves return it. A
    /// nil/unknown id, or a file that fails to load/parse, stays on the fallback.
    /// Never null; the pointer stays valid until Clear(). Poll again (see
    /// HasPendingLoads) until a loading material upgrades from the fallback.
    const Material *ResolveMaterial(const Core::AssetId &id);

    /// @brief The neutral fallback material (id 0): white albedo, metallic 0,
    /// roughness 0.6 — the engine's pre-material look, also shown while a real
    /// material is still loading. Never null.
    const Material *FallbackMaterial() const { return &_fallbackMaterial; }

    /// @brief Whether any async mesh/material load is still in flight. The app
    /// re-resolves its scene's MeshRenderers each frame while this is true, so
    /// components upgrade (placeholder→mesh, fallback→real material) as loads land.
    bool HasPendingLoads() const { return !_meshLoading.empty() || !_materialLoading.empty(); }

    /// @brief How many async mesh/material loads are still in flight. Cache-wide
    /// (not per scene). Lets a preload show a streaming-progress percentage by
    /// comparing against the count captured when its resolve began.
    std::size_t PendingLoadCount() const { return _meshLoading.size() + _materialLoading.size(); }

    /// @brief Cap on how many mesh/material loads decode concurrently. Streaming a
    /// whole level fans out one job per asset; letting them all run saturates
    /// every worker core in a burst and starves the main render thread (frame
    /// spikes during a load). Capping below the pool size leaves cores for
    /// rendering and physics, spreading the work out. Excess loads queue and start
    /// as running ones finish. Clamped to at least 1.
    void SetMaxConcurrentLoads(std::size_t n) { _maxConcurrentLoads = n < 1 ? 1 : n; }
    [[nodiscard]] std::size_t GetMaxConcurrentLoads() const { return _maxConcurrentLoads; }

    /// @brief The bindless material-texture descriptor table and its layout
    /// (GPU-driven stage D). Every resolved texture holds a slot here; materials
    /// reference channels by index. The MeshPass adds the layout to its pipeline
    /// (register space 1) and binds the table each draw. Stable across Clear()
    /// (the contents reset, the handles do not), so consumers bind once.
    nvrhi::IBindingLayout *BindlessLayout() const { return _bindlessLayout; }
    nvrhi::IDescriptorTable *BindlessTable() const { return _bindlessTable; }

    /// @brief The shared material table (GPU-driven stage D): a structured buffer
    /// whose row `Material::Id()` holds that material's MaterialConstants. Each
    /// per-instance record carries its material's id; the shader fetches the row
    /// by that index, so no per-material binding set is needed. Fixed capacity, so
    /// the handle is stable across Clear() (only the rows are rewritten) — the
    /// MeshPass binds it once. Never null after Initialize().
    nvrhi::IBuffer *MaterialTableBuffer() const { return _materialTable.NativeBuffer(); }

    /// @brief Row count of the material table. A material id IS its row, and the
    /// shaders use it as an unchecked index into an SSBO runtime array, so every
    /// id handed to a draw must be < this. See MintMaterialId.
    static constexpr uint32_t kMaxMaterials = 4096u;

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
    /// it @p id, then writes its row into the material table.
    void BuildMaterial(Material &material, const Geometry::MaterialData &data, uint32_t id);

    /// @brief Hands out the next material-table row, saturating at row 0 (the
    /// fallback) once the table is full. Ids are used as bindless indices by the
    /// shaders, so an id past the table would be an out-of-bounds GPU read, not a
    /// dropped row — never mint one by incrementing _nextMaterialId directly.
    uint32_t MintMaterialId();

    /// @brief Writes @p material's constants into row Material::Id() of the material
    /// table, uploading the dense prefix. Called whenever a material is (re)built.
    void WriteMaterialToTable(const Material &material);

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
    uint32_t                     _nextBindlessSlot = 0;

    // Latches when the table fills so RegisterBindlessTexture warns once rather
    // than per texture. Reset by Clear() alongside _nextBindlessSlot.
    bool _bindlessTableFull = false;

    // Material table (GPU-driven stage D): row `Material::Id()` holds a material's
    // MaterialConstants. Fixed capacity (stable handle across Clear, so MeshPass
    // binds it once). Rows are written one at a time at build time, each at its own
    // offset (see WriteMaterialToTable) — never a whole-prefix re-upload, which
    // would race in-flight frames reading other rows and flicker the scene.
    Buffer _materialTable;

    std::unordered_map<Core::AssetPath, MeshBuffer>         _meshes;
    std::unordered_map<TextureKey, Texture, TextureKeyHash> _textures;
    std::unordered_map<Core::AssetPath, Material>           _materials; // .amat files

    // Editor asset-browser thumbnails, decoded asynchronously (see ResolveThumbnail).
    // Kept apart from _textures so they never take a bindless slot (they display
    // through ImGui, not the material path). Path-keyed: thumbnails are always
    // Linear, so no colour-space dimension. A default-constructed (invalid) entry
    // marks a decode that failed, so a broken file isn't re-kicked every frame.
    std::unordered_map<Core::AssetPath, Texture> _thumbnails;
    std::unordered_set<Core::AssetPath>          _thumbnailLoading;
    // Bumped when the thumbnail set is invalidated; an in-flight decode whose
    // captured epoch no longer matches is dropped on publish. Atomic because the
    // worker reads it (to bail early); the authoritative drop is on the main thread.
    std::atomic<uint64_t> _thumbnailEpoch = 0;

    // The id-0 fallback; rebuilt on Clear (its texture pointers would dangle).
    Material _fallbackMaterial;

    // Monotonic, process-lifetime mesh id, assigned to each MeshBuffer at upload.
    // NOT reset by Clear() (symmetric with _nextMaterialId): a re-resolved mesh
    // gets a fresh id, an old id is never reused — so the draw sort key stays
    // unambiguous across a level reload.
    uint32_t _nextMeshId = 1;

    // Next material-table slot to hand out. Dense and RESET by Clear() (unlike the
    // mesh id): a material's id is now its row in the material table, so ids must
    // stay compact and start over with each asset set. Starts at 1; id 0 is the
    // fallback material's row. The retired per-material binding-set cache was the
    // only thing that needed ids to never repeat; the material table replaced it.
    uint32_t _nextMaterialId = 1;

    // Latches once the table fills so MintMaterialId warns a single time instead
    // of every subsequent material. Reset by Clear() alongside _nextMaterialId.
    bool _materialTableFull = false;

    // Mesh paths that failed to load (see ResolveMesh): warn once, and don't
    // re-parse a broken path every frame. Reset by Clear().
    std::unordered_set<Core::AssetPath> _missingMeshWarned;
    // Material paths that failed to load/parse: same rationale.
    std::unordered_set<Core::AssetPath> _missingMaterialWarned;

    // --- Async loading (workers decode/import; the main thread publishes) -------
    // AssetCache is only mutated on the main thread: worker jobs run pure CPU
    // (ImportMesh / Texture::DecodeImage) over copied inputs and touch no cache
    // state; their results publish back via _jobs->RunOnMain, so no locks are
    // needed. Non-owning; the app (Application) owns the job system and outlives
    // the cache.
    Core::JobSystem *_jobs = nullptr;

    // Bumped by Clear(). Every load captures the epoch at kick time; a publish
    // whose epoch no longer matches is dropped — that cancels in-flight loads from
    // a level that has since been unloaded (their decoded data is discarded and no
    // GPU resource is created). Atomic because the *worker* also reads it, to bail
    // before doing the expensive import when its epoch has already been superseded
    // (a queued-but-not-started stale load, common when the Load button is spammed);
    // the authoritative drop still happens in the main-thread continuation.
    std::atomic<uint64_t> _loadEpoch = 0;

    // Paths with a load job in flight — so a re-resolve while loading returns the
    // placeholder (null mesh / fallback material) without re-kicking, and
    // HasPendingLoads() knows work is outstanding. Cleared by Clear() (the epoch
    // bump makes the in-flight publishes no-ops).
    std::unordered_set<Core::AssetPath> _meshLoading;
    std::unordered_set<Core::AssetPath> _materialLoading;

    // --- In-flight load throttle (see SetMaxConcurrentLoads) --------------------
    // A load is *queued* the moment it is requested (its path enters
    // _meshLoading/_materialLoading so HasPendingLoads and the re-resolve loop see
    // it), but its worker job only starts once fewer than _maxConcurrentLoads are
    // already running. Excess loads wait in _pendingLoads and start as running ones
    // publish. Bounds concurrent CPU decode work so a big load can't saturate every
    // worker core and starve the main render thread (frame spikes).

    /// @brief One PBR channel's decoded pixels plus its source path (for dedup at
    /// publish). Nested so OnMaterialLoaded can take a MaterialLoadBundle by value.
    struct DecodedChannel
    {
        std::optional<DecodedImage> image;
        Core::AssetPath             path;
    };
    /// @brief A material load's result: parsed data + every channel decoded.
    struct MaterialLoadBundle
    {
        Geometry::MaterialData        data;
        std::array<DecodedChannel, 5> channels;
    };

    /// @brief A load queued but not yet started — stored as data (not a thunk) so
    /// PumpLoadQueue dispatches it by kind. Only the fields for its kind are used.
    struct PendingLoad
    {
        bool                           isMaterial = false;
        Core::AssetPath                path;
        std::uint64_t                  epoch = 0;
        PathToIdFn                     pathToId;     ///< mesh only
        Geometry::MaterialData         materialData; ///< material only
        std::array<Core::AssetPath, 5> channelPaths; ///< material only
    };

    std::size_t             _maxConcurrentLoads = 3;
    std::size_t             _activeLoads        = 0;
    std::deque<PendingLoad> _pendingLoads;

    /// @brief Starts queued loads up to the concurrency cap. Called when a load is
    /// queued and again after each running load publishes. Main thread only.
    void PumpLoadQueue();
    /// @brief Wires up a mesh import job (worker) + its publish (OnMeshLoaded).
    void StartMeshLoad(Core::AssetPath path, PathToIdFn pathToId, std::uint64_t epoch);
    /// @brief Wires up a material decode job (worker) + its publish (OnMaterialLoaded).
    void StartMaterialLoad(Core::AssetPath path, Geometry::MaterialData data,
                           std::array<Core::AssetPath, 5> channelPaths, std::uint64_t epoch);
    /// @brief Main-thread publish of a finished mesh import: free the load slot,
    /// then upload + cache the buffer (or record the failure).
    void OnMeshLoaded(Core::AssetPath path, std::uint64_t epoch,
                      std::expected<Geometry::MeshData, Geometry::MeshImportError> imported);
    /// @brief Main-thread publish of a finished material decode: free the load slot,
    /// then upload each channel's texture, build the material, and write its table row.
    void OnMaterialLoaded(Core::AssetPath path, std::uint64_t epoch, MaterialLoadBundle bundle);
    /// @brief Worker-thread body of a material load: decode every channel image
    /// sequentially (a nested parallel-for would defeat the concurrency cap). Pure —
    /// touches no cache state, so it is safe off the main thread.
    static MaterialLoadBundle DecodeMaterialChannels(Geometry::MaterialData data,
                                                     std::array<Core::AssetPath, 5> channelPaths,
                                                     std::uint64_t epoch, const std::atomic<std::uint64_t> &loadEpoch);
};
} /* namespace Assisi::Render */
