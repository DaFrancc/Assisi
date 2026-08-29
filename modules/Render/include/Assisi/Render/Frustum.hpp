/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Frustum.hpp
/// @brief View-frustum planes for coarse draw culling.

#include <algorithm>
#include <array>

#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::Render
{

/// @brief The six bounding planes of a view frustum, for sphere-vs-frustum culling.
///
/// Planes are extracted directly from a view-projection matrix (Gribb–Hartmann),
/// so no camera vectors or FOV are needed — whatever the matrix projects is what
/// the frustum bounds, including an off-centre or oblique projection. Each plane
/// is stored as `vec4(nx, ny, nz, d)` with the normal pointing *into* the frustum
/// and normalised, so `dot(normal, p) + d` is the signed distance from the plane
/// (positive inside).
class Frustum
{
public:
    Frustum() = default;

    /// @brief Extracts the six frustum planes from a view-projection matrix.
    /// Assumes GLM's configured clip space (Vulkan/D3D zero-to-one depth, see
    /// GLMConfig.hpp) for the near plane.
    static Frustum FromViewProjection(const glm::mat4 &viewProjection);

    /// @brief True if any part of the sphere lies inside (or on) the frustum.
    ///
    /// Conservative: a sphere straddling a corner may test "inside" against every
    /// plane yet fall in the corner void, so this can return true for a sphere
    /// just outside — the cost is a wasted draw, never a wrongly culled object.
    [[nodiscard]] bool IntersectsSphere(const Geometry::BoundingSphere &sphere) const
    {
        // Outside iff the sphere is fully behind any one plane (its centre farther
        // than the radius on the plane's outward side); otherwise potentially visible.
        return std::ranges::none_of(_planes, [&sphere](const glm::vec4 &plane) {
                return glm::dot(glm::vec3(plane), sphere.center) + plane.w < -sphere.radius;
            });
    }

    /// @brief True if any part of the (world-space) AABB lies inside the frustum.
    ///
    /// Tighter than the sphere for flat/elongated geometry. Same conservative
    /// caveat: a box straddling a frustum corner may pass every plane yet sit in
    /// the corner void — a wasted draw, never a wrongly culled object. Feed it a
    /// world-space box (Geometry::TransformedAabb).
    [[nodiscard]] bool IntersectsAabb(const Geometry::Aabb &aabb) const
    {
        // Outside iff the box is fully behind any one plane. Test only the box's
        // "positive vertex" — the corner furthest along the plane's inward normal:
        // if even that corner is behind the plane, every corner is.
        return std::ranges::none_of(_planes, [&aabb](const glm::vec4 &plane) {
                const glm::vec3 normal(plane);
                const glm::vec3 positiveVertex(normal.x >= 0.f ? aabb.max.x : aabb.min.x,
                                               normal.y >= 0.f ? aabb.max.y : aabb.min.y,
                                               normal.z >= 0.f ? aabb.max.z : aabb.min.z);
                return glm::dot(normal, positiveVertex) + plane.w < 0.f;
            });
    }

    /// @brief The six planes, `vec4(nx, ny, nz, d)` with inward normals (see the
    /// class comment). Uploaded to the GPU cull pass (mesh_cull.comp), which runs
    /// the same sphere/AABB tests as IntersectsSphere/IntersectsAabb.
    [[nodiscard]] const std::array<glm::vec4, 6> &Planes() const { return _planes; }

    /// @brief The same frustum with nothing in front of it — the near plane
    /// pushed out until it rejects nothing.
    ///
    /// What a shadow view needs, and only a shadow view. A camera's near plane
    /// removes geometry the viewer cannot see; a shadow view's would remove
    /// geometry between the light and everything it shades, which is the one
    /// thing that must not be removed. A caster nearer the light than the slice
    /// still darkens it, and the depth pass flattens such a caster onto the near
    /// plane rather than clipping it — so culling it here deletes a shadow the
    /// rasterizer was built to keep.
    [[nodiscard]] Frustum WithoutNearPlane() const
    {
        Frustum open = *this;
        // A zero normal makes the test `0 + d < -radius`, which for a positive d
        // is false for every sphere and box: the plane stops rejecting rather
        // than moving somewhere it could still reject from.
        open._planes[4] = glm::vec4(0.f, 0.f, 0.f, 1.f);
        return open;
    }

private:
    std::array<glm::vec4, 6> _planes{};
};

} /* namespace Assisi::Render */
