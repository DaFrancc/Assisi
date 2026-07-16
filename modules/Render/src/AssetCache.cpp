/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <algorithm>
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
} // namespace

void AssetCache::Initialize(nvrhi::IDevice *device, ColorSpace textureColorSpace)
{
    _device = device;
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
    // (primitive or file) is returned here on subsequent frames.
    if (const MeshBuffer *mesh = ResolvePrimitive(path))
        return mesh;

    // Not a primitive and not yet cached. Try loading it as a mesh file — but
    // only once per path: a broken path must not be re-parsed every frame, so a
    // prior failure recorded in _missingMeshWarned short-circuits to the cube.
    // An empty path is the expected "unset" default and is never a file.
    if (!path.Empty() && !_missingMeshWarned.contains(path))
    {
        // Hand the importer the path→id resolver so a glTF's texture channels are
        // stored as stable ids (resolved back to textures when a material builds).
        std::expected<Geometry::MeshData, Geometry::MeshImportError> imported =
            Geometry::ImportMesh(path.View(), _pathToId);
        if (imported)
        {
            MeshBuffer &buffer = _meshes[path];
            buffer.Upload(_arena, std::move(*imported));
            buffer.SetId(_nextMeshId++);
            return &buffer;
        }

        _missingMeshWarned.insert(path);
        Core::Log::Warn("AssetCache: no mesh for '{}' ({}), falling back to prim://cube.", path.View(),
                        Geometry::ToString(imported.error()));
    }

    return ResolvePrimitive(kCubePrimitive);
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

    // Load once: a broken/missing .amat short-circuits to the fallback and is
    // remembered, so it isn't re-read and re-parsed every frame.
    if (_missingMaterialWarned.contains(path))
        return &_fallbackMaterial;

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

    Material &material = _materials[path];
    BuildMaterial(material, *data, _nextMaterialId++);
    return &material;
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
    // _nextMaterialId is deliberately NOT reset: ids are never reused, so a stale
    // binding-set entry keyed on an old id is dead, never wrong.

    // The fallback's texture pointers dangled when _textures cleared; rebuild it
    // (and its prim:// defaults) so FallbackMaterial() stays valid immediately.
    BuildFallbackMaterial();
}

} /* namespace Assisi::Render */
