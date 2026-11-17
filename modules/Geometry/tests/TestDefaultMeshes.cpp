/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <Assisi/Geometry/DefaultMeshes.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Math/GLM.hpp>

using Assisi::Geometry::CreateErrorSignMesh;
using Assisi::Geometry::CreateIcosphereMesh;
using Assisi::Geometry::CreateUnitCubeMesh;
using Assisi::Geometry::CreateUnitCylinderMesh;
using Assisi::Geometry::CreateUnitQuadMesh;
using Assisi::Geometry::CreateUnitSphereMesh;
using Assisi::Geometry::MeshData;
using Assisi::Geometry::Vertex;

// A texture maps onto a face without mirroring iff the world-space tangent
// (d pos / d u) and bitangent (d pos / d v) form a frame whose cross product
// opposes the outward normal — i.e. dot(cross(T, B), normal) < 0 — under the
// top-left UV origin the engine samples with. (T/B derived via Lengyel's method,
// same as ComputeTangents.) A flipped or transposed face makes this positive.
// Asserted per face so a regression can't silently bring back a mirrored or
// sideways face.
TEST_CASE("CreateUnitCubeMesh: every face maps its texture upright, not mirrored")
{
    const MeshData mesh = CreateUnitCubeMesh();
    REQUIRE(mesh.Vertices.size() == 24); // 6 faces x 4 verts

    for (std::size_t face = 0; face < 6; ++face)
    {
        CAPTURE(face);

        const Vertex &v0 = mesh.Vertices[face * 4 + 0];
        const Vertex &v1 = mesh.Vertices[face * 4 + 1];
        const Vertex &v2 = mesh.Vertices[face * 4 + 2];

        const glm::vec3 e1   = v1.Position - v0.Position;
        const glm::vec3 e2   = v2.Position - v0.Position;
        const glm::vec2 duv1 = v1.TextureCoordinates - v0.TextureCoordinates;
        const glm::vec2 duv2 = v2.TextureCoordinates - v0.TextureCoordinates;

        const float det = duv1.x * duv2.y - duv2.x * duv1.y;
        REQUIRE(det != 0.0f); // degenerate UVs would leave the face untexturable

        const glm::vec3 tangent   = (duv2.y * e1 - duv1.y * e2) / det;
        const glm::vec3 bitangent = (duv1.x * e2 - duv2.x * e1) / det;

        const float handedness = glm::dot(glm::cross(tangent, bitangent), v0.Normal);
        CHECK(handedness < 0.0f); // >= 0 means the face is mirrored/flipped
    }
}

namespace
{
// Every vertex of a mesh that is going to be shaded needs a usable tangent
// frame: finite, unit length, and perpendicular to its own normal. The sphere
// and cylinder were silhouette-only meshes until they became `prim://`
// primitives, so this is the property that had to start holding.
void CheckTangentFrame(const MeshData &mesh)
{
    REQUIRE(!mesh.Vertices.empty());

    for (std::size_t i = 0; i < mesh.Vertices.size(); ++i)
    {
        CAPTURE(i);
        const Vertex &vertex = mesh.Vertices[i];

        REQUIRE(std::isfinite(vertex.Tangent.x));
        REQUIRE(std::isfinite(vertex.Tangent.y));
        REQUIRE(std::isfinite(vertex.Tangent.z));

        const glm::vec3 tangent{vertex.Tangent};
        CHECK(glm::length(tangent) == doctest::Approx(1.0f).epsilon(0.001f));
        CHECK(std::abs(glm::dot(tangent, vertex.Normal)) < 0.001f);
        CHECK(std::abs(vertex.Tangent.w) == doctest::Approx(1.0f));
    }
}
} // namespace

