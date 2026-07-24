/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Geometry/DefaultMeshes.hpp>
#include <Assisi/Geometry/MaterialFile.hpp>
#include <Assisi/Geometry/MeshImporter.hpp>
#include <Assisi/Render/AssetCache.hpp>

namespace Assisi::Render
{

namespace
{
/// @brief The fallback mesh path used for empty/unrecognised mesh references.
const Core::AssetPath kCubePrimitive{std::string_view{"prim://cube"}};

// `prim://` solid-colour texture primitives — a material's per-channel defaults,
// owned by the cache like any other engine-generated asset so a device rebuild
// regenerates them uniformly (see AssetCache.hpp). Each carries its own fixed
// colour space; the space a caller passes to ResolveTexture is ignored for these.
const Core::AssetPath kWhiteTexture{std::string_view{"prim://white"}};             // baseColor / emissive.
const Core::AssetPath kWhiteLinearTexture{std::string_view{"prim://white-linear"}}; // metallic-roughness / occlusion.
const Core::AssetPath kFlatNormalTexture{std::string_view{"prim://flat-normal"}};   // unperturbed tangent-space normal.

// Starting slot count for the bindless material-texture table; it grows past this
// on demand as more distinct textures resolve.
// Bindless texture-table capacity (slots). Fixed at table creation: nvrhi's
// Vulkan backend implements resizeDescriptorTable as an assert-only no-op, so
// the descriptor table's real capacity is whatever the layout was built with and
// can never change. Slots past this saturate onto slot 0 with a one-time warning.
constexpr uint32_t kBindlessCapacity = 16384u;

// Material-table capacity (rows). Fixed so the buffer handle is stable across
// Clear() and the MeshPass binds it once; generous enough for any real scene
// (the opaque sort key allows ~1M, but a level with thousands of *distinct*
// materials is unheard of). Materials past this saturate onto the fallback row
// (see MintMaterialId) rather than resizing the buffer — keeping the handle
// stable matters more than the ceiling, and at 96 B/row raising the ceiling is
// cheap (the whole table is 384 KB) if a scene ever needs it.
// (The capacity constant lives on AssetCache so MeshPass can assert against it.
// No file-local alias: inside AssetCache's own member functions, unqualified
// kMaxMaterials already resolves to the class member.)

// The five PBR texture channels, in MaterialTextures order (base, normal,
// metallic-roughness, occlusion, emissive). Each pairs the colour space the
// channel decodes in with the `prim://` default that stands in for an empty or
// failed channel; `isNormal` flags the one whose presence drives the shader's
// normal-mapping bit.
struct ChannelDesc
{
    ColorSpace             space;
    const Core::AssetPath *fallback;
    bool                   isNormal;
};

const std::array<ChannelDesc, 5> kChannels = {{
    {ColorSpace::Srgb, &kWhiteTexture, false},        // baseColor
    {ColorSpace::Linear, &kFlatNormalTexture, true},  // normal
    {ColorSpace::Linear, &kWhiteLinearTexture, false}, // metallic-roughness
    {ColorSpace::Linear, &kWhiteLinearTexture, false}, // occlusion
    {ColorSpace::Srgb, &kWhiteTexture, false},         // emissive
}};

// Minimum staging chunk for the shared upload command list. A texture burst (a
// 2K RGBA8 + mips is ~22 MB) would otherwise fragment into dozens of the default
// 64 KB chunks, each its own vkAllocateMemory; one big chunk absorbs a whole
// material's channels. Peak staging is reclaimed by recreating the list on Clear.
constexpr uint64_t kUploadChunkSize = 16ull << 20; // 16 MB

Core::AssetId ChannelId(const Geometry::MaterialData &data, std::size_t channel)
{
    switch (channel)
    {
    case 0: return data.BaseColorTexture;
    case 1: return data.NormalTexture;
    case 2: return data.MetallicRoughnessTexture;
    case 3: return data.OcclusionTexture;
    default: return data.EmissiveTexture;
    }
}

// (DecodedChannel and MaterialLoadBundle are private nested types of AssetCache —
// OnMaterialLoaded takes a MaterialLoadBundle by value, so they live in the header.)
} // namespace

void AssetCache::Initialize(nvrhi::IDevice *device, Core::JobSystem *jobs, ColorSpace textureColorSpace)
{
    _device = device;
    _jobs = jobs;
    _textureColorSpace = textureColorSpace;

    // The shared geometry arena's single vertex format is Geometry::Vertex.
    _arena.Initialize(_device, sizeof(Geometry::Vertex));

    // One persistent command list every streaming publish records into, submitted
    // once per PumpPublishes (P0). A large staging chunk keeps a texture burst from
    // fragmenting into many small GPU allocations (see kUploadChunkSize).
    nvrhi::CommandListParameters uploadParams;
    uploadParams.uploadChunkSize = kUploadChunkSize;
    _uploadList                  = _device->createCommandList(uploadParams);
    _uploadOpen                  = false;

    // Bindless material-texture table (GPU-driven stage D): a Pixel-visible
    // Texture_SRV array in its own register space. Every resolved texture takes a
    // slot; materials reference channels by index. Capacity is fixed here at
    // creation — nvrhi's Vulkan resizeDescriptorTable is an assert-only no-op, so
    // the table can never actually grow (see RegisterBindlessTexture).
    nvrhi::BindlessLayoutDesc bindlessDesc;
    bindlessDesc.visibility = nvrhi::ShaderType::Pixel;
    bindlessDesc.firstSlot = 0;
    bindlessDesc.maxCapacity = kBindlessCapacity;
    bindlessDesc.addRegisterSpace(nvrhi::BindingLayoutItem::Texture_SRV(0));
    _bindlessLayout = _device->createBindlessLayout(bindlessDesc);
    _bindlessTable = _device->createDescriptorTable(_bindlessLayout);
    _nextBindlessSlot = 0;

    // Material table (GPU-driven stage D): one MaterialConstants row per material,
    // indexed by Material::Id(). Fixed capacity — the handle stays put across
    // Clear() so MeshPass binds it once; BuildFallbackMaterial fills row 0 below.
    _materialTable.Create(_device, sizeof(MaterialConstants), kMaxMaterials, /*allowUnorderedAccess=*/false,
                          "AssetCache::MaterialTable");

    _primitiveFactories.emplace(kCubePrimitive, &Geometry::CreateUnitCubeMesh);

    // White sRGB stands in for empty baseColor/emissive channels; white *linear*
    // for metallic-roughness/occlusion (sampling 1.0 leaves the per-material
    // factor untouched — glTF "no texture" semantics); flat normal (128,128,255)
    // linear decodes to +Z, i.e. no perturbation.
    _texturePrimitives.emplace(kWhiteTexture, SolidColor{255, 255, 255, 255, ColorSpace::Srgb});
    _texturePrimitives.emplace(kWhiteLinearTexture, SolidColor{255, 255, 255, 255, ColorSpace::Linear});
    _texturePrimitives.emplace(kFlatNormalTexture, SolidColor{128, 128, 255, 255, ColorSpace::Linear});

    BuildFallbackMaterial();
}

const MeshBuffer *AssetCache::ResolvePrimitive(const Core::AssetPath &path)
{
    if (std::unordered_map<Core::AssetPath, MeshBuffer>::iterator it = _meshes.find(path); it != _meshes.end())
        return &it->second;

    std::unordered_map<Core::AssetPath, std::function<Geometry::MeshData()>>::iterator factory =
        _primitiveFactories.find(path);
    if (factory == _primitiveFactories.end())
        return nullptr;

    MeshBuffer &buffer = _meshes[path];
    buffer.Upload(_arena, factory->second());
    buffer.SetId(_nextMeshId++);
    return &buffer;
}

Core::AssetPath AssetCache::PathForId(const Core::AssetId &id) const
{
    if (id.IsNil())
        return {};

    // Reserved built-ins map to their `prim://` path from the static table, so
    // the primitives resolve even before (or without) an editor database.
    if (id.IsReserved())
    {
        for (const Core::BuiltinAssetEntry &entry : Core::BuiltinAssets())
            if (entry.id == id)
                return Core::AssetPath{entry.virtualPath};
        return {};
    }

    return _idToPath ? _idToPath(id) : Core::AssetPath{};
}

const MeshBuffer *AssetCache::ResolveMesh(const Core::AssetId &id)
{
    return ResolveMeshPath(PathForId(id));
}

const MeshBuffer *AssetCache::ResolveMeshPath(const Core::AssetPath &path)
{
    // ResolvePrimitive also serves the mesh cache: any path already uploaded
    // (primitive or file) is returned here on subsequent frames. Primitives are
    // generated in-process and cheap, so they stay synchronous.
    if (const MeshBuffer *mesh = ResolvePrimitive(path))
        return mesh;

    // An empty path is the "unset" default (never a file); a path that already
    // failed to import falls back to the cube and isn't retried.
    if (path.Empty() || _missingMeshWarned.contains(path))
        return ResolvePrimitive(kCubePrimitive);

    // A mesh file. If a load is already in flight, report "still loading" (null) so
    // the caller shows a placeholder; the re-resolve loop will pick up the buffer
    // once it's uploaded. Otherwise kick the import: assimp runs on a worker (pure
    // CPU over the path + a copy of the path→id resolver), then the arena upload
    // and cache insert publish back on the main thread.
    if (_meshLoading.contains(path))
        return nullptr;

    // Queue the load as data; PumpLoadQueue starts it only when fewer than
    // _maxConcurrentLoads are running, so a whole level's worth of loads don't all
    // decode at once and starve the main thread. The path enters _meshLoading now
    // (queued counts as pending) so the re-resolve loop won't re-request it.
    _meshLoading.insert(path);
    _pendingLoads.push_back(PendingLoad{.isMaterial = false,
                                        .path       = path,
                                        .epoch      = _loadEpoch.load(std::memory_order_relaxed),
                                        .pathToId   = _pathToId}); // copy: the worker must not touch cache state
    PumpLoadQueue();

    return nullptr; // loading — placeholder for now
}

const Texture *AssetCache::ResolveTexture(const Core::AssetPath &path, ColorSpace colorSpace)
{
    if (path.Empty())
        return nullptr;

    // A `prim://` texture primitive: its own fixed colour space wins over the
    // caller's, so the same primitive is only ever resident once.
    if (std::unordered_map<Core::AssetPath, SolidColor>::iterator prim = _texturePrimitives.find(path);
        prim != _texturePrimitives.end())
    {
        const SolidColor &color = prim->second;
        TextureKey        key{path, color.space};
        if (std::unordered_map<TextureKey, Texture, TextureKeyHash>::iterator it = _textures.find(key);
            it != _textures.end())
            return it->second.IsValid() ? &it->second : nullptr;

        Texture &texture = _textures[key];
        texture.UploadSolidColor(_device, color.r, color.g, color.b, color.a, color.space, path.View().data());
        RegisterBindlessTexture(texture);
        return &texture;
    }

    TextureKey key{path, colorSpace};
    if (std::unordered_map<TextureKey, Texture, TextureKeyHash>::iterator it = _textures.find(key);
        it != _textures.end())
        return it->second.IsValid() ? &it->second : nullptr;

    Texture &texture = _textures[key];
    if (std::expected<void, Core::AssetError> loaded = texture.LoadFromAssets(_device, path.View(), colorSpace);
        !loaded)
    {
        Core::Log::Warn("AssetCache: failed to load texture '{}' — the channel falls back to its default.",
                        path.View());
        // Keep the invalid entry so the failed load isn't retried every frame;
        // returning null lets the material substitute its channel default.
        return nullptr;
    }
    RegisterBindlessTexture(texture);
    return &texture;
}

const Texture *AssetCache::ResolveThumbnail(const Core::AssetPath &path)
{
    if (path.Empty())
        return nullptr;

    // Resident already: a valid entry is ready to show; an invalid one records a
    // decode that failed, so we don't re-kick a broken file every frame.
    if (std::unordered_map<Core::AssetPath, Texture>::iterator it = _thumbnails.find(path);
        it != _thumbnails.end())
        return it->second.IsValid() ? &it->second : nullptr;

    // A decode for this path is already in flight — show the placeholder until it
    // publishes, without kicking a duplicate.
    if (_thumbnailLoading.find(path) != _thumbnailLoading.end())
        return nullptr;

    // No job system (e.g. a headless test): degrade to a synchronous load rather
    // than a thumbnail that never resolves.
    if (_jobs == nullptr)
    {
        Texture &texture = _thumbnails[path];
        if (std::expected<void, Core::AssetError> loaded =
                texture.LoadFromAssets(_device, path.View(), ColorSpace::Linear);
            !loaded)
            return nullptr; // keep the invalid entry so a broken file isn't retried
        return &texture;
    }

    _thumbnailLoading.insert(path);
    const uint64_t               epoch      = _thumbnailEpoch.load(std::memory_order_relaxed);
    const std::atomic<uint64_t> *thumbEpoch = &_thumbnailEpoch; // worker reads it to bail early (read-only)
    const std::string            vpath(path.View());

    _jobs
        ->Run(Core::Pool::Worker,
              [vpath, epoch, thumbEpoch]() -> std::expected<DecodedImage, Core::AssetError> {
                  // Skip the decode if a directory change already superseded this
                  // thumbnail (the main-thread publish drops it regardless; this just
                  // avoids the wasted work when browsing folders quickly). The error
                  // value is never inspected — the continuation returns on the epoch
                  // mismatch before it looks at the result.
                  if (thumbEpoch->load(std::memory_order_relaxed) != epoch)
                      return std::unexpected(Core::AssetError::FileReadFailed);
                  return Texture::DecodeImage(vpath, ColorSpace::Linear);
              })
        .Then(Core::Pool::Main, [this, path, epoch](std::expected<DecodedImage, Core::AssetError> decoded) {
            if (epoch != _thumbnailEpoch.load(std::memory_order_relaxed))
                return; // superseded (see the mesh path's twin): return before erasing so a stale
                        // completion can't drop a live epoch's loading marker and re-kick a load.
            _thumbnailLoading.erase(path);
            if (!decoded)
            {
                // Remember the failure (a default-constructed, invalid entry) so a
                // broken file isn't re-kicked every frame.
                (void)_thumbnails[path];
                return;
            }
            Texture &texture = _thumbnails[path];
            texture.UploadDecoded(_device, *decoded, std::string(path.View()).c_str());
        });

    return nullptr; // loading — the browser shows a placeholder tile this frame
}

void AssetCache::ClearThumbnails(const ThumbnailReleaseFn &onRelease)
{
    // Drain the GPU before freeing: a thumbnail texture may still be sampled by an
    // in-flight frame's ImGui draw. Navigation is a user action, not a per-frame
    // path, so this stall is fine (unlike an LRU cap, which would need deferred
    // frees). With the GPU idle, onRelease can safely drop each texture's ImGui
    // descriptor-set binding before we destroy it.
    _device->waitForIdle();
    if (onRelease)
        for (std::pair<const Core::AssetPath, Texture> &entry : _thumbnails)
            if (entry.second.IsValid())
                onRelease(entry.second.NativeTexture());

    // Cancel in-flight decodes (their publishes see the bumped epoch and no-op),
    // then drop every resident thumbnail.
    ++_thumbnailEpoch;
    _thumbnailLoading.clear();
    _thumbnails.clear();
}

uint32_t AssetCache::RegisterBindlessTexture(Texture &texture)
{
    if (texture.BindlessIndex() != Texture::kInvalidBindlessIndex)
        return texture.BindlessIndex();

    // The table's real capacity is fixed at creation to the layout's maxCapacity;
    // nvrhi's Vulkan resizeDescriptorTable is an assert-only no-op, so there is no
    // growing to do — the ceiling below IS the table.
    if (_nextBindlessSlot >= kBindlessCapacity)
    {
        if (!_bindlessTableFull)
        {
            _bindlessTableFull = true;
            Core::Log::Warn("AssetCache: bindless texture table full ({} slots); further textures fall back to "
                            "slot 0 and will render with the wrong texture.",
                            kBindlessCapacity);
        }
        // Slot 0 is always written (the first texture resolved is a default), so
        // this is wrong-looking but defined — unlike recording a slot that was
        // never written, which samples undefined descriptor memory.
        texture.SetBindlessIndex(0u);
        return 0u;
    }

    const uint32_t slot = _nextBindlessSlot;
    if (!_device->writeDescriptorTable(_bindlessTable,
                                       nvrhi::BindingSetItem::Texture_SRV(slot, texture.NativeTexture())))
    {
        // Refused by the backend (capacity exceeded). Do NOT record the slot: the
        // descriptor at that index was never written, so sampling it is undefined.
        Core::Log::Error("AssetCache: writeDescriptorTable rejected slot {}; texture left unbound.", slot);
        texture.SetBindlessIndex(0u);
        return 0u;
    }

    ++_nextBindlessSlot;
    texture.SetBindlessIndex(slot);
    return slot;
}

uint32_t AssetCache::ResolveChannel(const Core::AssetId &channelId, ColorSpace space,
                                    const Core::AssetPath &fallbackPrimitive, bool *outPresent)
{
    const Core::AssetPath path = PathForId(channelId);
    if (!path.Empty())
    {
        if (const Texture *texture = ResolveTexture(path, space))
        {
            if (outPresent != nullptr)
                *outPresent = true;
            return texture->BindlessIndex();
        }
    }

    if (outPresent != nullptr)
        *outPresent = false;
    // The primitive dictates its own colour space; `space` here is a harmless hint.
    const Texture *fallback = ResolveTexture(fallbackPrimitive, space);
    return fallback != nullptr ? fallback->BindlessIndex() : 0u;
}

void AssetCache::BuildMaterial(Material &material, const Geometry::MaterialData &data, uint32_t id)
{
    MaterialTextures textures;
    textures.baseColor = ResolveChannel(data.BaseColorTexture, ColorSpace::Srgb, kWhiteTexture);
    textures.normal =
        ResolveChannel(data.NormalTexture, ColorSpace::Linear, kFlatNormalTexture, &textures.hasNormalTexture);
    textures.metallicRoughness = ResolveChannel(data.MetallicRoughnessTexture, ColorSpace::Linear, kWhiteLinearTexture);
    textures.occlusion = ResolveChannel(data.OcclusionTexture, ColorSpace::Linear, kWhiteLinearTexture);
    textures.emissive = ResolveChannel(data.EmissiveTexture, ColorSpace::Srgb, kWhiteTexture);

    material.Create(_device, id, data, textures);
    WriteMaterialToTable(material);
}

uint32_t AssetCache::MintMaterialId()
{
    // Ids index the bindless material table directly (the vertex shader passes
    // the id straight through to `materials[vMaterialIndex]`, an unbounded GLSL
    // runtime array), so an id past the table is not a dropped row — it is an
    // out-of-bounds GPU read. Saturate onto row 0, the fallback material, so
    // overflow degrades to "wrong but defined" instead of undefined behavior.
    if (_nextMaterialId >= kMaxMaterials)
    {
        if (!_materialTableFull)
        {
            _materialTableFull = true;
            Core::Log::Warn("AssetCache: material table full ({} rows); further materials render with the "
                            "fallback material.",
                            kMaxMaterials);
        }
        return 0u;
    }
    return _nextMaterialId++;
}

void AssetCache::WriteMaterialToTable(const Material &material, nvrhi::ICommandList *sharedList)
{
    const uint32_t id = material.Id();
    if (id >= kMaxMaterials)
    {
        // MintMaterialId saturates, so this is unreachable via the normal path;
        // it stays as a guard for ids arriving from anywhere else.
        Core::Log::Warn("AssetCache: material id {} exceeds the material table capacity ({}); row dropped.", id,
                        kMaxMaterials);
        return;
    }

    // Write ONLY this material's row, at its own offset — never re-upload the whole
    // table. Materials build incrementally across frames during an async load; a
    // full-prefix re-upload would rewrite every already-resident material's row and
    // race the in-flight frame's reads of the table, which flickers the whole scene.
    // This row is brand new (no draw references this material yet — loading entities
    // use the fallback), so writing it touches no bytes an in-flight frame reads.
    const MaterialConstants row    = material.Constants();
    const uint64_t          offset = static_cast<uint64_t>(id) * sizeof(MaterialConstants);
    if (sharedList != nullptr)
    {
        // Async publish: record into the caller's shared list (PumpPublishes submits).
        sharedList->writeBuffer(_materialTable.NativeBuffer(), &row, sizeof(row), offset);
        return;
    }
    // Synchronous fallback-material build: self-contained command list.
    nvrhi::CommandListHandle commandList = _device->createCommandList();
    commandList->open();
    commandList->writeBuffer(_materialTable.NativeBuffer(), &row, sizeof(row), offset);
    commandList->close();
    _device->executeCommandList(commandList);
}

void AssetCache::BuildFallbackMaterial()
{
    // The engine's pre-material look — deliberately different from MaterialData's
    // glTF spec defaults (metallic 1 / roughness 1): white albedo, non-metallic,
    // moderately rough. All channels empty, so every texture resolves to a
    // primitive default.
    Geometry::MaterialData data;
    data.BaseColorFactor = glm::vec4(1.f, 1.f, 1.f, 1.f);
    data.MetallicFactor = 0.f;
    data.RoughnessFactor = 0.6f;
    data.Name = "Fallback";
    BuildMaterial(_fallbackMaterial, data, 0);
}

const Material *AssetCache::ResolveMaterial(const Core::AssetId &id)
{
    return ResolveMaterialPath(PathForId(id));
}

const Material *AssetCache::ResolveMaterialPath(const Core::AssetPath &path)
{
    if (path.Empty())
        return &_fallbackMaterial;

    if (std::unordered_map<Core::AssetPath, Material>::iterator it = _materials.find(path); it != _materials.end())
        return &it->second;

    // A broken/missing .amat, or one whose load is already in flight, resolves to
    // the fallback for now (a model whose material isn't ready renders white).
    if (_missingMaterialWarned.contains(path) || _materialLoading.contains(path))
        return &_fallbackMaterial;

    // Parse the .amat on the main thread — it's small, and resolving its channel
    // ids to paths needs the asset database (main-thread state). The expensive part
    // (decoding the channel images) then runs on a worker; the upload + build
    // publishes back on the main thread. Until then, the fallback stands in.
    std::expected<std::string, Core::AssetError> text = Core::AssetSystem::ReadText(path.View());
    if (!text)
    {
        _missingMaterialWarned.insert(path);
        Core::Log::Warn("AssetCache: cannot read material '{}' — using the fallback material.", path.View());
        return &_fallbackMaterial;
    }

    std::expected<Geometry::MaterialData, Geometry::MaterialFileError> data = Geometry::DeserializeMaterial(*text);
    if (!data)
    {
        _missingMaterialWarned.insert(path);
        Core::Log::Warn("AssetCache: material '{}' failed to parse ({}) — using the fallback material.", path.View(),
                        Geometry::ToString(data.error()));
        return &_fallbackMaterial;
    }

    // Resolve each channel's texture path now (needs the DB); empty channels stay
    // empty and resolve to a prim:// default at publish time.
    std::array<Core::AssetPath, 5> channelPaths;
    for (std::size_t ch = 0; ch < kChannels.size(); ++ch)
        channelPaths[ch] = PathForId(ChannelId(*data, ch));

    // Queue the load as data; PumpLoadQueue starts it under the concurrency cap.
    // The path enters _materialLoading now (queued counts as pending). The material
    // was already parsed above on the main thread (it needs the DB); only the
    // channel image decode is deferred to a worker.
    _materialLoading.insert(path);
    _pendingLoads.push_back(PendingLoad{.isMaterial   = true,
                                        .path         = path,
                                        .epoch        = _loadEpoch.load(std::memory_order_relaxed),
                                        .materialData = std::move(*data),
                                        .channelPaths = channelPaths});
    PumpLoadQueue();

    return &_fallbackMaterial; // fallback while loading
}

void AssetCache::PumpLoadQueue()
{
    // Start queued loads until the concurrency cap is hit. Called when a load is
    // queued and again from each load's publish, so a finished load makes room for
    // the next. Main-thread only — no locking needed.
    while (_activeLoads < _maxConcurrentLoads && !_pendingLoads.empty())
    {
        PendingLoad load = std::move(_pendingLoads.front());
        _pendingLoads.pop_front();
        ++_activeLoads;
        if (load.isMaterial)
            StartMaterialLoad(std::move(load.path), std::move(load.materialData), load.channelPaths, load.epoch);
        else
            StartMeshLoad(std::move(load.path), std::move(load.pathToId), load.epoch);
    }
}

std::expected<AssetCache::MeshLoadBundle, Geometry::MeshImportError>
AssetCache::ImportAndStageMesh(nvrhi::IDevice *device, Core::AssetPath path, const PathToIdFn &pathToId,
                               std::uint64_t epoch, const std::atomic<std::uint64_t> &loadEpoch)
{
    if (loadEpoch.load(std::memory_order_relaxed) != epoch)
    {
        Core::Log::Warn("AssetCache: cancelled superseded mesh load for '{}'.", path.View());
        return std::unexpected(Geometry::MeshImportError::Cancelled);
    }

    std::expected<Geometry::MeshData, Geometry::MeshImportError> imported = Geometry::ImportMesh(path.View(), pathToId);
    if (!imported)
    {
        return std::unexpected(imported.error());
    }

    // Normalize the degenerate no-submesh case here, on the worker, while the CPU
    // geometry is still around: the staged publish path has no vertices left to
    // derive a table from (and this is main-thread work the direct path would do).
    Geometry::EnsureSubMeshTables(*imported);

    MeshLoadBundle bundle;
    bundle.vertexCount = static_cast<uint32_t>(imported->Vertices.size());
    bundle.indexCount  = static_cast<uint32_t>(imported->Indices.size());

    // Copy the geometry into a GPU staging buffer HERE, on the worker — this is the
    // bulk memcpy that used to land on the main thread inside writeBuffer (O(mesh
    // bytes) mid-frame). The main-thread publish then only records a copy.
    const std::string name(path.View());
    bundle.staging = MeshBuffer::StageMeshGeometry(device, *imported, name.c_str());
    if (bundle.staging != nullptr)
    {
        // Staged successfully: release the CPU copies so the mesh's bytes aren't
        // resident twice. The metadata (tables, bounds) is what publish still needs.
        // Only on success — the fallback path below re-reads these.
        imported->Vertices.clear();
        imported->Vertices.shrink_to_fit();
        imported->Indices.clear();
        imported->Indices.shrink_to_fit();
    }
    bundle.data = std::move(*imported);
    return bundle;
}

void AssetCache::StartMeshLoad(Core::AssetPath path, PathToIdFn pathToId, std::uint64_t epoch)
{
    // Import + stage on a worker (touching no cache state), then publish on the main
    // thread. The worker bails early if a Clear() has superseded this epoch — saving
    // the parse for a load nobody awaits anymore.
    nvrhi::IDevice                   *device    = _device;
    const std::atomic<std::uint64_t> *loadEpoch = &_loadEpoch;
    _jobs
        ->Run(Core::Pool::Worker, [device, path, pathToId, epoch, loadEpoch]()
              { return ImportAndStageMesh(device, path, pathToId, epoch, *loadEpoch); })
        .Then(Core::Pool::Main, [this, path, epoch](std::expected<MeshLoadBundle, Geometry::MeshImportError> r)
              { OnMeshLoaded(path, epoch, std::move(r)); });
}

void AssetCache::OnMeshLoaded(Core::AssetPath path, std::uint64_t epoch,
                              std::expected<MeshLoadBundle, Geometry::MeshImportError> imported)
{
    // A started load has finished (success, failure, or stale): free its slot and
    // let the next queued load begin. The decode fan-out continues even though this
    // result's GPU upload is deferred to PumpPublishes.
    --_activeLoads;
    PumpLoadQueue();

    if (epoch != _loadEpoch.load(std::memory_order_relaxed))
        return; // Stale: the level that asked for this mesh has since unloaded. Return WITHOUT
                // erasing the loading marker — Clear() already dropped ours, and a current-epoch
                // job for the same path may own it now.
    if (!imported)
    {
        // Failure is terminal and has no GPU work — settle it here rather than
        // queuing a publish. Drop the loading marker so a later resolve falls back.
        _meshLoading.erase(path);
        _missingMeshWarned.insert(path);
        Core::Log::Warn("AssetCache: no mesh for '{}' ({}), falling back to prim://cube.", path.View(),
                        Geometry::ToString(imported.error()));
        return; // a later resolve returns the cube
    }
    // Enqueue the publish (O(1)); the path stays in _meshLoading until PublishMesh
    // makes it resident, so HasPendingLoads / re-resolve keep treating it as pending.
    const std::size_t bytes = static_cast<std::size_t>(imported->vertexCount) * sizeof(Geometry::Vertex) +
                              static_cast<std::size_t>(imported->indexCount) * sizeof(uint32_t);
    _pendingPublishes.push_back(PendingPublish{
        .isMaterial = false, .path = path, .epoch = epoch, .byteSize = bytes, .mesh = std::move(*imported)});
}

AssetCache::MaterialLoadBundle AssetCache::DecodeAndRecordMaterialChannels(
    nvrhi::IDevice *device, Geometry::MaterialData data, std::array<Core::AssetPath, 5> channelPaths,
    std::uint64_t epoch, const std::atomic<std::uint64_t> &loadEpoch)
{
    if (loadEpoch.load(std::memory_order_relaxed) != epoch)
        return {}; // superseded before the decode ran; the publish drops it anyway

    // Decode each channel sequentially — one worker per material (a nested
    // parallel-for would spread a single material across every pool core, exactly
    // the saturation the concurrency cap exists to prevent). For each decoded
    // channel, create its GPU texture and record its upload into ONE command list
    // on this worker: that is where the writeTexture staging memcpy happens, and
    // moving it here is the whole point of P1 (the main-thread CPU spike). An empty
    // or failed channel is left without a texture → the prim:// fallback at publish.
    MaterialLoadBundle bundle;
    bundle.data = std::move(data);

    nvrhi::CommandListHandle list; // created lazily on the first channel that decodes
    for (std::size_t ch = 0; ch < kChannels.size(); ++ch)
    {
        if (channelPaths[ch].Empty())
            continue;
        std::expected<DecodedImage, Core::AssetError> img =
            Texture::DecodeImage(channelPaths[ch].View(), kChannels[ch].space);
        if (!img)
            continue;

        if (!list)
        {
            nvrhi::CommandListParameters params;
            params.uploadChunkSize = kUploadChunkSize;
            list                   = device->createCommandList(params);
            list->open();
        }
        const std::string    name(channelPaths[ch].View());
        nvrhi::TextureHandle texture = Texture::CreateImage(device, *img, name.c_str());
        Texture::RecordMips(list, texture, *img);

        bundle.channels[ch].texture = texture;
        bundle.channels[ch].path    = channelPaths[ch];
        for (const std::vector<unsigned char> &mip : img->mips)
            bundle.decodedBytes += mip.size();
    }
    if (list)
    {
        list->close();
        bundle.uploadList = std::move(list);
    }
    return bundle;
}

void AssetCache::StartMaterialLoad(Core::AssetPath path, Geometry::MaterialData data,
                                   std::array<Core::AssetPath, 5> channelPaths, std::uint64_t epoch)
{
    // The material was parsed on the main thread (it needs the DB); the worker
    // decodes the channel images AND records their GPU uploads (P1). The main thread
    // then only submits the recorded list, adopts the textures, and builds.
    nvrhi::IDevice                   *device    = _device;
    const std::atomic<std::uint64_t> *loadEpoch = &_loadEpoch;
    _jobs
        ->Run(Core::Pool::Worker,
              [device, data = std::move(data), channelPaths, epoch, loadEpoch]() mutable
              { return DecodeAndRecordMaterialChannels(device, std::move(data), channelPaths, epoch, *loadEpoch); })
        .Then(Core::Pool::Main, [this, path, epoch](MaterialLoadBundle bundle)
              { OnMaterialLoaded(path, epoch, std::move(bundle)); });
}

void AssetCache::OnMaterialLoaded(Core::AssetPath path, std::uint64_t epoch, MaterialLoadBundle bundle)
{
    --_activeLoads;
    PumpLoadQueue();

    if (epoch != _loadEpoch.load(std::memory_order_relaxed))
        return; // Stale (see OnMeshLoaded): return before erasing so a stale completion can't drop
                // a current-epoch job's loading marker and trigger a duplicate load. The bundle
                // (worker-created textures + list) is dropped here and freed.

    // Enqueue the publish (O(1)); the decode + upload recording already happened on
    // the worker, so this is cheap. The path stays in _materialLoading until
    // PublishMaterial submits + adopts, so it keeps rendering with the fallback.
    _pendingPublishes.push_back(PendingPublish{.isMaterial = true,
                                               .path       = path,
                                               .epoch      = epoch,
                                               .byteSize   = bundle.decodedBytes,
                                               .material   = std::move(bundle)});
}

void AssetCache::PublishMesh(PendingPublish publish)
{
    _meshLoading.erase(publish.path);
    MeshBuffer &buffer = _meshes[publish.path];
    if (publish.mesh.staging != nullptr)
    {
        // The worker already copied the geometry into a GPU staging buffer, so this
        // records two copyBuffers — O(1) in mesh size, no main-thread memcpy. Our
        // handle drops here; the upload list keeps the staging buffer alive until
        // its submit retires.
        buffer.UploadStaged(_arena, std::move(publish.mesh.data), publish.mesh.staging, publish.mesh.vertexCount,
                            publish.mesh.indexCount, BeginUpload());
    }
    else
    {
        // No staging (empty geometry, or the staging allocation failed): fall back to
        // the direct path, which memcpys here. The CPU data is still present — the
        // worker only releases it when staging succeeded.
        buffer.Upload(_arena, std::move(publish.mesh.data), BeginUpload());
    }
    buffer.SetId(_nextMeshId++);
}

void AssetCache::PublishMaterial(PendingPublish publish)
{
    _materialLoading.erase(publish.path);

    // The channel textures were created + recorded on the decode worker; hand its
    // closed list to the batch so FlushUploads submits it (the memcpy already ran).
    if (publish.material.uploadList)
        _uploadBatch.push_back(std::move(publish.material.uploadList));

    // Adopt each channel's worker-created texture into the cache (or reference an
    // already-resident one and drop the duplicate), then build the material and
    // write its row into the shared list. Every slot/row is brand new — no draw
    // references this material yet (loading entities use the fallback), so nothing
    // an in-flight frame reads is mutated.
    MaterialTextures textures;
    uint32_t        *slots[5] = {&textures.baseColor, &textures.normal, &textures.metallicRoughness,
                                 &textures.occlusion, &textures.emissive};
    for (std::size_t ch = 0; ch < kChannels.size(); ++ch)
    {
        RecordedChannel &rc = publish.material.channels[ch];
        if (rc.texture)
        {
            // Dedup by (path, space): a texture shared across materials resolves to
            // one resident copy. A duplicate's worker texture is simply not adopted —
            // its recorded upload still executes (harmless: it targets a texture we
            // release, kept alive by the list until the submit retires) and is freed.
            const TextureKey key{rc.path, kChannels[ch].space};
            if (auto it = _textures.find(key); it != _textures.end() && it->second.IsValid())
            {
                *slots[ch] = it->second.BindlessIndex();
            }
            else
            {
                Texture &texture = _textures[key];
                texture.Adopt(std::move(rc.texture));
                *slots[ch] = RegisterBindlessTexture(texture);
            }
            if (kChannels[ch].isNormal)
                textures.hasNormalTexture = true;
        }
        else
        {
            const Texture *fallback = ResolveTexture(*kChannels[ch].fallback, kChannels[ch].space);
            *slots[ch] = fallback != nullptr ? fallback->BindlessIndex() : 0u;
        }
    }

    Material &material = _materials[publish.path];
    material.Create(_device, MintMaterialId(), publish.material.data, textures);
    WriteMaterialToTable(material, BeginUpload());
}

nvrhi::ICommandList *AssetCache::BeginUpload()
{
    if (!_uploadOpen)
    {
        _uploadList->open();
        _uploadOpen = true;
    }
    return _uploadList;
}

void AssetCache::FlushUploads()
{
    if (!_uploadOpen && _uploadBatch.empty())
        return;
    if (_uploadOpen)
        _uploadList->close();

    // One submit for the whole batch (P0/P1): the worker-recorded material texture
    // lists plus the shared main-thread list (mesh arena writes, material rows). The
    // lists touch disjoint resources, so their relative order is irrelevant; draws in
    // the frame that follows this submit see all of it.
    std::vector<nvrhi::ICommandList *> lists;
    lists.reserve(_uploadBatch.size() + 1);
    for (const nvrhi::CommandListHandle &list : _uploadBatch)
        lists.push_back(list);
    if (_uploadOpen)
        lists.push_back(_uploadList);
    _device->executeCommandLists(lists.data(), lists.size());

    // Drop our handles: nvrhi's lifetime tracker keeps each list and its referenced
    // resources (the textures) alive until the GPU retires this submit.
    _uploadBatch.clear();
    _uploadOpen = false;
}

void AssetCache::PumpPublishes(double timeBudgetMs, std::size_t byteBudget)
{
    if (_pendingPublishes.empty())
        return;

    using Clock = std::chrono::steady_clock;
    const auto elapsedMsSince = [](Clock::time_point t)
    { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); };

