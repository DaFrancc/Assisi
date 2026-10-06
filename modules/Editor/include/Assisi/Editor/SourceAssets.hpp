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

#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Physics/CollisionSource.hpp>
#include <Assisi/Render/AssetSource.hpp>

#include <functional>
#include <optional>
#include <string>

namespace Assisi::Core
{
class AssetDatabase;
}

namespace Assisi::Editor
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
  /// @brief Whether @p id's sidecar names a kind other than @p kind. A sidecar
  ///        that names none is not refused here; the cook refuses it.
  [[nodiscard]] bool UsedAsOtherThan(const Core::AssetId &id, Core::AssetKindId kind) const;

  const Core::AssetDatabase *_database;
};

/// @brief Collision models imported from the source glTF on every read, so a
///        model saved in a modelling tool is what the next read sees.
class SourceCollisionSource final : public Physics::CollisionSource
{
public:
    /// The virtual path of an asset id, or nullopt for one that is unknown.
    using PathOf = std::function<std::optional<std::string>(Core::AssetId)>;

    /// @param pathOf names each id's file. Called from whichever thread reads a
    ///        model, so it must be safe to call from several at once.
    explicit SourceCollisionSource(PathOf pathOf) noexcept : _pathOf(std::move(pathOf)) {}

    [[nodiscard]] std::optional<Geometry::CollisionModel> Load(Core::AssetId id) const override;

private:
    PathOf _pathOf;
};

} // namespace Assisi::Editor
