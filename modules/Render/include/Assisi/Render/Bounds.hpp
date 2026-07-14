/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Bounds.hpp
/// @brief Coarse bounding volumes for visibility tests (frustum culling).

#include <cmath>

#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::Render
{

/// @brief A bounding sphere, the cheapest volume to both build and test.
///
/// A sphere is the natural fit for coarse culling because it survives an
/// arbitrary affine transform with only a matrix-times-point and a single scalar
/// scale (see TransformedBoundingSphere) — no eight-corner AABB re-fit per frame.
/// It is looser than a tight box, but the cost of a rare false "visible" is one
/// wasted draw, never a wrongly culled (popped) object.
struct BoundingSphere
{
    glm::vec3 center{0.f, 0.f, 0.f};
    float     radius = 0.f; ///< 0 means "no geometry" (an empty mesh).
};

/// @brief Fits a bounding sphere around a mesh's vertices in its local space.
///
/// Centre is the AABB midpoint; radius is the exact farthest-vertex distance from
/// that centre, so the sphere is guaranteed to enclose every vertex (never
/// under-culls) while staying tighter than an AABB half-diagonal. Returns a
/// zero-radius sphere at the origin for an empty mesh.
inline BoundingSphere ComputeBoundingSphere(const Geometry::MeshData &meshData)
{
    if (meshData.Vertices.empty())
    {
        return {};
    }

    glm::vec3 min = meshData.Vertices.front().Position;
    glm::vec3 max = min;
    for (const Geometry::Vertex &vertex : meshData.Vertices)
    {
        min = glm::min(min, vertex.Position);
        max = glm::max(max, vertex.Position);
    }

    const glm::vec3 center = (min + max) * 0.5f;
    float           radiusSquared = 0.f;
    for (const Geometry::Vertex &vertex : meshData.Vertices)
    {
        const glm::vec3 offset = vertex.Position - center;
        radiusSquared = glm::max(radiusSquared, glm::dot(offset, offset));
    }

    return BoundingSphere{.center = center, .radius = std::sqrt(radiusSquared)};
}

/// @brief Maps a local-space bounding sphere through a world matrix.
///
/// The centre transforms as a point. The radius scales by the largest of the
/// matrix's three basis-vector lengths — an upper bound on how much any direction
/// can stretch, so the result conservatively encloses the transformed geometry
/// even under non-uniform scale (rotation preserves length and drops out).
inline BoundingSphere TransformedBoundingSphere(const BoundingSphere &local, const glm::mat4 &worldMatrix)
{
    const glm::vec3 center = glm::vec3(worldMatrix * glm::vec4(local.center, 1.f));
    const float     scaleX = glm::length(glm::vec3(worldMatrix[0]));
    const float     scaleY = glm::length(glm::vec3(worldMatrix[1]));
    const float     scaleZ = glm::length(glm::vec3(worldMatrix[2]));
    const float     maxScale = glm::max(scaleX, glm::max(scaleY, scaleZ));
    return BoundingSphere{.center = center, .radius = local.radius * maxScale};
}

} /* namespace Assisi::Render */
