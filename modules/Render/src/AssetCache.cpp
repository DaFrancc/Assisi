/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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
constexpr uint32_t kInitialBindlessCapacity = 256u;

// Material-table capacity (rows). Fixed so the buffer handle is stable across
// Clear() and the MeshPass binds it once; generous enough for any real scene
// (the opaque sort key allows ~1M, but a level with thousands of *distinct*
// materials is unheard of). Rows past this are dropped with a one-time warning
// (see Buffer::Upload) rather than resized into — keeping the handle stable
// matters more than the ceiling.
constexpr uint32_t kMaxMaterials = 4096u;

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

// One channel's decoded image, produced on a worker. `image` is empty for an
// unset channel or one whose decode failed (→ the prim:// fallback at publish);
// `path` keys the texture cache so a texture shared across materials uploads once.
struct DecodedChannel
{
    std::optional<DecodedImage> image;
    Core::AssetPath             path;
};

// The full result of a material load job: the parsed data plus every channel's
// decoded pixels, ready for the main thread to upload and build.
struct MaterialLoadBundle
{
    Geometry::MaterialData        data;
    std::array<DecodedChannel, 5> channels;
};
} // namespace

void AssetCache::Initialize(nvrhi::IDevice *device, Core::JobSystem *jobs, ColorSpace textureColorSpace)
{
    _device = device;
    _jobs = jobs;
    _textureColorSpace = textureColorSpace;

    // The shared geometry arena's single vertex format is Geometry::Vertex.
    _arena.Initialize(_device, sizeof(Geometry::Vertex));

    // Bindless material-texture table (GPU-driven stage D): a Pixel-visible
    // Texture_SRV array in its own register space. Every resolved texture takes a
    // slot; materials reference channels by index. Sized on demand as textures
    // resolve (see RegisterBindlessTexture); starts at kInitialBindlessCapacity.
    nvrhi::BindlessLayoutDesc bindlessDesc;
    bindlessDesc.visibility = nvrhi::ShaderType::Pixel;
    bindlessDesc.firstSlot = 0;
    bindlessDesc.maxCapacity = 16384;
    bindlessDesc.addRegisterSpace(nvrhi::BindingLayoutItem::Texture_SRV(0));
    _bindlessLayout = _device->createBindlessLayout(bindlessDesc);
    _bindlessTable = _device->createDescriptorTable(_bindlessLayout);
    _bindlessCapacity = kInitialBindlessCapacity;
    _nextBindlessSlot = 0;
    _device->resizeDescriptorTable(_bindlessTable, _bindlessCapacity, false);

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

    _meshLoading.insert(path);
    const uint64_t   epoch = _loadEpoch;
    Core::AssetPath  loadPath = path;
    PathToIdFn       pathToId = _pathToId; // copy: the worker must not touch cache state

    _jobs->Run(Core::Pool::Worker,
               [loadPath, pathToId] { return Geometry::ImportMesh(loadPath.View(), pathToId); })
        .Then(Core::Pool::Main,
              [this, loadPath, epoch](std::expected<Geometry::MeshData, Geometry::MeshImportError> imported) {
                  _meshLoading.erase(loadPath);
                  if (epoch != _loadEpoch)
                      return; // the level that asked for this mesh has since unloaded — drop it
                  if (!imported)
                  {
                      _missingMeshWarned.insert(loadPath);
                      Core::Log::Warn("AssetCache: no mesh for '{}' ({}), falling back to prim://cube.",
                                      loadPath.View(), Geometry::ToString(imported.error()));
                      return; // a later resolve returns the cube
                  }
                  MeshBuffer &buffer = _meshes[loadPath];
                  buffer.Upload(_arena, std::move(*imported));
                  buffer.SetId(_nextMeshId++);
              });

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

uint32_t AssetCache::RegisterBindlessTexture(Texture &texture)
{
    if (texture.BindlessIndex() != Texture::kInvalidBindlessIndex)
        return texture.BindlessIndex();

    const uint32_t slot = _nextBindlessSlot++;
    if (slot >= _bindlessCapacity)
    {
        _bindlessCapacity = std::max(_bindlessCapacity * 2u, slot + 1u);
        _device->resizeDescriptorTable(_bindlessTable, _bindlessCapacity, true);
    }
    _device->writeDescriptorTable(_bindlessTable,
                                  nvrhi::BindingSetItem::Texture_SRV(slot, texture.NativeTexture()));
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

void AssetCache::WriteMaterialToTable(const Material &material)
{
    const uint32_t id = material.Id();
    if (id >= kMaxMaterials)
    {
        Core::Log::Warn("AssetCache: material id {} exceeds the material table capacity ({}); it will render "
                        "with a stale/garbage row.",
                        id, kMaxMaterials);
        return;
    }

    // Write ONLY this material's row, at its own offset — never re-upload the whole
    // table. Materials build incrementally across frames during an async load; a
    // full-prefix re-upload would rewrite every already-resident material's row on
    // a throwaway command list that races the in-flight frame's reads of the table,
    // which flickers the whole scene. This row is brand new (no draw references
    // this material yet — loading entities use the fallback), so writing it touches
    // no bytes an in-flight frame reads.
    const MaterialConstants row = material.Constants();
    nvrhi::CommandListHandle commandList = _device->createCommandList();
    commandList->open();
    commandList->writeBuffer(_materialTable.NativeBuffer(), &row, sizeof(row),
                             static_cast<uint64_t>(id) * sizeof(MaterialConstants));
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

    _materialLoading.insert(path);
    const uint64_t         epoch = _loadEpoch;
    Core::AssetPath        loadPath = path;
    Geometry::MaterialData materialData = std::move(*data);
    Core::JobSystem       *jobs = _jobs; // captured by value; the worker must not touch cache state

    _jobs
        ->Run(Core::Pool::Worker,
              [jobs, materialData, channelPaths]() -> MaterialLoadBundle {
                  // Worker: decode every channel image, in parallel across the
                  // channels — a texture-heavy material (e.g. a full glTF PBR set)
                  // then costs max(channel) rather than sum(channel). Pure CPU over
                  // copied inputs; each channel writes a disjoint slot, so no
                  // synchronisation is needed. Nested ParallelFor is deadlock-free
                  // (the waiting worker runs the sub-tasks itself — help-waiting).
                  MaterialLoadBundle bundle;
                  bundle.data = materialData;
                  jobs->ParallelFor(static_cast<uint32_t>(kChannels.size()), 1,
                                    [&bundle, &channelPaths](uint32_t begin, uint32_t end) {
                                        for (uint32_t ch = begin; ch < end; ++ch)
                                        {
                                            if (channelPaths[ch].Empty())
                                                continue; // unset channel → fallback prim at publish
                                            bundle.channels[ch].path = channelPaths[ch];
                                            std::expected<DecodedImage, Core::AssetError> img =
                                                Texture::DecodeImage(channelPaths[ch].View(), kChannels[ch].space);
                                            if (img)
                                                bundle.channels[ch].image = std::move(*img);
                                            // a decode failure leaves the image empty → fallback prim at publish
                                        }
                                    });
                  return bundle;
              })
        .Then(Core::Pool::Main, [this, loadPath, epoch](MaterialLoadBundle bundle) {
            _materialLoading.erase(loadPath);
            if (epoch != _loadEpoch)
                return; // the level that asked for this material has since unloaded — drop it

            // Publish (main thread): resolve each channel to a bindless slot, then
            // build the material and write its table row. Every slot/row written
            // here is brand new — no draw references this material yet (loading
            // entities use the fallback), so nothing an in-flight frame reads is
            // mutated.
            MaterialTextures textures;
            uint32_t        *slots[5] = {&textures.baseColor, &textures.normal, &textures.metallicRoughness,
                                         &textures.occlusion, &textures.emissive};
            for (std::size_t ch = 0; ch < kChannels.size(); ++ch)
            {
                DecodedChannel &dc = bundle.channels[ch];
                if (dc.image.has_value())
                {
                    // Dedup by (path, space): a texture shared across materials is
                    // uploaded once, even if it was decoded on more than one worker.
                    const TextureKey key{dc.path, kChannels[ch].space};
                    if (auto it = _textures.find(key); it != _textures.end() && it->second.IsValid())
                    {
                        *slots[ch] = it->second.BindlessIndex();
                    }
                    else
                    {
                        Texture &texture = _textures[key];
                        texture.UploadDecoded(_device, *dc.image, std::string(dc.path.View()).c_str());
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

            Material &material = _materials[loadPath];
            material.Create(_device, _nextMaterialId++, bundle.data, textures);
            WriteMaterialToTable(material);
        });

    return &_fallbackMaterial; // fallback while loading
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

    _meshes.clear();
    _arena.Reset(); // wholesale free — the MeshBuffers that held ranges are gone.

    // Drop every bindless table entry (releasing the textures they referenced),
    // then re-reserve. The handle is unchanged, so MeshPass's bound table stays
    // valid; the default textures re-register via BuildFallbackMaterial below.
    _device->resizeDescriptorTable(_bindlessTable, 0, false);
    _bindlessCapacity = kInitialBindlessCapacity;
    _nextBindlessSlot = 0;
    _device->resizeDescriptorTable(_bindlessTable, _bindlessCapacity, false);

    _textures.clear();
    _materials.clear();
    _missingMeshWarned.clear();
    _missingMaterialWarned.clear();

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