    // Per-phase timing to localize the streaming CPU spike (which of the remaining
    // main-thread costs dominates): mesh arena writeBuffer memcpy vs material publish
    // (adopt + bindless descriptor writes) vs the batched submit. Only logged when a
    // pump is expensive (see below), so it stays silent in the normal case.
    const Clock::time_point start = Clock::now();
    std::size_t             bytes     = 0;
    std::size_t             meshCount = 0;
    std::size_t             matCount  = 0;
    double                  meshMs    = 0.0;
    double                  matMs     = 0.0;
    bool                    any       = false;
    while (!_pendingPublishes.empty())
    {
        // Budget stops the batch — but always publish at least one (a single asset
        // larger than the whole budget must still make progress, not wedge forever).
        if (any)
        {
            if (bytes >= byteBudget)
                break;
            if (elapsedMsSince(start) >= timeBudgetMs)
                break;
        }

        PendingPublish publish = std::move(_pendingPublishes.front());
        _pendingPublishes.pop_front();

        // A Clear() between the decode continuation and this pump supersedes it; the
        // loading marker was already dropped by Clear, so just skip it.
        if (publish.epoch != _loadEpoch.load(std::memory_order_relaxed))
            continue;

        const std::size_t       publishBytes = publish.byteSize;
        const Clock::time_point publishStart = Clock::now();
        if (publish.isMaterial)
        {
            PublishMaterial(std::move(publish));
            matMs += elapsedMsSince(publishStart);
            ++matCount;
        }
        else
        {
            PublishMesh(std::move(publish));
            meshMs += elapsedMsSince(publishStart);
            ++meshCount;
        }
        bytes += publishBytes;
        any = true;
    }

