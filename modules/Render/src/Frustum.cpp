/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/Frustum.hpp>

namespace Assisi::Render
{
namespace
{
// Normalises a plane so its xyz is a unit normal and w is a true (metric)
// signed distance — required for the sphere test to compare w against a radius
// in world units.
glm::vec4 NormalizePlane(const glm::vec4 &plane)
{
    return plane / glm::length(glm::vec3(plane));
}
} // namespace

Frustum Frustum::FromViewProjection(const glm::mat4 &viewProjection)
{
    // Gribb–Hartmann: each plane is a sum/difference of the matrix rows, with the
    // normal pointing into the frustum. GLM is column-major, so row i is the i-th
    // element of each column: (m[0][i], m[1][i], m[2][i], m[3][i]).
    const glm::vec4 rowX{viewProjection[0][0], viewProjection[1][0], viewProjection[2][0], viewProjection[3][0]};
    const glm::vec4 rowY{viewProjection[0][1], viewProjection[1][1], viewProjection[2][1], viewProjection[3][1]};
    const glm::vec4 rowZ{viewProjection[0][2], viewProjection[1][2], viewProjection[2][2], viewProjection[3][2]};
    const glm::vec4 rowW{viewProjection[0][3], viewProjection[1][3], viewProjection[2][3], viewProjection[3][3]};

    Frustum frustum;
    frustum._planes[0] = NormalizePlane(rowW + rowX); // left
    frustum._planes[1] = NormalizePlane(rowW - rowX); // right
    frustum._planes[2] = NormalizePlane(rowW + rowY); // bottom
    frustum._planes[3] = NormalizePlane(rowW - rowY); // top
    // Near uses rowZ alone (not rowW + rowZ): with GLM_FORCE_DEPTH_ZERO_TO_ONE the
    // near-clip condition is clip.z >= 0 rather than OpenGL's clip.z >= -clip.w.
    frustum._planes[4] = NormalizePlane(rowZ);        // near
    frustum._planes[5] = NormalizePlane(rowW - rowZ); // far
    return frustum;
}

} // namespace Assisi::Render
