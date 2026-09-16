/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/SourceAssets.hpp>

#include <Assisi/Core/AssetDatabase.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Geometry/MaterialFile.hpp>
#include <Assisi/Geometry/MeshImporter.hpp>
#include <Assisi/Image/Compress.hpp>
#include <Assisi/Image/Decode.hpp>

#include <optional>
#include <string>

namespace Assisi::App
{

SourceAssetSource::SourceAssetSource(const Core::AssetDatabase &database) noexcept : _database(&database)
{
}

std::expected<Geometry::CookedMesh, Render::AssetLoadError> SourceAssetSource::LoadMesh(const Core::AssetId &id) const
{
    const std::optional<std::string> path = _database->PathFor(id);
    if (!path)
    {
        return std::unexpected(Render::AssetLoadError::UnknownAsset);
    }

    const Core::AssetDatabase &database = *_database;
    const Geometry::AssetIdResolver resolve = [&database](std::string_view texturePath)
                                              { return database.IdFor(texturePath).value_or(Core::AssetId{}); };
    std::expected<Geometry::MeshData, Geometry::MeshImportError> imported = Geometry::ImportMesh(*path, resolve);
    if (!imported)
    {
        return std::unexpected(imported.error() == Geometry::MeshImportError::ReadFailed
                                   ? Render::AssetLoadError::Unreadable
                                   : Render::AssetLoadError::Undecodable);
    }

    // What each slot draws with lives in the mesh's sidecar after import, not in
    // the import's own material table, which is only the default a `.amat` was
    // exploded from.
    Geometry::CookedMesh mesh;
    mesh.slotMaterials.reserve(imported->Materials.size());
    for (std::size_t slot = 0; slot < imported->Materials.size(); ++slot)
    {
        mesh.slotMaterials.push_back(database.SlotMaterial(id, static_cast<std::uint32_t>(slot)));
    }
    mesh.mesh = std::move(*imported);
    return mesh;
}

std::expected<Geometry::MaterialData, Render::AssetLoadError>
SourceAssetSource::LoadMaterial(const Core::AssetId &id) const
{
    const std::optional<std::string> path = _database->PathFor(id);
    if (!path)
    {
        return std::unexpected(Render::AssetLoadError::UnknownAsset);
    }
    const std::expected<std::string, Core::AssetError> text = Core::AssetSystem::ReadText(*path);
    if (!text)
    {
        return std::unexpected(Render::AssetLoadError::Unreadable);
    }
    std::expected<Geometry::MaterialData, Geometry::MaterialFileError> data = Geometry::DeserializeMaterial(*text);
    if (!data)
    {
        return std::unexpected(Render::AssetLoadError::Undecodable);
    }
    return std::move(*data);
}

std::expected<Image::DecodedImage, Render::AssetLoadError>
SourceAssetSource::LoadTexture(const Core::AssetId &id, Image::ColorSpace space, Image::PixelFormat format) const
{
    const std::optional<std::string> path = _database->PathFor(id);
    if (!path)
    {
        return std::unexpected(Render::AssetLoadError::UnknownAsset);
    }
    std::expected<Image::DecodedImage, Core::AssetError> decoded = Image::DecodeImage(*path, space);
    if (!decoded)
    {
        return std::unexpected(Render::AssetLoadError::Undecodable);
    }
    if (!Image::IsBlockCompressed(format))
    {
        return std::move(*decoded);
    }

    // The fast tier: this encode is one somebody is waiting on. The best one
    // belongs to the cook, which runs once and offline.
    std::expected<Image::DecodedImage, Core::AssetError> compressed =
        Image::Compress(*decoded, format, Image::CompressQuality::Fast);
    if (!compressed)
    {
        return std::unexpected(Render::AssetLoadError::Undecodable);
    }
    return std::move(*compressed);
}

std::expected<std::vector<std::byte>, Render::AssetLoadError>
SourceAssetSource::LoadShader(std::string_view vpath) const
{
    std::expected<std::vector<std::byte>, Core::AssetError> spirv = Core::AssetSystem::ReadBinary(vpath);
    if (!spirv)
    {
        return std::unexpected(Render::AssetLoadError::Unreadable);
    }
    return std::move(*spirv);
}

std::expected<Core::AssetId, Render::AssetLoadError> SourceAssetSource::Resolve(std::string_view vpath) const
{
    const std::optional<Core::AssetId> id = _database->IdFor(vpath);
    if (!id)
    {
        return std::unexpected(Render::AssetLoadError::UnknownAsset);
    }
    return *id;
}

std::string SourceAssetSource::Describe(const Core::AssetId &id) const
{
    return _database->PathFor(id).value_or(id.ToString());
}

} // namespace Assisi::App
