/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/SkinnedMeshPose.hpp>

#include <Assisi/Geometry/Pose.hpp>
#include <Assisi/Render/AssetCache.hpp>
#include <Assisi/Render/MeshSkinner.hpp>

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
        // The old mesh's range is let go by no longer being kept.
        skinned.posed = Render::MeshBuffer{};
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

    // Culled by where the pose puts it: the bind pose's bounds would cull a
    // raised arm the moment the body left the screen.
    if (skinned.posed.Id() != 0 && skeleton->JointBounds.size() == skeleton->JointCount())
    {
        const Geometry::Aabb bounds = Geometry::PosedBounds(skeleton->JointBounds, skinned.palette);
        if (!Geometry::IsEmpty(bounds))
        {
            skinned.posed.SetLocalBounds(Geometry::SphereAround(bounds), bounds);
        }
    }
}

void EvaluateScenePoses(ECS::Scene &scene, Render::AssetCache &cache)
{
    for (auto [entity, skinned, renderer] : scene.Query<Mut<SkinnedMesh>, MeshRenderer>())
    {
        // Kept first, so the copy evaluation fits the bounds of is this frame's.
        // A mesh with a skeleton but no weights draws as it is.
        if (BindSkinnedMesh(skinned, renderer) != nullptr &&
            !cache.KeepPosedInstance(*renderer.meshBuffer, skinned.posed))
        {
            skinned.posed = Render::MeshBuffer{};
        }
        EvaluateSkinnedMesh(skinned, renderer);
    }
    cache.EndSkinFrame();
}

void UnbindSkinnedMesh(SkinnedMesh &skinned)
{
    skinned.pose.clear();
    skinned.jointModel.clear();
    skinned.palette.clear();
    skinned.boundMeshId = kUnboundMesh;
    skinned.posed = Render::MeshBuffer{};
}

const Render::MeshBuffer *DrawnMesh(const MeshRenderer &renderer, const SkinnedMesh *skinned)
{
    if (skinned != nullptr && skinned->posed.Id() != 0)
    {
        return &skinned->posed;
    }
    return renderer.meshBuffer;
}

const Render::MeshBuffer *GatherPosedInstances(const ECS::Scene &scene, Render::SkinBatch &batch)
{
    batch.Reset();
    const Render::MeshBuffer *anyMesh = nullptr;
    for (auto [entity, skinned, renderer] : scene.Query<SkinnedMesh, MeshRenderer>())
    {
        const Render::MeshBuffer *source = renderer.meshBuffer;
        if (source == nullptr || skinned.posed.Id() == 0 || skinned.palette.size() != source->Skeleton().JointCount())
        {
            continue;
        }
        anyMesh = source;
        batch.Add(Render::SkinDispatch{.sourceVertexBase = source->VertexBase(),
                                       .posedVertexBase = skinned.posed.VertexBase(),
                                       .vertexCount = source->VertexCount(),
                                       .skinBase = source->SkinBase()},
                  skinned.palette);
    }
    return anyMesh;
}

} // namespace Assisi::Runtime