// A UV sphere's poles are the degenerate case: every quad there collapses to
// zero area in UV space, so the accumulated tangent is exactly zero and
// normalising it would put NaN in the vertex buffer. Checked at the coarsest
// tessellation too, where the poles are the largest share of the mesh.
namespace
{
// Every primitive here is convex and centred on the origin, so a triangle faces
// outward exactly when its counter-clockwise winding normal agrees with the
// direction from the origin to its centroid. MeshPass rasterises with
// frontCounterClockwise = true and back-face culling, so a triangle that fails
// this is culled when it should be drawn — the surface renders inside-out, and
// you see the far wall of the object through the near one.
//
// This went unchecked for as long as these meshes were only ever drawn as
// collider silhouettes, where winding is irrelevant. Promoting them to prim://
// scene geometry is what made it matter.
void CheckOutwardWinding(const MeshData &mesh, std::size_t expectedDegenerate = 0)
{
    REQUIRE(mesh.Indices.size() % 3 == 0);
    REQUIRE(!mesh.Indices.empty());

    std::size_t degenerate = 0;
    for (std::size_t i = 0; i < mesh.Indices.size(); i += 3)
    {
        CAPTURE(i);
        const Vertex &v0 = mesh.Vertices[mesh.Indices[i]];
        const Vertex &v1 = mesh.Vertices[mesh.Indices[i + 1]];
        const Vertex &v2 = mesh.Vertices[mesh.Indices[i + 2]];

        const glm::vec3 faceNormal = glm::cross(v1.Position - v0.Position, v2.Position - v0.Position);
        const glm::vec3 centroid   = (v0.Position + v1.Position + v2.Position) / 3.0f;

        // A UV sphere's pole quads collapse to zero area — two of their three
        // corners are the same point. They have no orientation to check, and the
        // rasteriser discards them. Counted rather than ignored, so a change
        // that degenerates a mesh wholesale cannot hide behind this skip.
        if (glm::length(faceNormal) < 1e-6f)
        {
            ++degenerate;
            continue;
        }

        CHECK(glm::dot(faceNormal, centroid) > 0.0f);

        // ...and the shading normals must agree with the winding, or lighting
        // contradicts the silhouette even where the triangle survives culling.
        CHECK(glm::dot(faceNormal, v0.Normal) > 0.0f);
    }
    CHECK(degenerate == expectedDegenerate);
}
} // namespace

TEST_CASE("Primitives wind counter-clockwise when seen from outside")
{
    CheckOutwardWinding(CreateUnitCubeMesh());

    // A UV sphere's pole rows are one degenerate triangle per slice at each end.
    CheckOutwardWinding(CreateUnitSphereMesh(48, 24), 2 * 48);
    CheckOutwardWinding(CreateUnitSphereMesh(5, 3), 2 * 5);

    CheckOutwardWinding(CreateUnitCylinderMesh());
    CheckOutwardWinding(CreateUnitCylinderMesh(5));

    // The icosphere has no poles, so none of its triangles collapse.
    CheckOutwardWinding(CreateIcosphereMesh(0));
    CheckOutwardWinding(CreateIcosphereMesh(2));
}

