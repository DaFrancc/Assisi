/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Geometry/SmoothingCheck.hpp>

#include <Assisi/Math/GLM.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>

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

/// The triangle's normal, as long as twice its area.
glm::vec3 FaceOf(const Corners &corners)
{
    return glm::cross(corners[1]->Position - corners[0]->Position, corners[2]->Position - corners[0]->Position);
}

/// Whether @p normal bends past kMaxSmoothingBendDegrees from a triangle whose
/// FaceOf is @p face. False for a triangle with no area or a corner with no
/// normal, which have no bend to measure.
bool BendsPast(const glm::vec3 &face, const glm::vec3 &normal, float limitCosine)
{
    const float faceLength = glm::length(face);
    const float normalLength = glm::length(normal);
    return faceLength > 0.f && normalLength > 0.f && glm::dot(face, normal) < limitCosine * faceLength * normalLength;
}

/// Whether a corner of the triangle bends its normal past
/// kMaxSmoothingBendDegrees from the triangle's own.
bool HasBentCorner(const Corners &corners, float limitCosine)
{
    const glm::vec3 face = FaceOf(corners);
    return std::ranges::any_of(corners,
                               [&](const Vertex *corner) { return BendsPast(face, corner->Normal, limitCosine); });
}

/// Whether @p range is one the check and the split look at: those whose shadows
/// follow their smoothing.
bool FollowsSmoothing(const MeshData &mesh, const SubMesh &range)
{
    return range.MaterialSlot >= mesh.Materials.size() || mesh.Materials[range.MaterialSlot].ShadowsFollowSmoothing;
}

/// A position or a normal as a key, exactly. Corners a texture seam gave
/// vertices of their own still meet at one position.
using VectorKey = std::array<float, 3>;

VectorKey KeyOf(const glm::vec3 &vector)
{
    return VectorKey{vector.x, vector.y, vector.z};
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
        if (!FollowsSmoothing(mesh, range))
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

namespace
{
/// What the split works from: each triangle's own normal, as a unit vector and
/// weighted by area, which triangles it may change, and which triangles meet at
/// each position.
struct SplitPlan
{
    std::vector<glm::vec3> faces;
    std::vector<glm::vec3> units;
    std::vector<bool> checked;
    std::map<VectorKey, std::vector<std::size_t>> around;
};

SplitPlan PlanSplit(const MeshData &mesh)
{
    const std::size_t triangles = mesh.Indices.size() / kCorners;
    SplitPlan plan{.faces = std::vector<glm::vec3>(triangles),
                   .units = std::vector<glm::vec3>(triangles),
                   .checked = std::vector<bool>(triangles, false),
                   .around = {}};
    for (std::size_t triangle = 0; triangle < triangles; ++triangle)
    {
        const Corners corners = CornersOf(mesh, triangle * kCorners);
        plan.faces[triangle] = FaceOf(corners);
        const float length = glm::length(plan.faces[triangle]);
        plan.units[triangle] = length > 0.f ? plan.faces[triangle] / length : glm::vec3(0.f);
        for (const Vertex *corner : corners)
        {
            plan.around[KeyOf(corner->Position)].push_back(triangle);
        }
    }
    for (const SubMesh &range : mesh.SubMeshes)
    {
        if (!FollowsSmoothing(mesh, range))
        {
            continue;
        }
        for (std::size_t index = range.IndexOffset; index + kCorners <= range.IndexOffset + range.IndexCount;
             index += kCorners)
        {
            plan.checked[index / kCorners] = true;
        }
    }
    return plan;
}

/// The normal a corner of @p triangle at @p position takes when it is split: the
/// area-weighted average of the triangles there that lie within the limit of
/// this one. It is itself within the limit of this triangle, an average of
/// directions that all are.
glm::vec3 SplitNormal(const SplitPlan &plan, std::size_t triangle, const glm::vec3 &position, float limitCosine)
{
    glm::vec3 sum(0.f);
    for (const std::size_t neighbour : plan.around.at(KeyOf(position)))
    {
        if (glm::dot(plan.units[neighbour], plan.units[triangle]) >= limitCosine)
        {
            sum += plan.faces[neighbour];
        }
    }
    return glm::normalize(sum);
}
} // namespace

std::size_t SplitSharpSmoothing(MeshData &mesh)
{
    const float limitCosine = std::cos(glm::radians(kMaxSmoothingBendDegrees));
    const SplitPlan plan = PlanSplit(mesh);

    // The vertex each split corner was given, by the vertex it came from and the
    // normal it took, so the corners there that take the same normal share one.
    std::map<std::pair<std::uint32_t, VectorKey>, std::uint32_t> given;
    std::size_t split = 0;
    for (std::size_t triangle = 0; triangle < plan.faces.size(); ++triangle)
    {
        if (!plan.checked[triangle])
        {
            continue;
        }
        for (std::size_t corner = 0; corner < kCorners; ++corner)
        {
            const std::uint32_t original = mesh.Indices[triangle * kCorners + corner];
            if (!BendsPast(plan.faces[triangle], mesh.Vertices[original].Normal, limitCosine))
            {
                continue;
            }
            const glm::vec3 normal = SplitNormal(plan, triangle, mesh.Vertices[original].Position, limitCosine);
            const std::pair<std::uint32_t, VectorKey> key{original, KeyOf(normal)};
            std::map<std::pair<std::uint32_t, VectorKey>, std::uint32_t>::const_iterator found = given.find(key);
            if (found == given.end())
            {
                Vertex vertex = mesh.Vertices[original];
                vertex.Normal = normal;
                mesh.Vertices.push_back(vertex);
                if (!mesh.Skin.empty())
                {
                    mesh.Skin.push_back(mesh.Skin[original]);
                }
                found = given.emplace(key, static_cast<std::uint32_t>(mesh.Vertices.size() - 1)).first;
            }
            mesh.Indices[triangle * kCorners + corner] = found->second;
            ++split;
        }
    }
    return split;
}

} // namespace Assisi::Geometry
