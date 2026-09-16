/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/CookedAssets.hpp>

#include <Assisi/Core/AssetProvider.hpp>
#include <Assisi/Core/CookedPayload.hpp>
#include <Assisi/Image/CookedTexture.hpp>

namespace Assisi::App
{

namespace
{

Render::AssetLoadError FromAssetError(Core::AssetError error) noexcept
{
    return error == Core::AssetError::UnknownAssetId ? Render::AssetLoadError::UnknownAsset
                                                     : Render::AssetLoadError::Unreadable;
}

} // namespace

CookedAssetSource::CookedAssetSource(const Core::AssetProvider &provider) noexcept : _provider(&provider)
{
}

std::expected<Geometry::CookedMesh, Render::AssetLoadError> CookedAssetSource::LoadMesh(const Core::AssetId &id) const
{
    const std::expected<std::vector<std::byte>, Core::AssetError> bytes = _provider->Open(id);
    if (!bytes)
    {
        return std::unexpected(FromAssetError(bytes.error()));
    }
    std::expected<Geometry::CookedMesh, Geometry::CookedMeshError> mesh = Geometry::ReadCookedMesh(*bytes);
    if (!mesh)
    {
        return std::unexpected(Render::AssetLoadError::Undecodable);
    }
    return std::move(*mesh);
}

std::expected<Geometry::MaterialData, Render::AssetLoadError>
CookedAssetSource::LoadMaterial(const Core::AssetId &id) const
{
    const std::expected<std::vector<std::byte>, Core::AssetError> bytes = _provider->Open(id);
    if (!bytes)
    {
        return std::unexpected(FromAssetError(bytes.error()));
    }
    std::expected<Geometry::MaterialData, Core::CookedPayloadError> material =
        Core::ReadReflectedBlob<Geometry::MaterialData>(*bytes);
    if (!material)
    {
        return std::unexpected(Render::AssetLoadError::Undecodable);
    }
    return std::move(*material);
}

std::expected<Image::DecodedImage, Render::AssetLoadError>
CookedAssetSource::LoadTexture(const Core::AssetId &id, Image::ColorSpace, Image::PixelFormat) const
{
    const std::expected<std::vector<std::byte>, Core::AssetError> bytes = _provider->Open(id);
    if (!bytes)
    {
        return std::unexpected(FromAssetError(bytes.error()));
    }
    std::expected<Image::DecodedImage, Image::CookedTextureError> image = Image::ReadCookedTexture(*bytes);
    if (!image)
    {
        return std::unexpected(Render::AssetLoadError::Undecodable);
    }
    return std::move(*image);
}

std::expected<std::vector<std::byte>, Render::AssetLoadError>
CookedAssetSource::LoadShader(std::string_view vpath) const
{
    const std::expected<Core::AssetId, Render::AssetLoadError> id = Resolve(vpath);
    if (!id)
    {
        return std::unexpected(id.error());
    }
    const std::expected<std::vector<std::byte>, Core::AssetError> bytes = _provider->Open(*id);
    if (!bytes)
    {
        return std::unexpected(FromAssetError(bytes.error()));
    }
    std::expected<std::vector<std::byte>, Core::CookedPayloadError> spirv = Core::ReadShaderBlob(*bytes);
    if (!spirv)
    {
        return std::unexpected(Render::AssetLoadError::Undecodable);
    }
    return std::move(*spirv);
}

std::expected<Core::AssetId, Render::AssetLoadError> CookedAssetSource::Resolve(std::string_view vpath) const
{
    const std::expected<Core::AssetId, Core::AssetError> id = _provider->Resolve(vpath);
    if (!id)
    {
        return std::unexpected(FromAssetError(id.error()));
    }
    return *id;
}

std::string CookedAssetSource::Describe(const Core::AssetId &id) const
{
    return id.ToString();
}

} // namespace Assisi::App
