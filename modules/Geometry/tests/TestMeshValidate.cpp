/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestMeshValidate.cpp
/// @brief Every way a MeshData can fail to address itself, and the one shape
/// that passes.
///
/// Each case here is a draw that would otherwise reach the GPU: an index past
/// the vertex array reads memory belonging to another mesh, a LOD gap is
/// geometry nothing draws, and a threshold that does not fall makes every later
/// level unreachable. None of them is reported by anything at runtime, which is
/// why cook time is where the check belongs.

#include <doctest/doctest.h>

#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Geometry/MeshValidate.hpp>

using Assisi::Geometry::LodRange;
using Assisi::Geometry::MeshData;
using Assisi::Geometry::MeshValidationError;
using Assisi::Geometry::SubMesh;
using Assisi::Geometry::ToString;
using Assisi::Geometry::ValidateMesh;

namespace
{

/// Two triangles, two submeshes, two LODs — the smallest mesh that exercises
/// every table rather than passing them by being empty.
MeshData MakeTwoLodMesh()
{
    MeshData mesh;
    mesh.Vertices.resize(6);
    mesh.Indices = {0, 1, 2, 3, 4, 5};

    // Field by field rather than a designated initializer: SubMesh carries
    // bounds this fixture does not care about, and naming only some members
    // draws -Wmissing-field-initializers, which is an error here.
    SubMesh first;
    first.IndexOffset  = 0;
    first.IndexCount   = 3;
    first.MaterialSlot = 0;
    mesh.SubMeshes.push_back(first);

    SubMesh second;
    second.IndexOffset  = 3;
    second.IndexCount   = 3;
    second.MaterialSlot = 0;
    mesh.SubMeshes.push_back(second);

    mesh.Lods.push_back(LodRange{.FirstSubMesh = 0, .SubMeshCount = 1, .ScreenSizeThreshold = 0.5f});
    mesh.Lods.push_back(LodRange{.FirstSubMesh = 1, .SubMeshCount = 1, .ScreenSizeThreshold = 0.25f});

    mesh.Materials.emplace_back();
    return mesh;
}

} // namespace

TEST_CASE("A well-formed two-LOD mesh validates")
{
    // The positive control. Without it every refusal below could be passing
    // because the fixture is malformed in some way none of them names.
    CHECK(ValidateMesh(MakeTwoLodMesh()).has_value());
}

TEST_CASE("An index naming a vertex that does not exist is refused")
{
    MeshData mesh   = MakeTwoLodMesh();
    mesh.Indices[4] = static_cast<std::uint32_t>(mesh.Vertices.size());

    const auto result = ValidateMesh(mesh);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == MeshValidationError::IndexOutOfRange);
}

TEST_CASE("A submesh running past the index array is refused")
{
    MeshData mesh                = MakeTwoLodMesh();
    mesh.SubMeshes[1].IndexCount = 4; // 3 + 4 > 6

    const auto result = ValidateMesh(mesh);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == MeshValidationError::SubMeshOutOfRange);
}

TEST_CASE("A submesh range whose end overflows a uint32 is refused")
{
    // Summing in uint32 would wrap this to a small number that passes the bound.
    MeshData mesh                 = MakeTwoLodMesh();
    mesh.SubMeshes[0].IndexOffset = 0xFFFFFFFFu;
    mesh.SubMeshes[0].IndexCount  = 8;

    const auto result = ValidateMesh(mesh);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == MeshValidationError::SubMeshOutOfRange);
}

TEST_CASE("A submesh naming a material slot the mesh lacks is refused")
{
    MeshData mesh                  = MakeTwoLodMesh();
    mesh.SubMeshes[0].MaterialSlot = 1; // only slot 0 exists

    const auto result = ValidateMesh(mesh);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == MeshValidationError::MaterialSlotOutOfRange);
}

