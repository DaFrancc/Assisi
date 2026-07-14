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

} /* namespace Assisi::Geometry */
