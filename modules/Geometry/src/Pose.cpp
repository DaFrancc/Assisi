/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Geometry/Pose.hpp>

#include <Assisi/Core/Assert.hpp>

#include <cstddef>

namespace Assisi::Geometry
{

glm::mat4 JointMatrix(const JointTransform &local)
{
    return glm::translate(glm::mat4(1.f), local.Translation) * glm::mat4_cast(local.Rotation) *
           glm::scale(glm::mat4(1.f), local.Scale);
}

void JointModelTransforms(const Skeleton &skeleton, std::span<const JointTransform> local,
                          std::span<glm::mat4> outModel)
{
    const std::size_t jointCount = skeleton.JointCount();
    ASSISI_ASSERT(local.size() == jointCount && outModel.size() == jointCount,
                  "a pose and its output hold one entry per joint");
    for (std::size_t joint = 0; joint < jointCount; ++joint)
    {
        const int32_t parent = skeleton.Parents[joint];
        const glm::mat4 &placedBy =
            parent == kNoParent ? skeleton.RootTransform : outModel[static_cast<std::size_t>(parent)];
        outModel[joint] = placedBy * JointMatrix(local[joint]);
    }
}

void SkinningPalette(const Skeleton &skeleton, std::span<const glm::mat4> model, std::span<glm::mat4> outPalette)
{
    const std::size_t jointCount = skeleton.JointCount();
    ASSISI_ASSERT(model.size() == jointCount && outPalette.size() == jointCount,
                  "model transforms and the palette hold one entry per joint");
    for (std::size_t joint = 0; joint < jointCount; ++joint)
    {
        outPalette[joint] = model[joint] * skeleton.InverseBind[joint];
    }
}

int32_t FindJoint(const Skeleton &skeleton, std::string_view name)
{
    for (uint32_t joint = 0; joint < skeleton.JointCount(); ++joint)
    {
        if (skeleton.Names[joint] == name)
        {
            return static_cast<int32_t>(joint);
        }
    }
    return kNoJoint;
}

bool IsEmpty(const Aabb &box)
{
    return glm::any(glm::greaterThan(box.min, box.max));
}

namespace
{

void Extend(Aabb &box, const Aabb &by)
{
    box.min = glm::min(box.min, by.min);
    box.max = glm::max(box.max, by.max);
}

} // namespace

std::vector<Aabb> FitJointBounds(std::span<const Vertex> vertices, std::span<const VertexSkin> skin,
                                 uint32_t jointCount)
{
    ASSISI_ASSERT(skin.size() == vertices.size(), "one skin record per vertex");
    std::vector<Aabb> bounds(jointCount, kEmptyAabb);
    for (std::size_t vertex = 0; vertex < vertices.size(); ++vertex)
    {
        const glm::vec3 &position = vertices[vertex].Position;
        for (int32_t slot = 0; slot < static_cast<int32_t>(kMaxInfluences); ++slot)
        {
            const uint32_t joint = skin[vertex].Joints[slot];
            if (skin[vertex].Weights[slot] > 0.f && joint < jointCount)
            {
                Extend(bounds[joint], Aabb{.min = position, .max = position});
            }
        }
    }
    return bounds;
}

Aabb PosedBounds(std::span<const Aabb> jointBounds, std::span<const glm::mat4> palette)
{
    ASSISI_ASSERT(jointBounds.size() == palette.size(), "one box and one matrix per joint");
    Aabb posed = kEmptyAabb;
    for (std::size_t joint = 0; joint < jointBounds.size(); ++joint)
    {
        if (!IsEmpty(jointBounds[joint]))
        {
            Extend(posed, TransformedAabb(jointBounds[joint], palette[joint]));
        }
    }
    return posed;
}

BoundingSphere SphereAround(const Aabb &box)
{
    return BoundingSphere{.center = 0.5f * (box.min + box.max), .radius = 0.5f * glm::length(box.max - box.min)};
}

} // namespace Assisi::Geometry
