/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/SkinnedMeshPose.hpp>

#include <Assisi/Geometry/Pose.hpp>

namespace Assisi::Runtime
{

const Geometry::Skeleton *BindPose(SkinnedMesh &skinned, const Geometry::Skeleton &skeleton, uint32_t meshId)
{
    if (skeleton.Empty())
    {
        UnbindSkinnedMesh(skinned);
        return nullptr;
    }
    if (skinned.boundMeshId != meshId)
    {
        skinned.pose = skeleton.RestLocal;
        skinned.jointModel.assign(skeleton.JointCount(), glm::mat4(1.f));
        skinned.palette.assign(skeleton.JointCount(), glm::mat4(1.f));
        skinned.boundMeshId = meshId;
    }
    return &skeleton;
}

const Geometry::Skeleton *BindSkinnedMesh(SkinnedMesh &skinned, const MeshRenderer &renderer)
{
    if (renderer.meshBuffer == nullptr)
    {
        UnbindSkinnedMesh(skinned);
        return nullptr;
    }
    return BindPose(skinned, renderer.meshBuffer->Skeleton(), renderer.meshBuffer->Id());
}

void EvaluateSkinnedMesh(SkinnedMesh &skinned, const MeshRenderer &renderer)
{
    const Geometry::Skeleton *skeleton = BindSkinnedMesh(skinned, renderer);
    if (skeleton == nullptr)
    {
        return;
    }
    Geometry::JointModelTransforms(*skeleton, skinned.pose, skinned.jointModel);
    Geometry::SkinningPalette(*skeleton, skinned.jointModel, skinned.palette);
}

void EvaluateScenePoses(ECS::Scene &scene)
{
    for (auto [entity, skinned, renderer] : scene.Query<Mut<SkinnedMesh>, MeshRenderer>())
    {
        EvaluateSkinnedMesh(skinned, renderer);
    }
}

void UnbindSkinnedMesh(SkinnedMesh &skinned)
{
    skinned.pose.clear();
    skinned.jointModel.clear();
    skinned.palette.clear();
    skinned.boundMeshId = kUnboundMesh;
}

} // namespace Assisi::Runtime
