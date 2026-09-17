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

#include <Assisi/Chiara/Profile.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Geometry/DefaultMeshes.hpp>
#include <Assisi/Geometry/MaterialChannels.hpp>
#include <Assisi/Render/AssetCache.hpp>

namespace Assisi::Render
{

namespace
{
/// @brief The fallback mesh for nil and unloadable mesh references.
constexpr Core::AssetId kCubePrimitive = Core::BuiltinAssetId::Cube;

// The primitive-shape ladder, by the reserved ids Core/AssetId.hpp assigns them;
// tessellations come from Geometry::PrimitiveTessellation rather than the
// factory defaults, which are tuned for editor collider silhouettes (see the
// note there). Registered eagerly but built lazily — ResolvePrimitive only
// invokes a factory on first use, so a level that names none of these pays for
// none of them.
struct PrimitiveDesc
{
    Core::AssetId id;
    Geometry::MeshData (*factory)();
};

const std::array<PrimitiveDesc, 8> kShapePrimitives = {{
    {Core::BuiltinAssetId::SphereLow,
     [] {
         return Geometry::CreateUnitSphereMesh(Geometry::PrimitiveTessellation::kSphereLowSlices,
                                               Geometry::PrimitiveTessellation::kSphereLowStacks);
     }},
    {Core::BuiltinAssetId::Sphere,
     [] {
         return Geometry::CreateUnitSphereMesh(Geometry::PrimitiveTessellation::kSphereSlices,
                                               Geometry::PrimitiveTessellation::kSphereStacks);
     }},
    {Core::BuiltinAssetId::SphereHigh,
     [] {
         return Geometry::CreateUnitSphereMesh(Geometry::PrimitiveTessellation::kSphereHighSlices,
                                               Geometry::PrimitiveTessellation::kSphereHighStacks);
     }},
    {Core::BuiltinAssetId::IcosphereLow,
     [] { return Geometry::CreateIcosphereMesh(Geometry::PrimitiveTessellation::kIcosphereLowSubdivisions); }},
    {Core::BuiltinAssetId::Icosphere,
     [] { return Geometry::CreateIcosphereMesh(Geometry::PrimitiveTessellation::kIcosphereSubdivisions); }},
    {Core::BuiltinAssetId::IcosphereHigh,
     [] { return Geometry::CreateIcosphereMesh(Geometry::PrimitiveTessellation::kIcosphereHighSubdivisions); }},
    {Core::BuiltinAssetId::Cylinder,
     [] { return Geometry::CreateUnitCylinderMesh(Geometry::PrimitiveTessellation::kCylinderSlices); }},
    {Core::BuiltinAssetId::CylinderHigh,
     [] { return Geometry::CreateUnitCylinderMesh(Geometry::PrimitiveTessellation::kCylinderHighSlices); }},
}};

// Solid-colour texture primitives — a material's per-channel defaults, owned by
// the cache like any other engine-generated asset so a device rebuild regenerates
// them uniformly (see AssetCache.hpp). Each carries its own fixed colour space;
// the space a caller passes to ResolveTexture is ignored for these.
constexpr Core::AssetId kWhiteTexture       = Core::BuiltinAssetId::White;       // baseColor / emissive.
constexpr Core::AssetId kWhiteLinearTexture = Core::BuiltinAssetId::WhiteLinear; // metallic-roughness / occlusion.
constexpr Core::AssetId kFlatNormalTexture  = Core::BuiltinAssetId::FlatNormal;  // unperturbed tangent-space normal.

// Bindless texture-table capacity (slots). Fixed at table creation: nvrhi's
// Vulkan backend implements resizeDescriptorTable as an assert-only no-op, so
// the descriptor table's real capacity is whatever the layout was built with and
// can never change. Slots past this saturate onto slot 0 with a one-time warning.
constexpr uint32_t kBindlessCapacity = 16384u;

// The channel enum and the format each channel wants both live in
// Geometry::MaterialChannels, because the cooker compresses the same textures
// offline and the two must reach the same answer. What stays here is what only
// a renderer has: the `prim://` texture that stands in for an empty or failed
// channel, and the flag marking the one whose presence drives the shader's
// normal-mapping bit.
using Geometry::MaterialChannel;

struct ChannelDesc
{
    Core::AssetId fallback;
    bool isNormal;
};

const std::array<ChannelDesc, static_cast<std::size_t>(MaterialChannel::Count)> kChannels = {{
    {kWhiteTexture, false},       // baseColor
    {kFlatNormalTexture, true},   // normal
    {kWhiteLinearTexture, false}, // metallic-roughness
    {kWhiteLinearTexture, false}, // occlusion
    {kWhiteTexture, false},       // emissive
}};

// Minimum staging chunk for the shared upload command list. A texture burst (a
// 2K RGBA8 + mips is ~22 MB) would otherwise fragment into dozens of the default
// 64 KB chunks, each its own vkAllocateMemory; one big chunk absorbs a whole
// material's channels. Peak staging is reclaimed by recreating the list on Clear.
constexpr uint64_t kUploadChunkSize = 16ull << 20; // 16 MB

Core::AssetId ChannelId(const Geometry::MaterialData &data, std::size_t channel)
{
    return Geometry::ChannelTexture(data, static_cast<MaterialChannel>(channel));
}

// The shared table, reached by the dense index kChannels is walked with.
Geometry::MaterialChannelFormat ChannelFormat(std::size_t channel)
{
    return Geometry::FormatFor(static_cast<MaterialChannel>(channel));
}

/// The installed source, or an error naming the id when none is installed — a
/// cache used before its executable installed one loads nothing rather than crash.
const AssetSource *SourceOrLog(const Core::AssetId &id)
{
    const AssetSource *source = GetAssetSource();
    if (source == nullptr)
    {
        Core::Log::Error("AssetCache: no asset source is installed, so {} cannot load.", id.ToString());
    }
    return source;
}
} // namespace

void AssetCache::Initialize(nvrhi::IDevice *device, Core::JobSystem *jobs, Image::ColorSpace textureColorSpace)
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
    // the table can never actually grow (see RegisterBindlessTexture). It is
    // update-after-bind (nvrhi-bindless-update-after-bind.patch): otherwise the
    // whole capacity counts against the ordinary per-stage sampled-image limit,
    // which is 200 on Intel, and no mesh pipeline builds there. That makes it
    // sampled images only; another descriptor type here needs its own
    // descriptorBinding*UpdateAfterBind feature enabled in VulkanContext.
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
    _materialTable.Create(_device, sizeof(MaterialConstants), kMaxMaterials, /*allowUnorderedAccess=*/ false,
                          "AssetCache::MaterialTable");