TEST_CASE("An empty submesh is refused")
{
    MeshData mesh                = MakeTwoLodMesh();
    mesh.SubMeshes[1].IndexCount = 0;

    const auto result = ValidateMesh(mesh);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == MeshValidationError::EmptySubMesh);
}

TEST_CASE("A LOD chain with a gap is refused")
{
    // Submesh 1 would be drawn by no level at all.
    MeshData mesh                = MakeTwoLodMesh();
    mesh.Lods[1].FirstSubMesh    = 2;
    mesh.Lods[1].SubMeshCount    = 0;

    const auto result = ValidateMesh(mesh);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == MeshValidationError::EmptyLod);
}

TEST_CASE("LODs that do not tile the submesh array in order are refused")
{
    MeshData mesh             = MakeTwoLodMesh();
    mesh.Lods[1].FirstSubMesh = 0; // overlaps LOD 0 rather than following it

    const auto result = ValidateMesh(mesh);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == MeshValidationError::LodsNotContiguous);
}

TEST_CASE("A LOD chain leaving trailing submeshes unaddressed is refused")
{
    MeshData mesh = MakeTwoLodMesh();
    mesh.Lods.pop_back(); // submesh 1 is now addressed by nothing

    const auto result = ValidateMesh(mesh);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == MeshValidationError::LodsNotContiguous);
}

TEST_CASE("An authored threshold that does not fall is refused")
{
    // Selection takes the first level the instance is big enough for, so an
    // equal or rising threshold makes the later level unreachable forever.
    MeshData mesh                        = MakeTwoLodMesh();
    mesh.Lods[1].ScreenSizeThreshold     = 0.75f;

    const auto result = ValidateMesh(mesh);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == MeshValidationError::LodThresholdNotDescending);
}

TEST_CASE("Two equal authored thresholds are refused")
{
    MeshData mesh                    = MakeTwoLodMesh();
    mesh.Lods[1].ScreenSizeThreshold = mesh.Lods[0].ScreenSizeThreshold;

    const auto result = ValidateMesh(mesh);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == MeshValidationError::LodThresholdNotDescending);
}

TEST_CASE("Unauthored thresholds are not compared")
{
    // Zero means the asset carried none, and DefaultLodScreenSize supplies a
    // ladder that descends by construction. Treating zero as a real threshold
    // would refuse every mesh whose author never set one.
    MeshData mesh                        = MakeTwoLodMesh();
    mesh.Lods[0].ScreenSizeThreshold     = 0.f;
    mesh.Lods[1].ScreenSizeThreshold     = 0.f;

    CHECK(ValidateMesh(mesh).has_value());
}

TEST_CASE("An index count that is not whole triangles is refused")
{
    MeshData mesh = MakeTwoLodMesh();
    mesh.Indices.pop_back();
    mesh.SubMeshes[1].IndexCount = 2;

    const auto result = ValidateMesh(mesh);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == MeshValidationError::IndexCountNotTriangles);
}

TEST_CASE("A mesh with no geometry is refused")
{
    const auto result = ValidateMesh(MeshData{});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == MeshValidationError::NoGeometry);
}

TEST_CASE("Geometry with no submesh table is refused rather than assumed")
{
    // The degenerate whole-mesh form is EnsureSubMeshTables' job. Accepting it
    // here would encode that rule a second time, in the one place that exists to
    // catch tables that do not agree.
    MeshData mesh = MakeTwoLodMesh();
    mesh.SubMeshes.clear();
    mesh.Lods.clear();

    const auto result = ValidateMesh(mesh);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == MeshValidationError::NoLods);
}

TEST_CASE("Every error has a description")
{
    // These reach the build log as the only thing said about a refused asset.
    CHECK(ToString(MeshValidationError::NoGeometry) != "is invalid");
    CHECK(ToString(MeshValidationError::IndexOutOfRange) != "is invalid");
    CHECK(ToString(MeshValidationError::LodThresholdNotDescending) != "is invalid");
    CHECK(ToString(MeshValidationError::MaterialSlotOutOfRange) != "is invalid");
}
