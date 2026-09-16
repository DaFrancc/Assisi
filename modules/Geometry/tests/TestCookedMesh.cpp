/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCookedMesh.cpp
/// @brief A cooked mesh reads back as the mesh that was written, and a blob that
/// is not one is refused rather than half-read.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Geometry/CookedMesh.hpp>
#include <Assisi/Geometry/MeshData.hpp>

using namespace Assisi;

namespace
{

/// Two triangles in two submeshes over two LODs, with values no default holds,
/// so a field the reader skipped comes back as a default and the comparison fails.
Geometry::MeshData TwoLodQuad()
{
    Geometry::MeshData mesh;
    for (std::uint32_t i = 0; i < 4; ++i)
    {
        const float f = static_cast<float>(i) + 0.25f;
        mesh.Vertices.push_back(Geometry::Vertex{.Position           = {f, f * 2.f, f * 3.f},
                                                 .Normal             = {0.f, f, 1.f},
                                                 .TextureCoordinates = {f * 0.5f, f * 0.75f},
                                                 .Tangent            = {1.f, 0.f, f, -1.f}});
    }
    mesh.Indices = {0, 1, 2, 2, 1, 3};

    Geometry::SubMesh first;
    first.IndexOffset           = 0;
    first.IndexCount            = 3;
    first.MaterialSlot          = 0;
    first.LocalBounds.center    = {1.f, 2.f, 3.f};
    first.LocalBounds.radius    = 4.f;
    first.LocalAabb.min         = {-1.f, -2.f, -3.f};
    first.LocalAabb.max         = {5.f, 6.f, 7.f};
    Geometry::SubMesh second    = first;
    second.IndexOffset          = 3;
    second.MaterialSlot         = 1;
    second.LocalBounds.radius   = 9.f;
    mesh.SubMeshes              = {first, second};

    mesh.Lods = {Geometry::LodRange{.FirstSubMesh = 0, .SubMeshCount = 1, .ScreenSizeThreshold = 0.5f},
                 Geometry::LodRange{.FirstSubMesh = 1, .SubMeshCount = 1, .ScreenSizeThreshold = 0.125f}};

    mesh.Materials.resize(2);
    mesh.LocalBounds.center = {0.5f, 0.25f, 0.125f};
    mesh.LocalBounds.radius = 11.f;
    mesh.LocalAabb.min      = {-8.f, -9.f, -10.f};
    mesh.LocalAabb.max      = {8.f, 9.f, 10.f};
    mesh.BoundsComputed     = true;
    return mesh;
}

const std::vector<Core::AssetId> kSlots{Core::DerivedAssetId("materials/a.amat"),
                                        Core::DerivedAssetId("materials/b.amat")};

std::vector<std::byte> Cook(const Geometry::MeshData &mesh)
{
    Core::BitWriter writer;
    Geometry::WriteCookedMesh(writer, mesh, kSlots);
    const std::span<const std::byte> bytes = writer.Data();
    return {bytes.begin(), bytes.end()};
}

} // namespace

