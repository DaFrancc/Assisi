/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <cstddef>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/DefaultMeshes.hpp>
#include <Assisi/Render/MeshData.hpp>

using Assisi::Render::CreateUnitCubeMesh;
using Assisi::Render::MeshData;
using Assisi::Render::Vertex;

// A texture maps onto a face without mirroring iff the world-space tangent
// (d pos / d u) and bitangent (d pos / d v) form a frame whose cross product
// opposes the outward normal — i.e. dot(cross(T, B), normal) < 0 — under the
// top-left UV origin the engine samples with. (Derive T/B via Lengyel's method,
// same as ComputeTangents.) A flipped or transposed face would make this
// positive. This is the invariant CreateUnitCubeMesh's UVs were corrected to
// satisfy; assert it per face so a regression can't silently bring back a
// mirrored or sideways face.
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
