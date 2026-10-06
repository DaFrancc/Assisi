/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file CookedAssets.hpp
/// @brief The renderer's assets read from cooked blobs: what a shipped game loads.
///
/// Every asset is a blob the cook already decoded — mesh arrays, block-compressed
/// mips, a material's reflected fields, SPIR-V — so a load is a read and a parse,
/// with no importer, image decoder or encoder behind it.

#include <Assisi/Physics/CollisionSource.hpp>
#include <Assisi/Render/AssetSource.hpp>

namespace Assisi::Core
{
class AssetProvider;
}

namespace Assisi::App
{

class CookedAssetSource final : public Render::AssetSource
{
public:
    /// @param provider serves the cooked blobs by id and path. Must outlive this
    ///        source.
    explicit CookedAssetSource(const Core::AssetProvider &provider) noexcept;

    [[nodiscard]] std::expected<Geometry::CookedMesh, Render::AssetLoadError>
    LoadMesh(const Core::AssetId &id) const override;

    [[nodiscard]] std::expected<Geometry::MaterialData, Render::AssetLoadError>
    LoadMaterial(const Core::AssetId &id) const override;

    /// A cooked texture comes back in the format and colour space it was cooked in:
    /// the cook chose them from the channel every material binds it as.
    [[nodiscard]] std::expected<Image::DecodedImage, Render::AssetLoadError>
    LoadTexture(const Core::AssetId &id, Image::ColorSpace space, Image::PixelFormat format) const override;

    [[nodiscard]] std::expected<std::vector<std::byte>, Render::AssetLoadError>
    LoadShader(std::string_view vpath) const override;

    [[nodiscard]] std::expected<Core::AssetId, Render::AssetLoadError> Resolve(std::string_view vpath) const override;

    /// The id alone: a pak indexes paths by hash and holds none of their text.
    [[nodiscard]] std::string Describe(const Core::AssetId &id) const override;

private:
    const Core::AssetProvider *_provider;
};

/// @brief Collision models read from cooked meshes, which carry them.
///
/// Has nothing to read until Read() names the package: the worlds that read
/// through it are made before the package is mounted.
class CookedCollisionSource final : public Physics::CollisionSource
{
public:
    /// @param provider serves the cooked meshes by id. Must outlive this source.
    void Read(const Core::AssetProvider &provider) noexcept { _provider = &provider; }

    [[nodiscard]] std::optional<Geometry::CollisionModel> Load(Core::AssetId id) const override;

private:
    const Core::AssetProvider *_provider = nullptr;
};

} // namespace Assisi::App