TEST_CASE("A cooked mesh reads back as the mesh that was written")
{
    const Geometry::MeshData source = TwoLodQuad();
    const std::vector<std::byte> bytes = Cook(source);

    const std::expected<Geometry::CookedMesh, Geometry::CookedMeshError> read = Geometry::ReadCookedMesh(bytes);
    REQUIRE(read.has_value());
    const Geometry::MeshData &mesh = read->mesh;

    REQUIRE(mesh.Vertices.size() == source.Vertices.size());
    for (std::size_t i = 0; i < mesh.Vertices.size(); ++i)
    {
        CHECK(mesh.Vertices[i].Position == source.Vertices[i].Position);
        CHECK(mesh.Vertices[i].Normal == source.Vertices[i].Normal);
        CHECK(mesh.Vertices[i].TextureCoordinates == source.Vertices[i].TextureCoordinates);
        CHECK(mesh.Vertices[i].Tangent == source.Vertices[i].Tangent);
    }
    CHECK(mesh.Indices == source.Indices);

    REQUIRE(mesh.SubMeshes.size() == source.SubMeshes.size());
    for (std::size_t i = 0; i < mesh.SubMeshes.size(); ++i)
    {
        CHECK(mesh.SubMeshes[i].IndexOffset == source.SubMeshes[i].IndexOffset);
        CHECK(mesh.SubMeshes[i].IndexCount == source.SubMeshes[i].IndexCount);
        CHECK(mesh.SubMeshes[i].MaterialSlot == source.SubMeshes[i].MaterialSlot);
        CHECK(mesh.SubMeshes[i].LocalBounds.center == source.SubMeshes[i].LocalBounds.center);
        CHECK(mesh.SubMeshes[i].LocalBounds.radius == source.SubMeshes[i].LocalBounds.radius);
        CHECK(mesh.SubMeshes[i].LocalAabb.min == source.SubMeshes[i].LocalAabb.min);
        CHECK(mesh.SubMeshes[i].LocalAabb.max == source.SubMeshes[i].LocalAabb.max);
    }

    REQUIRE(mesh.Lods.size() == source.Lods.size());
    for (std::size_t i = 0; i < mesh.Lods.size(); ++i)
    {
        CHECK(mesh.Lods[i].FirstSubMesh == source.Lods[i].FirstSubMesh);
        CHECK(mesh.Lods[i].SubMeshCount == source.Lods[i].SubMeshCount);
        CHECK(mesh.Lods[i].ScreenSizeThreshold == source.Lods[i].ScreenSizeThreshold);
    }

    CHECK(mesh.LocalBounds.center == source.LocalBounds.center);
    CHECK(mesh.LocalBounds.radius == source.LocalBounds.radius);
    CHECK(mesh.LocalAabb.min == source.LocalAabb.min);
    CHECK(mesh.LocalAabb.max == source.LocalAabb.max);
    CHECK(mesh.BoundsComputed);

    // One material slot per id, so a slot index into the mesh stays in range.
    CHECK(mesh.Materials.size() == kSlots.size());
    CHECK(read->slotMaterials == kSlots);
}

TEST_CASE("A truncated mesh blob is refused rather than half-read")
{
    const std::vector<std::byte> bytes = Cook(TwoLodQuad());

    // Every length short of the whole: a reader that trusted a count would read
    // past the end or return a mesh missing its tail.
    for (std::size_t length = 0; length < bytes.size(); ++length)
    {
        CAPTURE(length);
        const std::span<const std::byte> cut{bytes.data(), length};
        CHECK_FALSE(Geometry::ReadCookedMesh(cut).has_value());
    }
}

TEST_CASE("A blob of another kind is not read as a mesh")
{
    Core::BitWriter writer;
    Core::WriteCookedHeader(writer, Core::CookedKind::Texture);
    writer.WriteUInt8(Geometry::kMeshPayloadVersion);

    const std::expected<Geometry::CookedMesh, Geometry::CookedMeshError> read =
        Geometry::ReadCookedMesh(writer.Data());
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == Geometry::CookedMeshError::NotAMesh);
}

TEST_CASE("A mesh blob of a version this build does not read is refused")
{
    std::vector<std::byte> bytes = Cook(TwoLodQuad());
    Core::BitWriter header;
    Core::WriteCookedHeader(header, Core::CookedKind::Mesh);
    bytes[header.Data().size()] = std::byte{Geometry::kMeshPayloadVersion + 1};

    const std::expected<Geometry::CookedMesh, Geometry::CookedMeshError> read = Geometry::ReadCookedMesh(bytes);
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == Geometry::CookedMeshError::UnsupportedVersion);
}

TEST_CASE("A mesh blob whose tables disagree is refused")
{
    // A forged or corrupt blob can frame correctly and still name an index past
    // the vertex array, which the GPU would read out of bounds.
    Geometry::MeshData broken = TwoLodQuad();
    broken.Indices[0]         = 99;

    const std::expected<Geometry::CookedMesh, Geometry::CookedMeshError> read =
        Geometry::ReadCookedMesh(Cook(broken));
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == Geometry::CookedMeshError::Invalid);
}
