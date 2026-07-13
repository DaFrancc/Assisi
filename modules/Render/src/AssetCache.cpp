/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <string_view>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/AssetCache.hpp>
#include <Assisi/Render/DefaultMeshes.hpp>

namespace Assisi::Render
{

namespace
{
/// @brief The fallback mesh path used for empty/unrecognised mesh references.
const Core::AssetPath kCubePrimitive{std::string_view{"prim://cube"}};
} // namespace

void AssetCache::Initialize(nvrhi::IDevice *device, ColorSpace textureColorSpace)
{
    _device = device;
    _textureColorSpace = textureColorSpace;
    _primitiveFactories.emplace(kCubePrimitive, &CreateUnitCubeMesh);
}

const MeshBuffer *AssetCache::ResolvePrimitive(const Core::AssetPath &path)
{
    if (std::unordered_map<Core::AssetPath, MeshBuffer>::iterator it = _meshes.find(path); it != _meshes.end())
        return &it->second;

    std::unordered_map<Core::AssetPath, std::function<MeshData()>>::iterator factory =
        _primitiveFactories.find(path);
    if (factory == _primitiveFactories.end())
        return nullptr;

    MeshBuffer &buffer = _meshes[path];
    buffer.Upload(_device, factory->second());
    return &buffer;
}

const MeshBuffer *AssetCache::ResolveMesh(const Core::AssetPath &path)
{
    if (const MeshBuffer *mesh = ResolvePrimitive(path))
        return mesh;

    // No mesh-file loader exists yet, so anything that isn't a known primitive
    // falls back to the cube. Warn only when the caller actually asked for
    // something (an empty path is the expected "unset" default), and only once
    // per path — this resolve runs per entity per frame.
    if (!path.Empty() && _missingMeshWarned.insert(path).second)
        Core::Log::Warn("AssetCache: no mesh for '{}', falling back to prim://cube.", path.View());

    return ResolvePrimitive(kCubePrimitive);
}

const Texture *AssetCache::ResolveTexture(const Core::AssetPath &path)
{
    if (path.Empty())
        return nullptr;

    if (std::unordered_map<Core::AssetPath, Texture>::iterator it = _textures.find(path); it != _textures.end())
        return it->second.IsValid() ? &it->second : nullptr;

    Texture &texture = _textures[path];
    if (std::expected<void, Core::AssetError> loaded =
            texture.LoadFromAssets(_device, path.View(), _textureColorSpace);
        !loaded)
    {
        Core::Log::Warn("AssetCache: failed to load texture '{}' — entity renders flat white.", path.View());
        // Keep the invalid entry so the failed load isn't retried every frame;
        // returning null makes the mesh pass fall back to its white texture.
        return nullptr;
    }
    return &texture;
}

void AssetCache::Clear()
{
    _meshes.clear();
    _textures.clear();
    _missingMeshWarned.clear();
}

} /* namespace Assisi::Render */
