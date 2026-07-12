/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Frustum.hpp
/// @brief View-frustum planes for coarse draw culling.

#include <algorithm>
#include <array>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Bounds.hpp>

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
    [[nodiscard]] bool IntersectsSphere(const BoundingSphere &sphere) const
    {
        // Outside iff the sphere is fully behind any one plane (its centre farther
        // than the radius on the plane's outward side); otherwise potentially visible.
        return std::ranges::none_of(_planes, [&sphere](const glm::vec4 &plane) {
            return glm::dot(glm::vec3(plane), sphere.center) + plane.w < -sphere.radius;
        });
    }

  private:
    std::array<glm::vec4, 6> _planes{};
};

} /* namespace Assisi::Render */
