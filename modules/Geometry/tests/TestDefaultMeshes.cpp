/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>

#include <Assisi/Geometry/DefaultMeshes.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Math/GLM.hpp>

using Assisi::Geometry::CreateIcosphereMesh;
using Assisi::Geometry::CreateUnitCubeMesh;
using Assisi::Geometry::CreateUnitCylinderMesh;
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
