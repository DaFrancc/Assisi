/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SourceAssets.hpp
/// @brief The renderer's assets read from the source tree: what the editor loads.
///
/// Every asset is found through the asset database and decoded from the file an
/// author edits — a glTF imported, an image decoded and block-compressed at the
/// fast tier, a material parsed from its JSON — so a change saved in the editor
/// is what the next load sees. A shipped game has none of these files and uses
/// the cooked source instead.

#include <Assisi/Render/AssetSource.hpp>

namespace Assisi::Core
{
class AssetDatabase;
}

namespace Assisi::App
{

class SourceAssetSource final : public Render::AssetSource
{
public:
    /// @param database resolves ids to paths and back, and says which material each
    ///        mesh slot draws with. Must outlive this source, and must not be rebuilt
    ///        while a load is running on a worker.
    explicit SourceAssetSource(const Core::AssetDatabase &database) noexcept;

    [[nodiscard]] std::expected<Geometry::CookedMesh, Render::AssetLoadError>
    LoadMesh(const Core::AssetId &id) const override;

    [[nodiscard]] std::expected<Geometry::MaterialData, Render::AssetLoadError>
    LoadMaterial(const Core::AssetId &id) const override;

    [[nodiscard]] std::expected<Image::DecodedImage, Render::AssetLoadError>
    LoadTexture(const Core::AssetId &id, Image::ColorSpace space, Image::PixelFormat format) const override;

    [[nodiscard]] std::expected<std::vector<std::byte>, Render::AssetLoadError>
    LoadShader(std::string_view vpath) const override;

    [[nodiscard]] std::expected<Core::AssetId, Render::AssetLoadError> Resolve(std::string_view vpath) const override;

    [[nodiscard]] std::string Describe(const Core::AssetId &id) const override;

private:
    const Core::AssetDatabase *_database;
};

} // namespace Assisi::App
