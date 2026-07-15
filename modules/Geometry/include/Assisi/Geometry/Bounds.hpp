/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Bounds.hpp
/// @brief Coarse bounding-volume types (sphere + AABB) for visibility tests.
///
/// Lives in Geometry (not Render) because bounds are pure CPU math: the
/// importer computes per-submesh bounds at import time, and physics/tools/
/// tests can use them without linking the renderer. Render consumes these
/// types for frustum culling (see Render/Frustum.hpp). The functions that fit
/// bounds around mesh geometry live beside MeshData in MeshData.hpp — this
/// header deliberately has no MeshData dependency so MeshData can embed these
/// types per submesh.

#include <Assisi/Math/GLM.hpp>

namespace Assisi::Geometry
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

/// @brief An axis-aligned bounding box in the mesh's local space.
///
/// Tighter than the sphere for flat or elongated geometry (the sphere's
/// isotropic radius swallows a large floor). Kept beside the sphere so culling
/// can use the cheap sphere reject first and the box for refinement — and it is
/// the screen-space depth bound the future HZB occlusion test projects.
struct Aabb
{
    glm::vec3 min{0.f, 0.f, 0.f};
    glm::vec3 max{0.f, 0.f, 0.f}; ///< min == max == origin means "no geometry".
};

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

/// @brief Maps a local-space AABB through a world matrix, re-fitting a new
///        axis-aligned box around the transformed one (Arvo's method).
///
/// The world box is built from the transformed centre plus an extent formed by
/// the absolute value of the matrix's linear part times the local extent — the
/// closed form of "transform the eight corners and take their min/max", without
/// materialising the corners. Exact under translation/scale/axis-aligned
/// rotation; under an off-axis rotation the re-fit box is a conservative
/// over-estimate (still never smaller than the geometry), which is all a cull
/// needs. Rows/cols follow GLM's column-major layout (`worldMatrix[col]`).
inline Aabb TransformedAabb(const Aabb &local, const glm::mat4 &worldMatrix)
{
    const glm::vec3 center = 0.5f * (local.min + local.max);
    const glm::vec3 extent = 0.5f * (local.max - local.min);

    const glm::vec3 worldCenter = glm::vec3(worldMatrix * glm::vec4(center, 1.f));
    // Columns are the absolute basis vectors; abs3x3 * extent sums |basis_j| *
    // extent_j into each world axis — the half-size of the re-fit box.
    const glm::mat3 absBasis(glm::abs(glm::vec3(worldMatrix[0])), glm::abs(glm::vec3(worldMatrix[1])),
                             glm::abs(glm::vec3(worldMatrix[2])));
    const glm::vec3 worldExtent = absBasis * extent;

    return Aabb{.min = worldCenter - worldExtent, .max = worldCenter + worldExtent};
}

} /* namespace Assisi::Geometry */