    _primitiveFactories.emplace(kCubePrimitive, &Geometry::CreateUnitCubeMesh);
    for (const PrimitiveDesc &primitive : kShapePrimitives)
    {
        _primitiveFactories.emplace(primitive.id, primitive.factory);
    }

    // White sRGB stands in for empty baseColor/emissive channels; white *linear*
    // for metallic-roughness/occlusion (sampling 1.0 leaves the per-material
    // factor untouched — glTF "no texture" semantics); flat normal (128,128,255)
    // linear decodes to +Z, i.e. no perturbation.
    _texturePrimitives.emplace(kWhiteTexture, SolidColor{255, 255, 255, 255, Image::ColorSpace::Srgb});
    _texturePrimitives.emplace(kWhiteLinearTexture, SolidColor{255, 255, 255, 255, Image::ColorSpace::Linear});
    _texturePrimitives.emplace(kFlatNormalTexture, SolidColor{128, 128, 255, 255, Image::ColorSpace::Linear});

    BuildFallbackMaterial();
}

const MeshBuffer *AssetCache::ResolvePrimitive(const Core::AssetId &id)
{
    if (std::unordered_map<Core::AssetId, MeshBuffer>::iterator it = _meshes.find(id); it != _meshes.end())
        return &it->second;

    std::unordered_map<Core::AssetId, std::function<Geometry::MeshData()>>::iterator factory =
        _primitiveFactories.find(id);
    if (factory == _primitiveFactories.end())
        return nullptr;

    MeshBuffer &buffer = _meshes[id];
    buffer.Upload(_arena, factory->second());
    buffer.SetId(_nextMeshId++);
    return &buffer;
}

std::string AssetCache::Describe(const Core::AssetId &id) const
{
    const AssetSource *source = GetAssetSource();
    return source != nullptr ? source->Describe(id) : id.ToString();
}

Core::AssetId AssetCache::SlotMaterial(const Core::AssetId &meshId, std::uint32_t slot) const
{
    const auto found = _slotMaterials.find(meshId);
    if (found == _slotMaterials.end() || slot >= found->second.size())
    {
        return {};
    }
    return found->second[slot];
}

void AssetCache::SetSlotMaterials(const Core::AssetId &meshId, std::vector<Core::AssetId> slotMaterials)
{
    if (const auto found = _slotMaterials.find(meshId); found != _slotMaterials.end())
    {
        found->second = std::move(slotMaterials);
    }
}

const MeshBuffer *AssetCache::ResolveMesh(const Core::AssetId &id)
{
    // ResolvePrimitive also serves the mesh cache: any mesh already uploaded
    // (primitive or loaded) is returned here on subsequent frames. Primitives are
    // generated in-process and cheap, so they stay synchronous.
    if (const MeshBuffer *mesh = ResolvePrimitive(id))
        return mesh;

    // A nil id is the "unset" default; a reserved id that is not a primitive names
    // nothing; a mesh that already failed to load falls back to the cube and isn't
    // retried.
    if (id.IsReserved() || _missingMeshWarned.contains(id))
        return ResolvePrimitive(kCubePrimitive);

    // If a load is already in flight, report "still loading" (null) so the caller
    // shows a placeholder; the re-resolve loop will pick up the buffer once it's
    // uploaded. Otherwise kick the load: the source runs on a worker, then the
    // arena upload and cache insert publish back on the main thread.
    if (_meshLoading.contains(id))
        return nullptr;

    // Queue the load as data; PumpLoadQueue starts it only when fewer than
    // _maxConcurrentLoads are running, so a whole level's worth of loads don't all
    // decode at once and starve the main thread. The id enters _meshLoading now
    // (queued counts as pending) so the re-resolve loop won't re-request it.
    _meshLoading.insert(id);
    // PendingLoad is a poor-man's variant: the material-only members are spelled
    // out empty so "unset" reads as deliberate rather than forgotten.
    _pendingLoads.push_back(PendingLoad{.isMaterial   = false,
                                        .id           = id,
                                        .epoch        = _loadEpoch.load(std::memory_order_relaxed),
                                        .materialData = {},
                                        .channelIds   = {}});
    PumpLoadQueue();

    return nullptr; // loading — placeholder for now
}

