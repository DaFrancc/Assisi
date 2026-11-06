/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Geometry/SmoothingCheck.hpp>

#include <Assisi/Math/GLM.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace Assisi::Geometry
{

namespace
{
/// Indices per triangle in MeshData's triangle list.
constexpr std::size_t kCorners = 3;

/// Steps along each edge of the barycentric grid a triangle's bulge is sampled
/// on. The bulge is a smooth quadratic over the triangle, so a coarse grid
/// finds its peak to well within the thresholds' precision.
constexpr int kBulgeSteps = 8;

using Corners = std::array<const Vertex *, kCorners>;

Corners CornersOf(const MeshData &mesh, std::size_t firstIndex)
{
    Corners corners{};
    for (std::size_t corner = 0; corner < kCorners; ++corner)
    {
        corners[corner] = &mesh.Vertices[mesh.Indices[firstIndex + corner]];
    }
    return corners;
}

/// Whether a corner of the triangle bends its normal past
/// kMaxSmoothingBendDegrees from the triangle's own. False for a triangle with
/// no area, and a corner with no normal is passed over.
bool HasBentCorner(const Corners &corners, float limitCosine)
{
    const glm::vec3 face =
        glm::cross(corners[1]->Position - corners[0]->Position, corners[2]->Position - corners[0]->Position);
    const float faceLength = glm::length(face);
    if (faceLength == 0.f)
    {
        return false;
    }
    for (const Vertex *corner : corners)
    {
        const float normalLength = glm::length(corner->Normal);
        if (normalLength > 0.f && glm::dot(face, corner->Normal) < limitCosine * faceLength * normalLength)
        {
            return true;
        }
    }
    return false;
}

/// How far the curve the triangle's vertex normals describe stands off it at
/// its furthest: each corner's tangent plane lifts a point by however far the
/// point lies below it, and the lifts blend as the corners do.
float BulgeOf(const Corners &corners)
{
    std::array<glm::vec3, kCorners> normals{};
    for (std::size_t corner = 0; corner < kCorners; ++corner)
    {
        const float length = glm::length(corners[corner]->Normal);
        normals[corner] = length > 0.f ? corners[corner]->Normal / length : glm::vec3(0.f);
    }
    const float steps = static_cast<float>(kBulgeSteps);
    float furthest = 0.f;
    for (int i = 0; i <= kBulgeSteps; ++i)
    {
        for (int j = 0; i + j <= kBulgeSteps; ++j)
        {
            const std::array<float, kCorners> weights{static_cast<float>(i) / steps, static_cast<float>(j) / steps,
                                                      static_cast<float>(kBulgeSteps - i - j) / steps};
            glm::vec3 point(0.f);
            for (std::size_t corner = 0; corner < kCorners; ++corner)
            {
                point += weights[corner] * corners[corner]->Position;
            }
            glm::vec3 lift(0.f);
            for (std::size_t corner = 0; corner < kCorners; ++corner)
            {
                const float below = std::max(glm::dot(corners[corner]->Position - point, normals[corner]), 0.f);
                lift += weights[corner] * below * normals[corner];
            }
            furthest = std::max(furthest, glm::length(lift));
        }
    }
    return furthest;
}

/// The largest extent of @p mesh's bounding box.
float SizeOf(const MeshData &mesh)
{
    glm::vec3 low(0.f);
    glm::vec3 high(0.f);
    if (!mesh.Vertices.empty())
    {
        low = mesh.Vertices.front().Position;
        high = low;
    }
    for (const Vertex &vertex : mesh.Vertices)
    {
        low = glm::min(low, vertex.Position);
        high = glm::max(high, vertex.Position);
    }
    const glm::vec3 extent = high - low;
    return std::max({extent.x, extent.y, extent.z});
}
} // namespace

std::vector<SharpSmoothing> FindSharpSmoothing(const MeshData &mesh)
{
    const float limitCosine = std::cos(glm::radians(kMaxSmoothingBendDegrees));
    const float size = SizeOf(mesh);
    std::vector<SharpSmoothing> found;
    if (size == 0.f)
    {
        return found;
    }
    for (std::size_t subMesh = 0; subMesh < mesh.SubMeshes.size(); ++subMesh)
    {
        const SubMesh &range = mesh.SubMeshes[subMesh];
        if (range.MaterialSlot < mesh.Materials.size() && !mesh.Materials[range.MaterialSlot].ShadowsFollowSmoothing)
        {
            continue;
        }

        float bulge = 0.f;
        for (std::size_t index = range.IndexOffset; index + kCorners <= range.IndexOffset + range.IndexCount;
             index += kCorners)
        {
            const Corners corners = CornersOf(mesh, index);
            if (HasBentCorner(corners, limitCosine))
            {
                bulge = std::max(bulge, BulgeOf(corners));
            }
        }
        if (bulge / size >= kMinorSmoothingBulge)
        {
            found.push_back(SharpSmoothing{.bulge = bulge / size, .subMesh = static_cast<uint32_t>(subMesh)});
        }
    }
    return found;
}

} // namespace Assisi::Geometry
