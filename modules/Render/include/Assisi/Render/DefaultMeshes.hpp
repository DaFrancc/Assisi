#pragma once

/// @file DefaultMeshes.hpp
/// @brief Factory functions for built-in primitive meshes.

#include <Assisi/Render/MeshData.hpp>

#include <cmath>
#include <vector>

namespace Assisi::Render
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
        const unsigned int i0 = mesh.Indices[i];
        const unsigned int i1 = mesh.Indices[i + 1];
        const unsigned int i2 = mesh.Indices[i + 2];

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

    /* 24 vertices: 4 per face so normals and UVs are correct per-face. */
    mesh.Vertices = {
        /* +X face */
        {{0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
        {{0.5f, 0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
        {{0.5f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
        {{0.5f, -0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}},

        /* -X face */
        {{-0.5f, -0.5f, 0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
        {{-0.5f, 0.5f, 0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
        {{-0.5f, 0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
        {{-0.5f, -0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}},

        /* +Y face */
        {{-0.5f, 0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
        {{-0.5f, 0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
        {{0.5f, 0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}},
        {{0.5f, 0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}},

        /* -Y face */
        {{-0.5f, -0.5f, 0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}},
        {{-0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}},
        {{0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},
        {{0.5f, -0.5f, 0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},

        /* +Z face */
        {{-0.5f, -0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
        {{0.5f, -0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        {{0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
        {{-0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},

        /* -Z face */
        {{0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}},
        {{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}},
        {{-0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}},
        {{0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}},
    };

    /* 36 indices (6 faces × 2 triangles × 3 indices). */
    mesh.Indices = {0,  1,  2,  2,  3,  0,  4,  5,  6,  6,  7,  4,  8,  9,  10, 10, 11, 8,
                    12, 13, 14, 14, 15, 12, 16, 17, 18, 18, 19, 16, 20, 21, 22, 22, 23, 20};

    ComputeTangents(mesh);

    return mesh;
}
} /* namespace Assisi::Render */