namespace
{

/// The rows of prim://error's letters, and the vertices of one of its boxes.
constexpr uint32_t kSignRows = 5;
constexpr std::size_t kBoxVertices = 24;

/// The height of the middle of row @p row of the sign, the bottom row 0 (m).
float RowCentre(uint32_t row)
{
    return (static_cast<float>(row) + 0.5f) * Assisi::Geometry::kErrorSignCell;
}

/// Whether one of @p sign's boxes is centred at @p x, @p y.
bool HasCellAt(const MeshData &sign, float x, float y)
{
    const float tolerance = 0.25f * Assisi::Geometry::kErrorSignCell;
    for (std::size_t first = 0; first + kBoxVertices <= sign.Vertices.size(); first += kBoxVertices)
    {
        glm::vec3 centre{0.f};
        for (std::size_t vertex = first; vertex < first + kBoxVertices; ++vertex)
        {
            centre += sign.Vertices[vertex].Position;
        }
        centre /= static_cast<float>(kBoxVertices);
        if (std::abs(centre.x - x) < tolerance && std::abs(centre.y - y) < tolerance)
        {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("CreateErrorSignMesh: every triangle winds counter-clockwise toward its own normal")
{
    // Its boxes sit away from the origin, so outward is each face's normal
    // rather than the direction from the centre.
    const MeshData mesh = CreateErrorSignMesh();
    for (std::size_t i = 0; i + 2 < mesh.Indices.size(); i += 3)
    {
        const Vertex &v0 = mesh.Vertices[mesh.Indices[i]];
        const Vertex &v1 = mesh.Vertices[mesh.Indices[i + 1]];
        const Vertex &v2 = mesh.Vertices[mesh.Indices[i + 2]];
        const glm::vec3 faceNormal = glm::cross(v1.Position - v0.Position, v2.Position - v0.Position);
        CHECK(glm::dot(faceNormal, v0.Normal) > 0.0f);
    }
}

TEST_CASE("CreateErrorSignMesh: ERROR in block letters, standing on the ground and facing +Z")
{
    const MeshData mesh = CreateErrorSignMesh();

    // E, R, R, O, R on a 3-by-5 grid: 11 + 10 + 10 + 12 + 10 cells, a box of
    // 12 triangles each.
    constexpr std::size_t kCells = 53;
    CHECK(mesh.Indices.size() == kCells * 12 * 3);

    glm::vec3 low{std::numeric_limits<float>::max()};
    glm::vec3 high{std::numeric_limits<float>::lowest()};
    for (const Vertex &vertex : mesh.Vertices)
    {
        low = glm::min(low, vertex.Position);
        high = glm::max(high, vertex.Position);
    }
    // Five letters three cells wide with a cell between each, five cells tall
    // and one deep.
    const float cell = Assisi::Geometry::kErrorSignCell;
    CHECK(high.x - low.x == doctest::Approx(19.f * cell));
    CHECK(high.y - low.y == doctest::Approx(5.f * cell));
    CHECK(high.z - low.z == doctest::Approx(cell));
    CHECK(low.y == doctest::Approx(0.f));
    CHECK(low.x == doctest::Approx(-high.x));
    CHECK(low.z == doctest::Approx(-high.z));

    // Read from +Z, left is -x: the E's spine fills its first column from the
    // ground to the top, while the last R's leg leaves the top right cell empty.
    // Read from behind, the two would swap.
    const float firstColumn = low.x + 0.5f * cell;
    const float lastColumn = high.x - 0.5f * cell;
    for (uint32_t row = 0; row < kSignRows; ++row)
    {
        CAPTURE(row);
        CHECK(HasCellAt(mesh, firstColumn, RowCentre(row)));
    }
    CHECK_FALSE(HasCellAt(mesh, lastColumn, RowCentre(kSignRows - 1)));
    CHECK(HasCellAt(mesh, lastColumn, RowCentre(kSignRows - 2)));
}

TEST_CASE("CreateUnitQuadMesh: one face toward +Z, its texture upright and unmirrored")
{
    const MeshData mesh = CreateUnitQuadMesh();
    REQUIRE(mesh.Vertices.size() == 4);
    REQUIRE(mesh.Indices.size() == 6);

    for (const Vertex &vertex : mesh.Vertices)
    {
        CHECK(vertex.Position.z == 0.0f);
        CHECK(std::abs(vertex.Position.x) == doctest::Approx(0.5f));
        CHECK(std::abs(vertex.Position.y) == doctest::Approx(0.5f));
        CHECK(vertex.Normal == glm::vec3(0.0f, 0.0f, 1.0f));
    }

    for (std::size_t i = 0; i < mesh.Indices.size(); i += 3)
    {
        CAPTURE(i);
        const Vertex &v0 = mesh.Vertices[mesh.Indices[i]];
        const Vertex &v1 = mesh.Vertices[mesh.Indices[i + 1]];
        const Vertex &v2 = mesh.Vertices[mesh.Indices[i + 2]];
        // Counter-clockwise seen from +Z, which MeshPass draws as the front.
        CHECK(glm::cross(v1.Position - v0.Position, v2.Position - v0.Position).z > 0.0f);

        // The cube's test for an unmirrored face, on each triangle.
        const glm::vec3 e1   = v1.Position - v0.Position;
        const glm::vec3 e2   = v2.Position - v0.Position;
        const glm::vec2 duv1 = v1.TextureCoordinates - v0.TextureCoordinates;
        const glm::vec2 duv2 = v2.TextureCoordinates - v0.TextureCoordinates;
        const float det = duv1.x * duv2.y - duv2.x * duv1.y;
        REQUIRE(det != 0.0f);
        const glm::vec3 tangent   = (duv2.y * e1 - duv1.y * e2) / det;
        const glm::vec3 bitangent = (duv1.x * e2 - duv2.x * e1) / det;
        CHECK(glm::dot(glm::cross(tangent, bitangent), v0.Normal) < 0.0f);
        // Texture-up along +Y: v runs from the top edge down.
        CHECK(bitangent.y < 0.0f);
    }

    CheckTangentFrame(mesh);
}

TEST_CASE("CreateUnitSphereMesh: every vertex carries a usable tangent frame")
{
    CheckTangentFrame(CreateUnitSphereMesh());
    CheckTangentFrame(CreateUnitSphereMesh(3, 2));
}

TEST_CASE("CreateUnitCylinderMesh: every vertex carries a usable tangent frame")
{
    CheckTangentFrame(CreateUnitCylinderMesh());
    CheckTangentFrame(CreateUnitCylinderMesh(3));
}

TEST_CASE("CreateIcosphereMesh: every vertex carries a usable tangent frame")
{
    CheckTangentFrame(CreateIcosphereMesh(0));
    CheckTangentFrame(CreateIcosphereMesh(3));
}

// Subdividing splits each triangle in four, and the shared edge midpoints must
// be welded rather than duplicated — an unwelded icosphere still looks right but
// carries four times the vertices it needs and splits its normals along every
// edge. Euler's formula pins the welded vertex count exactly.
TEST_CASE("CreateIcosphereMesh: subdivision welds shared edge midpoints")
{
    for (uint32_t subdivisions = 0; subdivisions <= 3; ++subdivisions)
    {
        CAPTURE(subdivisions);
        const MeshData mesh = CreateIcosphereMesh(subdivisions);

        const std::size_t faces = static_cast<std::size_t>(20) << (2 * subdivisions);
        CHECK(mesh.Indices.size() / 3 == faces);
        // V = F/2 + 2 for a closed triangulated surface.
        CHECK(mesh.Vertices.size() == faces / 2 + 2);

        for (const Vertex &vertex : mesh.Vertices)
        {
            CHECK(glm::length(vertex.Position) == doctest::Approx(1.0f).epsilon(0.001f));
        }
    }
}

// The perf reference scenes publish exact triangle counts as part of their
// contract, and those numbers only mean anything if the ladder the scenes are
// built from keeps the tessellation they were computed against. Changing a rung
// is allowed; changing it without moving the published numbers is the bug this
// catches.
TEST_CASE("The prim:// tessellation ladder matches the counts the perf scenes publish")
{
    namespace Tessellation = Assisi::Geometry::PrimitiveTessellation;

    const auto sphereTriangles = [](uint32_t slices, uint32_t stacks)
                                 { return CreateUnitSphereMesh(slices, stacks).Indices.size() / 3; };
    const auto cylinderTriangles = [](uint32_t slices)
                                   { return CreateUnitCylinderMesh(slices).Indices.size() / 3; };
    const auto icosphereTriangles = [](uint32_t subdivisions)
                                    { return CreateIcosphereMesh(subdivisions).Indices.size() / 3; };

    CHECK(CreateUnitCubeMesh().Indices.size() / 3 == 12);

    CHECK(sphereTriangles(Tessellation::kSphereLowSlices, Tessellation::kSphereLowStacks) == 144);
    CHECK(sphereTriangles(Tessellation::kSphereSlices, Tessellation::kSphereStacks) == 576);
    CHECK(sphereTriangles(Tessellation::kSphereHighSlices, Tessellation::kSphereHighStacks) == 4096);

    CHECK(icosphereTriangles(Tessellation::kIcosphereLowSubdivisions) == 320);
    CHECK(icosphereTriangles(Tessellation::kIcosphereSubdivisions) == 1280);
    CHECK(icosphereTriangles(Tessellation::kIcosphereHighSubdivisions) == 20480);

    CHECK(cylinderTriangles(Tessellation::kCylinderSlices) == 96);
    CHECK(cylinderTriangles(Tessellation::kCylinderHighSlices) == 256);
}
