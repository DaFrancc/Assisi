/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCookedAssets.cpp
/// @brief The cooked asset source hands back what the cook wrote, and says which
/// of "not there", "not readable" and "not that kind of asset" a failure is.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include <Assisi/App/CookedAssets.hpp>
#include <Assisi/Core/AssetProvider.hpp>
#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/CookedPayload.hpp>
#include <Assisi/Core/Reflect/AssetTypeRegistry.hpp>
#include <Assisi/Geometry/CookedMesh.hpp>
#include <Assisi/Geometry/MaterialData.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Geometry/MeshValidate.hpp>
#include <Assisi/Image/CookedTexture.hpp>

using namespace Assisi;

namespace
{

/// Cooked blobs by path, held in memory: a pak without the file.
class MemoryProvider final : public Core::AssetProvider
{
public:
    Core::AssetId Add(std::string_view vpath, std::vector<std::byte> bytes)
    {
        const Core::AssetId id = Core::DerivedAssetId(vpath);
        _blobs[id]             = std::move(bytes);
        return id;
    }

    [[nodiscard]] std::expected<std::vector<std::byte>, Core::AssetError> Open(Core::AssetId id) const override
    {
        const auto found = _blobs.find(id);
        if (found == _blobs.end())
        {
            return std::unexpected(Core::AssetError::UnknownAssetId);
        }
        return found->second;
    }

    [[nodiscard]] std::expected<Core::AssetId, Core::AssetError> Resolve(std::string_view vpath) const override
    {
        const Core::AssetId id = Core::DerivedAssetId(vpath);
        if (!_blobs.contains(id))
        {
            return std::unexpected(Core::AssetError::UnknownAssetId);
        }
        return id;
    }

private:
    std::unordered_map<Core::AssetId, std::vector<std::byte>> _blobs;
};

std::vector<std::byte> Bytes(const Core::BitWriter &writer)
{
    const std::span<const std::byte> bytes = writer.Data();
    return {bytes.begin(), bytes.end()};
}

/// One triangle, one submesh, one slot.
Geometry::MeshData Triangle()
{
    Geometry::MeshData mesh;
    mesh.Vertices.push_back(Geometry::Vertex{.Position = {0.f, 0.f, 0.f}});
    mesh.Vertices.push_back(Geometry::Vertex{.Position = {1.f, 0.f, 0.f}});
    mesh.Vertices.push_back(Geometry::Vertex{.Position = {0.f, 1.f, 0.f}});
    mesh.Indices = {0, 1, 2};
    mesh.Materials.resize(1);
    Geometry::EnsureSubMeshTables(mesh);
    Geometry::EnsureMeshBounds(mesh);
    return mesh;
}

/// A 4x4 BC7 image with its full mip chain.
Image::DecodedImage Bc7Chain()
{
    constexpr std::uint32_t kEdge        = 4;
    constexpr std::size_t kBc7BlockBytes = 16;
    constexpr std::size_t kLevels        = 3; // 4x4, 2x2 and 1x1 texels, one block each.

    Image::DecodedImage image;
    image.width      = kEdge;
    image.height     = kEdge;
    image.format     = Image::PixelFormat::Bc7;
    image.colorSpace = Image::ColorSpace::Srgb;
    for (std::size_t level = 0; level < kLevels; ++level)
    {
        image.mips.emplace_back(kBc7BlockBytes, static_cast<unsigned char>(level + 1));
    }
    return image;
}

} // namespace

TEST_CASE("The cooked asset source reads each kind of blob the cook writes")
{
    MemoryProvider provider;
    const App::CookedAssetSource source(provider);

    const Core::AssetId materialId = Core::DerivedAssetId("materials/red.amat");
    {
        Core::BitWriter writer;
        Geometry::WriteCookedMesh(writer, Triangle(), std::array{materialId});
        (void)provider.Add("meshes/tri.glb", Bytes(writer));
    }
    {
        Geometry::MaterialData material;
        material.RoughnessFactor = 0.25f;
        const Core::Reflect::AssetTypeMeta *meta =
            Core::Reflect::AssetTypeRegistry::Instance().Find(std::type_index(typeid(Geometry::MaterialData)));
        REQUIRE(meta != nullptr);
        Core::BitWriter writer;
        REQUIRE(Core::WriteReflectedBlob(writer, *meta, &material));
        (void)provider.Add("materials/red.amat", Bytes(writer));
    }
    {
        Core::BitWriter writer;
        Image::WriteCookedTexture(writer, Bc7Chain());
        (void)provider.Add("textures/red.png", Bytes(writer));
    }
    constexpr std::array kSpirv{std::byte{0x03}, std::byte{0x02}, std::byte{0x23}, std::byte{0x07}};
    {
        Core::BitWriter writer;
        Core::WriteShaderBlob(writer, kSpirv);
        (void)provider.Add("shaders/mesh.vert.spv", Bytes(writer));
    }

    const auto mesh = source.LoadMesh(Core::DerivedAssetId("meshes/tri.glb"));
    REQUIRE(mesh.has_value());
    CHECK(mesh->mesh.Indices.size() == 3);
    REQUIRE(mesh->slotMaterials.size() == 1);
    CHECK(mesh->slotMaterials.front() == materialId);

    const auto material = source.LoadMaterial(materialId);
    REQUIRE(material.has_value());
    CHECK(material->RoughnessFactor == doctest::Approx(0.25f));

    // Asked for as linear RGBA, and handed back as cooked: the cook chose the
    // format from the channel, and a load does no encode.
    const auto texture = source.LoadTexture(Core::DerivedAssetId("textures/red.png"), Image::ColorSpace::Linear,
                                            Image::PixelFormat::Rgba8);
    REQUIRE(texture.has_value());
    CHECK(texture->format == Image::PixelFormat::Bc7);
    CHECK(texture->colorSpace == Image::ColorSpace::Srgb);
    CHECK(texture->mips.size() == 3);

    const auto shader = source.LoadShader("shaders/mesh.vert.spv");
    REQUIRE(shader.has_value());
    CHECK(std::ranges::equal(*shader, kSpirv));
}

TEST_CASE("The cooked asset source tells a missing asset from one of the wrong kind")
{
    MemoryProvider provider;
    const App::CookedAssetSource source(provider);

    constexpr std::array kSpirv{std::byte{0x03}, std::byte{0x02}};
    Core::BitWriter writer;
    Core::WriteShaderBlob(writer, kSpirv);
    const Core::AssetId shaderId = provider.Add("shaders/mesh.vert.spv", Bytes(writer));

    const auto missing = source.LoadMesh(Core::DerivedAssetId("meshes/none.glb"));
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == Render::AssetLoadError::UnknownAsset);

    const auto missingShader = source.LoadShader("shaders/none.frag.spv");
    REQUIRE_FALSE(missingShader.has_value());
    CHECK(missingShader.error() == Render::AssetLoadError::UnknownAsset);

    const auto notAMesh = source.LoadMesh(shaderId);
    REQUIRE_FALSE(notAMesh.has_value());
    CHECK(notAMesh.error() == Render::AssetLoadError::Undecodable);

    const auto notAMaterial = source.LoadMaterial(shaderId);
    REQUIRE_FALSE(notAMaterial.has_value());
    CHECK(notAMaterial.error() == Render::AssetLoadError::Undecodable);

    const auto notATexture = source.LoadTexture(shaderId, Image::ColorSpace::Srgb, Image::PixelFormat::Bc7);
    REQUIRE_FALSE(notATexture.has_value());
    CHECK(notATexture.error() == Render::AssetLoadError::Undecodable);
}
