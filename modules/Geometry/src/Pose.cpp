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

} // namespace Assisi::Geometry