Image::PixelFormat AssetCache::EffectiveFormat(Image::PixelFormat wanted) const
{
    if (!_textureCompressionSupported)
    {
        return Image::PixelFormat::Rgba8;
    }
    return wanted;
}

std::expected<void, AssetLoadError> AssetCache::LoadTexture(Texture &texture, const Core::AssetId &id,
                                                            Image::ColorSpace colorSpace, Image::PixelFormat format)
{
    const AssetSource *source = SourceOrLog(id);
    if (source == nullptr)
    {
        return std::unexpected(AssetLoadError::UnknownAsset);
    }
    std::expected<Image::DecodedImage, AssetLoadError> loaded = source->LoadTexture(id, colorSpace, format);
    if (!loaded)
    {
        return std::unexpected(loaded.error());
    }

    texture.UploadDecoded(_device, *loaded, Describe(id).c_str());
    if (!texture.IsValid())
    {
        return std::unexpected(AssetLoadError::Undecodable);
    }
    return {};
}

const Texture *AssetCache::ResolveTexture(const Core::AssetId &id, Image::ColorSpace colorSpace,
                                          Image::PixelFormat format)
{
    if (id.IsNil())
        return nullptr;

    // A solid-colour texture primitive: its own fixed colour space wins over the
    // caller's, so the same primitive is only ever resident once. Always
    // uncompressed — one texel does not fill a block.
    if (std::unordered_map<Core::AssetId, SolidColor>::iterator prim = _texturePrimitives.find(id);
        prim != _texturePrimitives.end())
    {
        const SolidColor &color = prim->second;
        TextureKey key{id, color.space, Image::PixelFormat::Rgba8};
        if (std::unordered_map<TextureKey, Texture, TextureKeyHash>::iterator it = _textures.find(key);
            it != _textures.end())
            return it->second.IsValid() ? &it->second : nullptr;

        Texture &texture = _textures[key];
        texture.UploadSolidColor(_device, color.r, color.g, color.b, color.a, color.space, id.ToString().c_str());
        RegisterBindlessTexture(texture);
        return &texture;
    }

    TextureKey key{id, colorSpace, format};
    if (std::unordered_map<TextureKey, Texture, TextureKeyHash>::iterator it = _textures.find(key);
        it != _textures.end())
        return it->second.IsValid() ? &it->second : nullptr;

    Texture &texture = _textures[key];
    if (std::expected<void, AssetLoadError> loaded = LoadTexture(texture, id, colorSpace, format); !loaded)
    {
        Core::Log::Warn("AssetCache: failed to load texture '{}' ({}) - drawing the error pattern.", Describe(id),
                        ToString(loaded.error()));
        ++_failedTextures;
        // The checkerboard rather than the channel's neutral default: a white
        // stand-in is what a great many correct materials look like, so the
        // failure would reach a build with nobody having seen it. The entry stays
        // resident either way, so a broken asset is not retried every frame.
        texture.UploadErrorPattern(_device, Describe(id).c_str());
        if (!texture.IsValid())
        {
            return nullptr;
        }
        RegisterBindlessTexture(texture);
        return &texture;
    }
    RegisterBindlessTexture(texture);
    return &texture;
}

