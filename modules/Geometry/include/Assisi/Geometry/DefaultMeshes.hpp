/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file DefaultMeshes.hpp
/// @brief Factory functions for built-in primitive meshes, plus tangent generation.

#include <Assisi/Geometry/MeshData.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

namespace Assisi::Geometry
{

/// @brief Computes and fills the Tangent field of every vertex in @p mesh.
///
/// Uses Lengyel's method: derives tangent/bitangent from edge positions and UV
/// deltas, accumulates per triangle, then orthogonalises each vertex tangent
/// against its normal (Gram-Schmidt) and stores the bitangent handedness in w.
///
/// @pre @p mesh must have valid Positions, Normals, TextureCoordinates, and
///      a triangle-list index buffer (Indices.size() % 3 == 0).
inline void ComputeTangents(MeshData &mesh)
{
    const size_t vertCount = mesh.Vertices.size();
    std::vector<glm::vec3> tan1(vertCount, glm::vec3(0.0f));
    std::vector<glm::vec3> tan2(vertCount, glm::vec3(0.0f));

    for (size_t i = 0; i < mesh.Indices.size(); i += 3)
    {
        const uint32_t i0 = mesh.Indices[i];
        const uint32_t i1 = mesh.Indices[i + 1];
        const uint32_t i2 = mesh.Indices[i + 2];

        const glm::vec3 &p0 = mesh.Vertices[i0].Position;
        const glm::vec3 &p1 = mesh.Vertices[i1].Position;
        const glm::vec3 &p2 = mesh.Vertices[i2].Position;

        const glm::vec2 &uv0 = mesh.Vertices[i0].TextureCoordinates;
        const glm::vec2 &uv1 = mesh.Vertices[i1].TextureCoordinates;
        const glm::vec2 &uv2 = mesh.Vertices[i2].TextureCoordinates;

        const glm::vec3 e1  = p1 - p0;
        const glm::vec3 e2  = p2 - p0;
        const float     du1 = uv1.x - uv0.x;
        const float     dv1 = uv1.y - uv0.y;
        const float     du2 = uv2.x - uv0.x;
        const float     dv2 = uv2.y - uv0.y;

        const float det = du1 * dv2 - du2 * dv1;
        if (!std::isfinite(det) || det == 0.0f)
        {
            continue;
        }

        const float     r    = 1.0f / det;
        const glm::vec3 sdir = (dv2 * e1 - dv1 * e2) * r;
        const glm::vec3 tdir = (du1 * e2 - du2 * e1) * r;

        tan1[i0] += sdir;
        tan1[i1] += sdir;
        tan1[i2] += sdir;
        tan2[i0] += tdir;
        tan2[i1] += tdir;
        tan2[i2] += tdir;
    }

    for (size_t i = 0; i < vertCount; ++i)
    {
        const glm::vec3 &n = mesh.Vertices[i].Normal;
        const glm::vec3 &t = tan1[i];

        // Gram-Schmidt orthogonalise
        const glm::vec3 tangent = glm::normalize(t - glm::dot(t, n) * n);
        // Bitangent handedness: +1 if right-handed, -1 if mirrored
        const float w = (glm::dot(glm::cross(n, t), tan2[i]) < 0.0f) ? -1.0f : 1.0f;

        mesh.Vertices[i].Tangent = glm::vec4(tangent, w);
    }
}


/// @brief Returns a unit cube mesh centered at the origin.
///
/// Uses 24 vertices (4 per face) so each face has its own normals and UVs.
/// Produces 36 indices (6 faces × 2 triangles × 3 indices).
inline MeshData CreateUnitCubeMesh()
{
    MeshData mesh;

    /* 24 vertices: 4 per face so normals and UVs are correct per-face. UVs use a
     * top-left origin (uv (0,0) = top-left texel, matching stb_image / Vulkan and
     * the ImGui path), and every face is oriented so the texture reads upright and
     * un-mirrored when viewed from outside: side faces put texture-up along world
     * +Y; the top (+Y) and bottom (-Y) faces put texture-up along world -Z. Each
     * face's u x v points along -normal, i.e. no face is mirrored. Positions and
     * the index buffer below are unchanged, so triangle winding / back-face culling
     * are unaffected — only the UV assignment differs. */
    mesh.Vertices = {
        /* +X face (up = +Y) */
        {{0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
        {{0.5f, 0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
        {{0.5f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
        {{0.5f, -0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}},

        /* -X face (up = +Y) */
        {{-0.5f, -0.5f, 0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
        {{-0.5f, 0.5f, 0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
        {{-0.5f, 0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
        {{-0.5f, -0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}},

        /* +Y face (up = -Z) */
        {{-0.5f, 0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
        {{-0.5f, 0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}},
        {{0.5f, 0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}},
        {{0.5f, 0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},

        /* -Y face (up = -Z) */
        {{-0.5f, -0.5f, 0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}},
        {{-0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},
        {{0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},
        {{0.5f, -0.5f, 0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}},

        /* +Z face (up = +Y) */
        {{-0.5f, -0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
        {{0.5f, -0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
        {{0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        {{-0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},

        /* -Z face (up = +Y) */
        {{0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}},
        {{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}},
        {{-0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}},
        {{0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}},
    };

    /* 36 indices (6 faces × 2 triangles × 3 indices). */
    mesh.Indices = {0,  1,  2,  2,  3,  0,  4,  5,  6,  6,  7,  4,  8,  9,  10, 10, 11, 8,
                    12, 13, 14, 14, 15, 12, 16, 17, 18, 18, 19, 16, 20, 21, 22, 22, 23, 20};

    ComputeTangents(mesh);

    return mesh;
}

/// @brief Returns a solid unit sphere (radius 1, centred at the origin) as a
/// UV-sphere with @p slices longitudinal and @p stacks latitudinal divisions.
///
/// Watertight, so its silhouette is a filled disc from any angle — the editor
/// scales it by a collider's radius to draw a clean round outline. Positions and
/// (radial) normals are set; UVs are the sphere parameterisation. The default
/// tessellation is high because a coarse sphere reads as a faceted polygon in a
/// silhouette outline rather than a circle.
inline MeshData CreateUnitSphereMesh(uint32_t slices = 48, uint32_t stacks = 24)
{
    constexpr float kPi = 3.14159265358979323846f;
    MeshData        mesh;

    for (uint32_t i = 0; i <= stacks; ++i)
    {
        const float v   = static_cast<float>(i) / static_cast<float>(stacks);
        const float phi = v * kPi; // 0 at +Y pole, π at −Y pole
        const float y   = std::cos(phi);
        const float ring = std::sin(phi);
        for (uint32_t j = 0; j <= slices; ++j)
        {
            const float     u     = static_cast<float>(j) / static_cast<float>(slices);
            const float     theta = u * 2.0f * kPi;
            const glm::vec3 p(ring * std::cos(theta), y, ring * std::sin(theta));
            mesh.Vertices.push_back({p, p, {u, v}});
        }
    }

    const uint32_t stride = slices + 1;
    for (uint32_t i = 0; i < stacks; ++i)
    {
        for (uint32_t j = 0; j < slices; ++j)
        {
            const uint32_t a = i * stride + j;
            const uint32_t b = a + stride;
            mesh.Indices.insert(mesh.Indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    }
    return mesh;
}

/// @brief Returns a solid unit cylinder (radius 1, spanning y ∈ [−1, 1]) with
/// @p slices around its axis, capped at both ends so it is watertight.
///
/// The editor scales it by (radius, halfHeight, radius) for a cylinder collider,
/// and reuses it (with two unit spheres at the ends) to build a capsule's
/// silhouette. Only positions matter for the silhouette; normals/UVs are set
/// plausibly.
inline MeshData CreateUnitCylinderMesh(uint32_t slices = 48)
{
    constexpr float kPi = 3.14159265358979323846f;
    MeshData        mesh;

    // Side rim vertices, top (y=+1) then bottom (y=−1), with radial normals.
    for (uint32_t j = 0; j <= slices; ++j)
    {
        const float u     = static_cast<float>(j) / static_cast<float>(slices);
        const float theta = u * 2.0f * kPi;
        const float x     = std::cos(theta);
        const float z     = std::sin(theta);
        mesh.Vertices.push_back({{x, 1.0f, z}, {x, 0.0f, z}, {u, 0.0f}});
        mesh.Vertices.push_back({{x, -1.0f, z}, {x, 0.0f, z}, {u, 1.0f}});
    }
    for (uint32_t j = 0; j < slices; ++j)
    {
        const uint32_t top = j * 2;
        const uint32_t bot = top + 1;
        mesh.Indices.insert(mesh.Indices.end(), {top, bot, top + 2, top + 2, bot, bot + 2});
    }

    // Cap fans: a centre vertex for each end, fanned over its own rim ring.
    const auto addCap = [&mesh, slices, kPi](float y, float ny) {
        const uint32_t center = static_cast<uint32_t>(mesh.Vertices.size());
        mesh.Vertices.push_back({{0.0f, y, 0.0f}, {0.0f, ny, 0.0f}, {0.5f, 0.5f}});
        const uint32_t first = static_cast<uint32_t>(mesh.Vertices.size());
        for (uint32_t j = 0; j <= slices; ++j)
        {
            const float theta = static_cast<float>(j) / static_cast<float>(slices) * 2.0f * kPi;
            const float x     = std::cos(theta);
            const float z     = std::sin(theta);
            mesh.Vertices.push_back({{x, y, z}, {0.0f, ny, 0.0f}, {0.5f + 0.5f * x, 0.5f + 0.5f * z}});
        }
        for (uint32_t j = 0; j < slices; ++j)
        {
            mesh.Indices.insert(mesh.Indices.end(), {center, first + j, first + j + 1});
        }
    };
    addCap(1.0f, 1.0f);
    addCap(-1.0f, -1.0f);

    return mesh;
}
} /* namespace Assisi::Geometry */
