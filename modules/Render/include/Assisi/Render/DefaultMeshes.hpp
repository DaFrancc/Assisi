#pragma once

#ifndef DEFAULTMESHES_HPP
#define DEFAULTMESHES_HPP

#include <Assisi/Render/MeshData.hpp>

namespace Assisi::Render
{
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

    return mesh;
}
} /* namespace Assisi::Render */

#endif /* DEFAULTMESHES_HPP */