void AssetCache::ReportTextureFailures()
{
    const std::uint32_t total = _failedTextures + Texture::UploadFailureCount();
    if (total == _reportedTextureFailures)
    {
        return;
    }
    _reportedTextureFailures = total;
    // One line once the queue is empty, rather than a warning per texture buried
    // in a load that logs hundreds of them.
    Core::Log::Error("AssetCache: {} texture(s) failed and are drawn as the error pattern.", total);
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

uint32_t AssetCache::ResolveChannel(const Core::AssetId &channelId, Geometry::MaterialChannel channel,
                                    bool *outPresent)
{
    const ChannelDesc &desc                       = kChannels[static_cast<std::size_t>(channel)];
    const Geometry::MaterialChannelFormat wanted   = Geometry::FormatFor(channel);

    if (!channelId.IsNil())
    {
        if (const Texture *texture = ResolveTexture(channelId, wanted.space, EffectiveFormat(wanted.format)))
        {
            if (outPresent != nullptr)
                *outPresent = true;
            return texture->BindlessIndex();
        }
    }

    if (outPresent != nullptr)
        *outPresent = false;
    // The primitive dictates its own colour space and is never compressed; what is
    // passed here is a harmless hint.
    const Texture *fallback = ResolveTexture(desc.fallback, wanted.space, Image::PixelFormat::Rgba8);
    return fallback != nullptr ? fallback->BindlessIndex() : 0u;
}

void AssetCache::BuildMaterial(Material &material, const Geometry::MaterialData &data, uint32_t id)
{
    MaterialTextures textures;
    textures.baseColor         = ResolveChannel(data.BaseColorTexture, MaterialChannel::BaseColor);
    textures.normal =
        ResolveChannel(data.NormalTexture, MaterialChannel::Normal, &textures.hasNormalTexture);
    textures.metallicRoughness = ResolveChannel(data.MetallicRoughnessTexture, MaterialChannel::MetallicRoughness);
    textures.occlusion         = ResolveChannel(data.OcclusionTexture, MaterialChannel::Occlusion);
    textures.emissive          = ResolveChannel(data.EmissiveTexture, MaterialChannel::Emissive);

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
    const uint64_t offset = static_cast<uint64_t>(id) * sizeof(MaterialConstants);
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

bool AssetCache::UpdateMaterialFactors(const Core::AssetId &id, const Geometry::MaterialData &data)
{
    const std::unordered_map<Core::AssetId, Material>::iterator it = _materials.find(id);
    if (it == _materials.end())
    {
        return false;
    }

    Material &material = it->second;
    MarkMaterialAuthored(it->first);
    // Copied, not aliased: Create writes through to _textures, and passing its
    // own member back in by reference would be a self-assignment mid-rebuild.
    const MaterialTextures textures = material.Textures();
    material.Create(_device, material.Id(), data, textures);
    WriteMaterialToTable(material);
    return true;
}

bool AssetCache::ReloadMaterial(const Core::AssetId &id, const Geometry::MaterialData &data)
{
    if (id.IsNil())
    {
        return false;
    }

    // Built in place under a new id: the same map entry, so every pointer already
    // handed out for this material stays valid and reads the new id from it. A
    // material with no entry yet gets one here rather than waiting for a load.
    MarkMaterialAuthored(id);
    BuildMaterial(_materials[id], data, MintMaterialId());
    return true;
}

void AssetCache::MarkMaterialAuthored(const Core::AssetId &id)
{
    // The editor's copy now outranks the stored one. A load kicked before this may
    // still be in flight, and its publish would overwrite the live material with
    // what is on disk — which is exactly the state the author has not saved yet.
    _authoredMaterials.insert(id);
    _missingMaterialWarned.erase(id);
}

const Material *AssetCache::ResolveMaterial(const Core::AssetId &id)
{
    if (id.IsNil())
        return &_fallbackMaterial;

    if (std::unordered_map<Core::AssetId, Material>::iterator it = _materials.find(id); it != _materials.end())
        return &it->second;

    // A broken/missing material, or one whose load is already in flight, resolves to
    // the fallback for now (a model whose material isn't ready renders white).
    if (_missingMaterialWarned.contains(id) || _materialLoading.contains(id))
        return &_fallbackMaterial;

    // The material itself on the main thread — it's small. The expensive part
    // (loading the channel textures) then runs on a worker; the upload + build
    // publishes back on the main thread. Until then, the fallback stands in.
    const AssetSource *source = SourceOrLog(id);
    std::expected<Geometry::MaterialData, AssetLoadError> data =
        source != nullptr ? source->LoadMaterial(id) : std::unexpected(AssetLoadError::UnknownAsset);
    if (!data)
    {
        _missingMaterialWarned.insert(id);
        Core::Log::Warn("AssetCache: material '{}' did not load ({}) - using the fallback material.", Describe(id),
                        ToString(data.error()));
        return &_fallbackMaterial;
    }

    // Empty channels stay nil and resolve to a solid-colour default at publish time.
    std::array<Core::AssetId, 5> channelIds;
    for (std::size_t ch = 0; ch < kChannels.size(); ++ch)
        channelIds[ch] = ChannelId(*data, ch);

    // Queue the load as data; PumpLoadQueue starts it under the concurrency cap.
    // The id enters _materialLoading now (queued counts as pending).
    _materialLoading.insert(id);
    _pendingLoads.push_back(PendingLoad{.isMaterial   = true,
                                        .id           = id,
                                        .epoch        = _loadEpoch.load(std::memory_order_relaxed),
                                        .materialData = std::move(*data),
                                        .channelIds   = channelIds});
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
            StartMaterialLoad(load.id, std::move(load.materialData), load.channelIds, load.epoch);
        else
            StartMeshLoad(load.id, load.epoch);
    }
}

std::expected<AssetCache::MeshLoadBundle, AssetLoadError>
AssetCache::LoadAndStageMesh(AssetCache &cache, Core::AssetId id, std::uint64_t epoch,
                             const std::atomic<std::uint64_t> &loadEpoch)
{
    nvrhi::IDevice *device = cache._device;
    if (loadEpoch.load(std::memory_order_relaxed) != epoch)
    {
        return std::unexpected(AssetLoadError::UnknownAsset);
    }

    const AssetSource *source = SourceOrLog(id);
    if (source == nullptr)
    {
        return std::unexpected(AssetLoadError::UnknownAsset);
    }
    std::expected<Geometry::CookedMesh, AssetLoadError> loaded = source->LoadMesh(id);
    if (!loaded)
    {
        return std::unexpected(loaded.error());
    }
    Geometry::MeshData *imported = &loaded->mesh;

    // Normalize the degenerate no-submesh case here, on the worker, while the CPU
    // geometry is still around: the staged publish path has no vertices left to
    // derive a table from (and this is main-thread work the direct path would do).
    Geometry::EnsureSubMeshTables(*imported);

    MeshLoadBundle bundle;
    bundle.slotMaterials = std::move(loaded->slotMaterials);
    bundle.vertexCount = static_cast<uint32_t>(imported->Vertices.size());
    bundle.indexCount  = static_cast<uint32_t>(imported->Indices.size());

    // Copy the geometry into a GPU staging buffer HERE, on the worker — this is the
    // bulk memcpy that would otherwise land on the main thread inside writeBuffer
    // (O(mesh bytes) mid-frame). The main-thread publish then only records a copy.
    // The buffer is pooled: creating and destroying one per mesh would just move
    // the cost into the next frame's runGarbageCollection, on the main thread.
    const std::uint64_t stagingBytes = MeshBuffer::MeshStagingBytes(*imported);
    if (stagingBytes > 0)
    {
        nvrhi::BufferHandle staging = cache.AcquireStagingBuffer(stagingBytes);
        if (MeshBuffer::StageMeshGeometry(device, staging, *imported))
        {
            bundle.staging = std::move(staging);
        }
    }
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

void AssetCache::StartMeshLoad(Core::AssetId id, std::uint64_t epoch)
{
    // Load + stage on a worker (touching no cache state), then publish on the main
    // thread. The worker bails early if a Clear() has superseded this epoch — saving
    // the load for a mesh nobody awaits anymore.
    const std::atomic<std::uint64_t> *loadEpoch = &_loadEpoch;
    _jobs
    ->Run(Core::Pool::Worker, [this, id, epoch, loadEpoch]()
          { return LoadAndStageMesh(*this, id, epoch, *loadEpoch); })
    .Then(Core::Pool::Main, [this, id, epoch](std::expected<MeshLoadBundle, AssetLoadError> r)
          { OnMeshLoaded(id, epoch, std::move(r)); });
}

void AssetCache::OnMeshLoaded(Core::AssetId id, std::uint64_t epoch,
                              std::expected<MeshLoadBundle, AssetLoadError> imported)
{
    // A started load has finished (success, failure, or stale): free its slot and
    // let the next queued load begin. The decode fan-out continues even though this
    // result's GPU upload is deferred to PumpPublishes.
    --_activeLoads;
    PumpLoadQueue();

    if (epoch != _loadEpoch.load(std::memory_order_relaxed))
        return; // Stale: the level that asked for this mesh has since unloaded. Return WITHOUT
                // erasing the loading marker — Clear() already dropped ours, and a current-epoch
                // job for the same mesh may own it now.
    if (!imported)
    {
        // Failure is terminal and has no GPU work — settle it here rather than
        // queuing a publish. Drop the loading marker so a later resolve falls back.
        _meshLoading.erase(id);
        _missingMeshWarned.insert(id);
        Core::Log::Warn("AssetCache: no mesh for '{}' ({}), falling back to prim://cube.", Describe(id),
                        ToString(imported.error()));
        return; // a later resolve returns the cube
    }
    // Enqueue the publish (O(1)); the id stays in _meshLoading until PublishMesh
    // makes it resident, so HasPendingLoads / re-resolve keep treating it as pending.
    const std::size_t bytes = static_cast<std::size_t>(imported->vertexCount) * sizeof(Geometry::Vertex) +
                              static_cast<std::size_t>(imported->indexCount) * sizeof(uint32_t);
    _pendingPublishes.push_back(PendingPublish{.isMaterial = false,
                                               .id         = id,
                                               .epoch      = epoch,
                                               .byteSize   = bytes,
                                               .mesh       = std::move(*imported),
                                               .material   = {}}); // material-only
}

AssetCache::MaterialLoadBundle AssetCache::DecodeAndRecordMaterialChannels(
    AssetCache &cache, Geometry::MaterialData data, std::array<Core::AssetId, 5> channelIds, std::uint64_t epoch,
    const std::atomic<std::uint64_t> &loadEpoch)
{
    nvrhi::IDevice *device = cache._device;
    if (loadEpoch.load(std::memory_order_relaxed) != epoch)
        return {}; // superseded before the load ran; the publish drops it anyway

    // Load each channel sequentially — one worker per material (a nested
    // parallel-for would spread a single material across every pool core, exactly
    // the saturation the concurrency cap exists to prevent). For each loaded
    // channel, create its GPU texture and record its upload into ONE command list
    // on this worker: that is where the writeTexture staging memcpy happens, and
    // moving it here is the whole point of P1 (the main-thread CPU spike). An empty
    // or failed channel is left without a texture → the solid-colour default at publish.
    MaterialLoadBundle bundle;
    bundle.data = std::move(data);

    const AssetSource *source = GetAssetSource();
    nvrhi::CommandListHandle list; // created lazily on the first channel that loads
    for (std::size_t ch = 0; ch < kChannels.size(); ++ch)
    {
        if (channelIds[ch].IsNil())
            continue;

        // The id travels with the failure, not only with the success: it is what
        // names the asset in the warning and what keys the error texture, and
        // without it every failed channel collapses onto one empty key.
        bundle.channels[ch].id = channelIds[ch];

        const Image::PixelFormat format = cache.EffectiveFormat(ChannelFormat(ch).format);
        std::expected<Image::DecodedImage, AssetLoadError> img =
            source != nullptr ? source->LoadTexture(channelIds[ch], ChannelFormat(ch).space, format)
                              : std::unexpected(AssetLoadError::UnknownAsset);
        // A source holding textures already encoded returns the format it has; one
        // the channel's key does not expect would be cached under the wrong key.
        if (!img || img->format != format)
        {
            bundle.channels[ch].failed = true;
            continue;
        }

        if (!list)
        {
            list = cache.AcquireUploadList(); // pooled: no per-material create/destroy
            list->open();
        }
        const std::string name = source->Describe(channelIds[ch]);
        nvrhi::TextureHandle texture = Texture::CreateImage(device, *img, name.c_str());
        Texture::RecordMips(list, texture, *img);

        bundle.channels[ch].texture = texture;
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

void AssetCache::StartMaterialLoad(Core::AssetId id, Geometry::MaterialData data,
                                   std::array<Core::AssetId, 5> channelIds, std::uint64_t epoch)
{
    // The material was loaded on the main thread; the worker loads the channel
    // textures AND records their GPU uploads (P1). The main thread then only
    // submits the recorded list, adopts the textures, and builds.
    const std::atomic<std::uint64_t> *loadEpoch = &_loadEpoch;
    _jobs
    ->Run(Core::Pool::Worker,
          [this, data = std::move(data), channelIds, epoch, loadEpoch]() mutable
          { return DecodeAndRecordMaterialChannels(*this, std::move(data), channelIds, epoch, *loadEpoch); })
    .Then(Core::Pool::Main, [this, id, epoch](MaterialLoadBundle bundle)
          { OnMaterialLoaded(id, epoch, std::move(bundle)); });
}

void AssetCache::OnMaterialLoaded(Core::AssetId id, std::uint64_t epoch, MaterialLoadBundle bundle)
{
    --_activeLoads;
    PumpLoadQueue();

    if (epoch != _loadEpoch.load(std::memory_order_relaxed))
        return; // Stale (see OnMeshLoaded): return before erasing so a stale completion can't drop
                // a current-epoch job's loading marker and trigger a duplicate load. The bundle
                // (worker-created textures + list) is dropped here and freed.

    // Enqueue the publish (O(1)); the decode + upload recording already happened on
    // the worker, so this is cheap. The id stays in _materialLoading until
    // PublishMaterial submits + adopts, so it keeps rendering with the fallback.
    _pendingPublishes.push_back(PendingPublish{.isMaterial = true,
                                               .id         = id,
                                               .epoch      = epoch,
                                               .byteSize   = bundle.decodedBytes,
                                               .mesh       = {}, // mesh-only
                                               .material   = std::move(bundle)});
}

void AssetCache::PublishMesh(PendingPublish publish)
{
    _meshLoading.erase(publish.id);
    _slotMaterials[publish.id] = std::move(publish.mesh.slotMaterials);
    MeshBuffer &buffer = _meshes[publish.id];
    if (publish.mesh.staging != nullptr)
    {
        // The worker already copied the geometry into a GPU staging buffer, so this
        // records two copyBuffers — O(1) in mesh size, no main-thread memcpy.
        buffer.UploadStaged(_arena, std::move(publish.mesh.data), publish.mesh.staging, publish.mesh.vertexCount,
                            publish.mesh.indexCount, BeginUpload());
        // Hand the buffer to the batch so FlushUploads can park it behind an event
        // query and recycle it, instead of letting it be destroyed by a later GC.
        _batchStaging.push_back(std::move(publish.mesh.staging));
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
    _materialLoading.erase(publish.id);

    // A load kicked before the editor took this material over. Its payload is the
    // stored contents, which are older than the live edits — publishing it would
    // silently revert whatever has not been saved yet. The decoded textures go
    // out of scope here; the recorded upload list is simply not submitted.
    if (_authoredMaterials.contains(publish.id))
    {
        return;
    }

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
    uint32_t *slots[5] = {&textures.baseColor, &textures.normal, &textures.metallicRoughness,
                          &textures.occlusion, &textures.emissive};
    for (std::size_t ch = 0; ch < kChannels.size(); ++ch)
    {
        RecordedChannel &rc = publish.material.channels[ch];
        if (rc.texture)
        {
            // Dedup by (id, space, format): a texture shared across materials
            // resolves to one resident copy. A duplicate's worker texture is simply
            // not adopted — its recorded upload still executes (harmless: it targets
            // a texture we release, kept alive by the list until the submit retires)
            // and is freed. The format has to be the one the worker actually encoded,
            // or the publish would look up a key nothing was stored under.
            const TextureKey key{rc.id, ChannelFormat(ch).space, EffectiveFormat(ChannelFormat(ch).format)};
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
        else if (rc.failed)
        {
            // The material named a texture and it could not be produced. The
            // neutral default would render as a perfectly ordinary white surface,
            // so this takes the checkerboard instead.
            Core::Log::Warn("AssetCache: failed to load texture '{}' - drawing the error pattern.", Describe(rc.id));
            ++_failedTextures;

            const TextureKey key{rc.id, ChannelFormat(ch).space, Image::PixelFormat::Rgba8};
            Texture &texture = _textures[key];
            if (!texture.IsValid())
            {
                texture.UploadErrorPattern(_device, Describe(rc.id).c_str(), BeginUpload());
            }
            *slots[ch] = texture.IsValid() ? RegisterBindlessTexture(texture) : 0u;
        }
        else
        {
            const Texture *fallback =
                ResolveTexture(kChannels[ch].fallback, ChannelFormat(ch).space, Image::PixelFormat::Rgba8);
            *slots[ch] = fallback != nullptr ? fallback->BindlessIndex() : 0u;
        }
    }

    Material &material = _materials[publish.id];
    material.Create(_device, MintMaterialId(), publish.material.data, textures);
    WriteMaterialToTable(material, BeginUpload());
}

nvrhi::CommandListHandle AssetCache::AcquireUploadList()
{
    {
        std::lock_guard<std::mutex> lock(_poolMutex);
        if (!_freeUploadLists.empty())
        {
            nvrhi::CommandListHandle list = std::move(_freeUploadLists.back());
            _freeUploadLists.pop_back();
            return list;
        }
    }
    // Pool empty (first loads, or more concurrent loads than before): create one.
    // A generous upload chunk is right for a POOLED list — it is reused for the
    // process's lifetime, so the chunk is amortized instead of being allocated and
    // freed per asset (which is what made this expensive in the first place).
    nvrhi::CommandListParameters params;
    params.uploadChunkSize = kUploadChunkSize;
    return _device->createCommandList(params);
}

nvrhi::BufferHandle AssetCache::AcquireStagingBuffer(std::uint64_t bytes)
{
    {
        std::lock_guard<std::mutex> lock(_poolMutex);
        // First buffer big enough wins. Sizes cluster (a level's meshes are mostly
        // similar), so this stays effectively O(1) without bucketing.
        for (std::size_t i = 0; i < _freeStagingBuffers.size(); ++i)
        {
            if (_freeStagingBuffers[i]->getDesc().byteSize >= bytes)
            {
                nvrhi::BufferHandle buffer = std::move(_freeStagingBuffers[i]);
                _freeStagingBuffers.erase(_freeStagingBuffers.begin() + static_cast<std::ptrdiff_t>(i));
                return buffer;
            }
        }
    }

    nvrhi::BufferDesc desc;
    desc.byteSize = bytes;
    desc.cpuAccess = nvrhi::CpuAccessMode::Write;
    desc.debugName = "AssetCache::MeshStaging";
    desc.initialState = nvrhi::ResourceStates::CopySource;
    desc.keepInitialState = true;
    return _device->createBuffer(desc);
}

void AssetCache::RecycleRetiredStaging()
{
    ASSISI_PROFILE_SCOPE("recycle-staging");

    std::lock_guard<std::mutex> lock(_poolMutex);
    for (std::size_t i = 0; i < _stagingInFlight.size();)
    {
        // pollEventQuery is non-blocking: a batch still executing is simply left for
        // a later pump. Never wait here — that would trade a GC spike for a GPU stall.
        if (_device->pollEventQuery(_stagingInFlight[i].query))
        {
            // Effect end of the flow opened when this batch was parked, so the
            // viewer draws an arrow from the frame that caused the cost to the
            // (much later) frame that pays it.
            ASSISI_PROFILE_FLOW_END("staging-lifetime", _stagingInFlight[i].chiaraFlowId);

            for (nvrhi::BufferHandle &buffer : _stagingInFlight[i].buffers)
                _freeStagingBuffers.push_back(std::move(buffer));
            _device->resetEventQuery(_stagingInFlight[i].query);
            _freeEventQueries.push_back(std::move(_stagingInFlight[i].query));
            _stagingInFlight.erase(_stagingInFlight.begin() + static_cast<std::ptrdiff_t>(i));
        }
        else
        {
            ++i;
        }
    }
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

    ASSISI_PROFILE_SCOPE("flush-uploads");
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

    // Return the worker lists to the pool rather than dropping them. Recycling is
    // safe immediately: nvrhi round-robins each list's internal command buffers and
    // fences upload-chunk reuse against the completed submission, so reopening one
    // never disturbs work still in flight. Dropping them instead would hand the
    // destruction to the next frame's runGarbageCollection — a main-thread spike.
    {
        std::lock_guard<std::mutex> lock(_poolMutex);
        for (nvrhi::CommandListHandle &list : _uploadBatch)
            _freeUploadLists.push_back(std::move(list));

        // Staging buffers must wait for the GPU: a worker memcpys into them. Park
        // them behind an event query on this submit; RecycleRetiredStaging frees
        // them for reuse once it retires (polled, never waited on).
        if (!_batchStaging.empty())
        {
            StagingInFlight parked;
            if (!_freeEventQueries.empty())
            {
                parked.query = std::move(_freeEventQueries.back());
                _freeEventQueries.pop_back();
            }
            else
            {
                parked.query = _device->createEventQuery();
            }
            _device->setEventQuery(parked.query, nvrhi::CommandQueue::Graphics);
            parked.buffers = std::move(_batchStaging);

            // Cause end of the flow, opened inside the flush-uploads scope so the
            // arrow starts on the work that created the debt.
            parked.chiaraFlowId = Chiara::NewFlowId();
            ASSISI_PROFILE_FLOW_BEGIN("staging-lifetime", parked.chiaraFlowId);

            _stagingInFlight.push_back(std::move(parked));
            _batchStaging.clear();
        }
    }
    _uploadBatch.clear();
    _uploadOpen = false;
}

void AssetCache::PumpPublishes(double timeBudgetMs, std::size_t byteBudget)
{
    ASSISI_PROFILE_SCOPE("pump-publishes");

    // Reclaim staging buffers whose submit has retired, so the next loads reuse
    // them instead of allocating. Cheap (a poll per parked batch) and worth doing
    // even when nothing is queued, so buffers don't sit parked after a load ends.
    RecycleRetiredStaging();

    if (_pendingPublishes.empty())
    {
        // The queue is empty, so every texture this load was going to produce has
        // either landed or failed — the one moment a total means anything.
        ReportTextureFailures();
        return;
    }

    using Clock = std::chrono::steady_clock;
    const auto elapsedMsSince = [](Clock::time_point t)
                                { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); };

    // `start` drives the pump's time budget. Per-phase timing is left to the
    // profile scopes below, which give the same mesh/material/flush split nested
    // under the frame — hand-summed milliseconds would only be a second set of
    // numbers to keep honest.
    const Clock::time_point start     = Clock::now();
    std::size_t bytes     = 0;
    std::size_t meshCount = 0;
    std::size_t matCount  = 0;
    bool any       = false;
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

        const std::size_t publishBytes = publish.byteSize;
        if (publish.isMaterial)
        {
            // The scope name is the aggregation key, so it stays constant and the
            // asset goes in an arg — naming the scope after the path would shatter
            // cross-frame aggregation into one bucket per asset.
            ASSISI_PROFILE_SCOPE("publish-material");
            ASSISI_PROFILE_ARG_STR("asset", Describe(publish.id));
            ASSISI_PROFILE_ARG_U64("bytes", static_cast<std::uint64_t>(publishBytes));

            PublishMaterial(std::move(publish));
            ++matCount;
        }
        else
        {
            ASSISI_PROFILE_SCOPE("publish-mesh");
            ASSISI_PROFILE_ARG_STR("asset", Describe(publish.id));
            ASSISI_PROFILE_ARG_U64("bytes", static_cast<std::uint64_t>(publishBytes));

            PublishMesh(std::move(publish));
            ++meshCount;
        }
        bytes += publishBytes;
        any = true;
    }

    // One submit for the whole batch (P0): draws in the frame that follows see it.
    if (any)
        FlushUploads();

    // The streaming plan's permanent regression sensor (R5). Together with the
    // scopes above these give the phase split: mesh = arena memcpy, material =
    // descriptor writes, flush = submit; all-low-but-total-high means the main
    // thread was preempted.
    ASSISI_PROFILE_COUNTER("stream/pending-publishes", static_cast<double>(_pendingPublishes.size()));
    ASSISI_PROFILE_COUNTER("stream/pump-bytes", static_cast<double>(bytes));
    ASSISI_PROFILE_COUNTER("stream/mesh-count", static_cast<double>(meshCount));
    ASSISI_PROFILE_COUNTER("stream/mat-count", static_cast<double>(matCount));

    // Emitted here, not from a central pump: this is the only code that moves
    // these numbers, and a memory graph wants its samples where the change happened.
    ASSISI_PROFILE_COUNTER("mem/arena-vertex-used", static_cast<double>(_arena.VertexUsedBytes()));
    ASSISI_PROFILE_COUNTER("mem/arena-vertex-capacity", static_cast<double>(_arena.VertexCapacityBytes()));
    ASSISI_PROFILE_COUNTER("mem/arena-index-used", static_cast<double>(_arena.IndexUsedBytes()));
    ASSISI_PROFILE_COUNTER("mem/arena-index-capacity", static_cast<double>(_arena.IndexCapacityBytes()));

    {
        std::lock_guard<std::mutex> lock(_poolMutex);
        std::size_t parkedBuffers = 0;
        for (const StagingInFlight &parked : _stagingInFlight)
        {
            parkedBuffers += parked.buffers.size();
        }
        ASSISI_PROFILE_COUNTER("mem/staging-parked-buffers", static_cast<double>(parkedBuffers));
        ASSISI_PROFILE_COUNTER("mem/staging-free-buffers", static_cast<double>(_freeStagingBuffers.size()));
    }
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

    // waitForIdle above retired every submit, so all parked staging is reusable —
    // reclaim it without polling. The pools themselves survive Clear() deliberately:
    // they exist so a level load does not churn GPU allocations, and the next load
    // is exactly when they are wanted.
    {
        std::lock_guard<std::mutex> lock(_poolMutex);
        for (StagingInFlight &parked : _stagingInFlight)
        {
            for (nvrhi::BufferHandle &buffer : parked.buffers)
                _freeStagingBuffers.push_back(std::move(buffer));
            _device->resetEventQuery(parked.query);
            _freeEventQueries.push_back(std::move(parked.query));
        }
        _stagingInFlight.clear();
        for (nvrhi::BufferHandle &buffer : _batchStaging)
            _freeStagingBuffers.push_back(std::move(buffer));
        _batchStaging.clear();
    }

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
    _slotMaterials.clear();
    _missingMeshWarned.clear();
    _missingMaterialWarned.clear();
    _authoredMaterials.clear();

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