    // One submit for the whole batch (P0): draws in the frame that follows see it.
    double flushMs = 0.0;
    if (any)
    {
        const Clock::time_point flushStart = Clock::now();
        FlushUploads();
        flushMs = elapsedMsSince(flushStart);
    }

    // Diagnostic: a pump is a spike when its main-thread wall-clock blows past the
    // budget (the "always at least one" escape hatch fired with an expensive item,
    // or the pump was preempted mid-work). The phase split says which cost it was —
    // meshMs high => arena memcpy (mesh P1 next); matMs high => descriptor writes;
    // flushMs high => submit; all low but total high => the main thread was
    // preempted by the decode workers (points at P2, a low-priority IO pool).
    const double totalMs = elapsedMsSince(start);
    if (totalMs >= 2.0)
        Core::Log::Info("AssetCache pump {:.2f} ms: mesh {}x {:.2f} ms, mat {}x {:.2f} ms, flush {:.2f} ms; {} queued",
                        totalMs, meshCount, meshMs, matCount, matMs, flushMs, _pendingPublishes.size());
}

void AssetCache::Clear()
{
    // Drain the GPU before freeing anything. Clear() destroys resources a
    // still-in-flight frame may reference — the bindless descriptor table it has
    // bound, the textures and arena buffers it samples/draws. Freeing those
    // mid-use is the rapid-load crash ("VkDescriptorSet ... destroyed ... without
    // UPDATE_AFTER_BIND"): a Load lands while the previous frame's command buffer
    // is still executing. waitForIdle guarantees no command buffer is in flight,
    // so the frees below are safe; a stall on level load is fine.
    _device->waitForIdle();

    // Cancel every in-flight async load: bump the epoch so their publishes become
    // no-ops (they check it on the main thread), and forget the "loading" marks.
    // The worker jobs themselves may still be decoding; they finish, publish, and
    // are dropped. Do this before freeing so a publish landing during this Clear
    // (it can't — we're on the main thread, same as DrainMain) would already see
    // the new epoch.
    ++_loadEpoch;
    _meshLoading.clear();
    _materialLoading.clear();

    // Drop loads that were queued but never started (their epoch is now stale
    // anyway). Loads already running are left to finish and publish; their
    // continuations decrement _activeLoads and see the bumped epoch, so they drop
    // their result — no need to touch _activeLoads here (resetting it while a job
    // is mid-flight would underflow when that job's continuation decrements).
    _pendingLoads.clear();

    // Drop decoded-but-not-yet-uploaded publishes: their epoch is now stale, so
    // PumpPublishes would skip them anyway, but freeing their held CPU data now
    // (mesh vertices, decoded images) releases the memory immediately. Any that were
    // already recorded into the upload list this frame can't exist here — pumps
    // always FlushUploads before returning, so _uploadOpen is false between them.
    _pendingPublishes.clear();
    _uploadBatch.clear(); // empty between pumps (FlushUploads drains it); defensive.

    // Recreate the shared upload list to return its peak staging memory: an upload
    // manager never shrinks its chunk pool, so a level's worth of texture bursts
    // leaves it holding the high-water mark until dropped. Safe here — waitForIdle
    // above guarantees its last submit has retired, and _uploadOpen is false.
    nvrhi::CommandListParameters uploadParams;
    uploadParams.uploadChunkSize = kUploadChunkSize;
    _uploadList                  = _device->createCommandList(uploadParams);
    _uploadOpen                  = false;

    _meshes.clear();
    _arena.Reset(); // wholesale free — the MeshBuffers that held ranges are gone.

    // Hand out slots from 0 again; the default textures re-register via
    // BuildFallbackMaterial below and overwrite the low slots in place. Stale
    // descriptors above them are simply never referenced again (no material
    // points at them), so there is nothing to release explicitly — and nothing
    // to resize: the table's capacity is fixed at creation.
    _nextBindlessSlot  = 0;
    _bindlessTableFull = false;

    _textures.clear();
    _materials.clear();
    _missingMeshWarned.clear();
    _missingMaterialWarned.clear();

    // Cancel any in-flight thumbnail decodes and drop resident thumbnails too, so a
    // full reset leaves nothing behind. The editor's thumbnail cache is a separate
    // AssetCache instance and evicts through ClearThumbnails (which releases the
    // ImGui bindings); on the scene cache these are simply empty. See ResolveThumbnail.
    ++_thumbnailEpoch;
    _thumbnailLoading.clear();
    _thumbnails.clear();

    // Hand out ids from 1 again: an id is a row in the table, so ids stay dense per
    // asset set. Row 0 (the fallback) is repopulated by BuildFallbackMaterial below,
    // overwriting its old row in place. The buffer handle is unchanged (fixed
    // capacity), so MeshPass's bound material table stays valid; stale rows from the
    // previous asset set are simply never referenced (no entity points at them).
    _nextMaterialId = 1;

    // The fallback's texture pointers dangled when _textures cleared; rebuild it
    // (and its prim:// defaults) so FallbackMaterial() stays valid immediately.
    BuildFallbackMaterial();
}

} /* namespace Assisi::Render */
