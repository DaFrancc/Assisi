/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Geometry/MeshValidate.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/Logger.hpp>

#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>

namespace Assisi::Geometry
{

namespace
{
/// Indices per triangle. The index array is a triangle list and nothing in the
/// pipeline reads it any other way.
constexpr std::size_t kIndicesPerTriangle = 3;

/// Whether @p point lies in @p box, edges included. Exact: the box was fit
/// from these very positions, so a point on its face compares equal.
bool BoxHolds(const Aabb &box, const glm::vec3 &point)
{
    return glm::all(glm::greaterThanEqual(point, box.min)) && glm::all(glm::lessThanEqual(point, box.max));
}

std::expected<void, MeshValidationError> ValidateSkeleton(const Skeleton &skeleton)
{
    const std::size_t jointCount = skeleton.Names.size();
    if (skeleton.Parents.size() != jointCount || skeleton.RestLocal.size() != jointCount ||
        skeleton.InverseBind.size() != jointCount || skeleton.JointBounds.size() != jointCount)
    {
        return std::unexpected(MeshValidationError::SkeletonTablesMismatch);
    }
    for (std::size_t joint = 0; joint < jointCount; ++joint)
    {
        const int32_t parent = skeleton.Parents[joint];
        if (parent != kNoParent && (parent < 0 || static_cast<std::size_t>(parent) >= joint))
        {
            return std::unexpected(MeshValidationError::JointOrder);
        }
    }

    std::unordered_set<std::string_view> names;
    for (const std::string &name : skeleton.Names)
    {
        if (!names.insert(name).second)
        {
            return std::unexpected(MeshValidationError::DuplicateJointName);
        }
    }
    return {};
}

std::expected<void, MeshValidationError> ValidateSkin(const MeshData &mesh)
{
    if (mesh.Skin.empty() && mesh.Skeleton.Empty())
    {
        return {};
    }
    if (mesh.Skeleton.Empty())
    {
        return std::unexpected(MeshValidationError::SkinWithoutSkeleton);
    }
    if (mesh.Skin.empty())
    {
        return std::unexpected(MeshValidationError::SkeletonWithoutSkin);
    }
    if (mesh.Skin.size() != mesh.Vertices.size())
    {
        return std::unexpected(MeshValidationError::SkinSizeMismatch);
    }
    if (const std::expected<void, MeshValidationError> skeleton = ValidateSkeleton(mesh.Skeleton); !skeleton)
    {
        return skeleton;
    }

    const uint32_t jointCount = mesh.Skeleton.JointCount();
    for (std::size_t vertex = 0; vertex < mesh.Skin.size(); ++vertex)
    {
        const VertexSkin &skin = mesh.Skin[vertex];
        float sum = 0.f;
        for (int32_t slot = 0; slot < static_cast<int32_t>(kMaxInfluences); ++slot)
        {
            if (skin.Weights[slot] < 0.f)
            {
                return std::unexpected(MeshValidationError::WeightsNotNormalized);
            }
            if (skin.Joints[slot] >= jointCount)
            {
                return std::unexpected(MeshValidationError::JointOutOfRange);
            }
            if (skin.Weights[slot] > 0.f &&
                !BoxHolds(mesh.Skeleton.JointBounds[skin.Joints[slot]], mesh.Vertices[vertex].Position))
            {
                return std::unexpected(MeshValidationError::JointBoundsMissVertex);
            }
            sum += skin.Weights[slot];
        }
        if (std::abs(sum - 1.f) > kWeightSumTolerance)
        {
            return std::unexpected(MeshValidationError::WeightsNotNormalized);
        }
    }
    return {};
}

} // namespace

std::string_view ToString(MeshValidationError error) noexcept
{
    switch (error)
    {
    case MeshValidationError::NoGeometry:
        return "has no vertices or no indices";
    case MeshValidationError::IndexCountNotTriangles:
        return "has an index count that is not a whole number of triangles";
    case MeshValidationError::IndexOutOfRange:
        return "has an index naming a vertex that does not exist";
    case MeshValidationError::SubMeshOutOfRange:
        return "has a submesh whose index range runs past the index array";
    case MeshValidationError::EmptySubMesh:
        return "has a submesh covering no indices";
    case MeshValidationError::MaterialSlotOutOfRange:
        return "has a submesh naming a material slot the mesh does not have";
    case MeshValidationError::NoLods:
        return "has submeshes but no LOD addressing them";
    case MeshValidationError::LodOutOfRange:
        return "has a LOD whose submesh range runs past the submesh array";
    case MeshValidationError::EmptyLod:
        return "has a LOD covering no submeshes";
    case MeshValidationError::LodsNotContiguous:
        return "has LODs that do not tile the submesh array in order";
    case MeshValidationError::LodThresholdNotDescending:
        return "has authored LOD screen sizes that do not fall across the chain";
    case MeshValidationError::SkinSizeMismatch:
        return "has a skin array that is not one entry per vertex";
    case MeshValidationError::SkinWithoutSkeleton:
        return "has skinned vertices but no skeleton";
    case MeshValidationError::SkeletonWithoutSkin:
        return "has a skeleton but no skinned vertices";
    case MeshValidationError::SkeletonTablesMismatch:
        return "has skeleton tables of different lengths";
    case MeshValidationError::JointOrder:
        return "has a joint whose parent does not come before it";
    case MeshValidationError::JointOutOfRange:
        return "has a vertex naming a joint the skeleton does not have";
    case MeshValidationError::WeightsNotNormalized:
        return "has a vertex whose weights are negative or do not sum to one";
    case MeshValidationError::DuplicateJointName:
        return "has two joints with the same name";
    case MeshValidationError::JointBoundsMissVertex:
        return "has a joint whose bounds leave out a vertex it moves";
    default:
        ASSISI_ASSERT(false, "ToString reached a MeshValidationError with no description");
        Core::Log::Error("MeshValidate: no description for this error");
        return "is invalid";
    }
}

std::expected<void, MeshValidationError> ValidateMesh(const MeshData &mesh)
{
    if (mesh.Vertices.empty() || mesh.Indices.empty())
    {
        return std::unexpected(MeshValidationError::NoGeometry);
    }
    if (mesh.Indices.size() % kIndicesPerTriangle != 0)
    {
        return std::unexpected(MeshValidationError::IndexCountNotTriangles);
    }

    const auto vertexCount = static_cast<std::uint32_t>(mesh.Vertices.size());
    for (const std::uint32_t index : mesh.Indices)
    {
        if (index >= vertexCount)
        {
            return std::unexpected(MeshValidationError::IndexOutOfRange);
        }
    }

    if (mesh.SubMeshes.empty())
    {
        // The degenerate form, which EnsureSubMeshTables normalizes away. A mesh
        // that reaches here without it has geometry no LOD can select.
        return std::unexpected(MeshValidationError::NoLods);
    }

    const std::size_t indexCount = mesh.Indices.size();
    const std::size_t slotCount  = mesh.Materials.size();
    for (const SubMesh &submesh : mesh.SubMeshes)
    {
        if (submesh.IndexCount == 0)
        {
            return std::unexpected(MeshValidationError::EmptySubMesh);
        }
        // Summed as size_t so a range whose end overflows uint32 is caught here
        // rather than wrapping to a small number that passes.
        const std::size_t end =
            static_cast<std::size_t>(submesh.IndexOffset) + static_cast<std::size_t>(submesh.IndexCount);
        if (end > indexCount)
        {
            return std::unexpected(MeshValidationError::SubMeshOutOfRange);
        }
        if (submesh.MaterialSlot >= slotCount)
        {
            return std::unexpected(MeshValidationError::MaterialSlotOutOfRange);
        }
    }

    if (mesh.Lods.empty())
    {
        return std::unexpected(MeshValidationError::NoLods);
    }

    // The submesh array is stored grouped by LOD, LOD0 first, so the ranges have
    // to tile it exactly: a gap is geometry no level draws, and an overlap is
    // geometry two levels both draw.
    std::size_t expectedFirst = 0;
    for (const LodRange &lod : mesh.Lods)
    {
        if (lod.SubMeshCount == 0)
        {
            return std::unexpected(MeshValidationError::EmptyLod);
        }
        if (lod.FirstSubMesh != expectedFirst)
        {
            return std::unexpected(MeshValidationError::LodsNotContiguous);
        }
        const std::size_t end =
            static_cast<std::size_t>(lod.FirstSubMesh) + static_cast<std::size_t>(lod.SubMeshCount);
        if (end > mesh.SubMeshes.size())
        {
            return std::unexpected(MeshValidationError::LodOutOfRange);
        }
        expectedFirst = end;
    }
    if (expectedFirst != mesh.SubMeshes.size())
    {
        return std::unexpected(MeshValidationError::LodsNotContiguous);
    }

    // Selection takes the first level the instance is still big enough for, so a
    // threshold that does not fall makes every level after it unreachable. Only
    // the authored ones are compared: a zero means none was authored and the
    // default ladder supplies one, which descends by construction.
    float previousThreshold = 0.f;
    bool haveAuthored       = false;
    for (const LodRange &lod : mesh.Lods)
    {
        if (lod.ScreenSizeThreshold <= 0.f)
        {
            continue;
        }
        if (haveAuthored && lod.ScreenSizeThreshold >= previousThreshold)
        {
            return std::unexpected(MeshValidationError::LodThresholdNotDescending);
        }
        previousThreshold = lod.ScreenSizeThreshold;
        haveAuthored      = true;
    }

    return ValidateSkin(mesh);
}

} // namespace Assisi::Geometry
