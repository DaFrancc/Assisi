/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/SkinnedMeshPose.hpp>

#include <span>

#include <Assisi/Core/ContentHash.hpp>
#include <Assisi/Geometry/Pose.hpp>
#include <Assisi/Render/AssetCache.hpp>
#include <Assisi/Render/MeshSkinner.hpp>

namespace Assisi::Runtime
{

namespace
{

/// Fits @p skinned's posed copy to where its palette puts @p skeleton's
/// joints, once the copy holds a range: culled by where the pose puts it, the
/// bind pose's bounds would cull a raised arm the moment the body left the
/// screen.
void FitPosedBounds(SkinnedPose &skinned, const Geometry::Skeleton &skeleton)
{
    if (skinned.posed.Id() == 0 || skeleton.JointBounds.size() != skeleton.JointCount())
    {
        return;
    }
    const Geometry::Aabb bounds = Geometry::PosedBounds(skeleton.JointBounds, skinned.palette);
    if (!Geometry::IsEmpty(bounds))
    {
        skinned.posed.SetLocalBounds(Geometry::SphereAround(bounds), bounds);
    }
}

} // namespace

const Geometry::Skeleton *BindPose(SkinnedPose &skinned, const Geometry::Skeleton &skeleton, uint32_t meshId)
{
    if (skeleton.Empty())
    {
        UnbindSkinnedMesh(skinned);
        return nullptr;
    }
    if (skinned.boundMeshId != meshId)
    {
        skinned.pose = skeleton.RestLocal;
        skinned.finalPose = skeleton.RestLocal;
        skinned.jointModel.assign(skeleton.JointCount(), glm::mat4(1.f));
        skinned.palette.assign(skeleton.JointCount(), glm::mat4(1.f));
        skinned.boundMeshId = meshId;
        // The old mesh's range is let go by no longer being kept.
        skinned.posed = Render::MeshBuffer{};
        skinned.paletteHash = 0;
        skinned.poseChanged = false;
    }
    return &skeleton;
}

const Geometry::Skeleton *BindSkinnedMesh(SkinnedPose &skinned, const MeshRenderer &renderer)
{
    if (renderer.meshBuffer == nullptr)
    {
        UnbindSkinnedMesh(skinned);
        return nullptr;
    }
    return BindPose(skinned, renderer.meshBuffer->Skeleton(), renderer.meshBuffer->Id());
}

void EvaluateSkinnedMesh(SkinnedPose &skinned, const MeshRenderer &renderer)
{
    const Geometry::Skeleton *skeleton = BindSkinnedMesh(skinned, renderer);
    if (skeleton == nullptr)
    {
        return;
    }
    skinned.finalPose.assign(skinned.pose.begin(), skinned.pose.end());
    Geometry::JointModelTransforms(*skeleton, skinned.finalPose, skinned.jointModel);
    Geometry::SkinningPalette(*skeleton, skinned.jointModel, skinned.palette);
    NotePoseChange(skinned);
    FitPosedBounds(skinned, *skeleton);
}

void FinishPoses(ECS::Scene &scene, Render::AssetCache *cache)
{
    ReleaseOrphanedPoses(scene);
    for (auto [entity, skinned, marker, renderer] : scene.Query<Mut<SkinnedPose>, SkinnedMesh, MeshRenderer>())
    {
        // Kept first, so the copy evaluation fits the bounds of is this frame's.
        // A mesh with a skeleton but no weights draws as it is.
        if (cache != nullptr && BindSkinnedMesh(skinned, renderer) != nullptr &&
            !cache->KeepPosedInstance(*renderer.meshBuffer, skinned.posed))
        {
            skinned.posed = Render::MeshBuffer{};
        }
        EvaluateSkinnedMesh(skinned, renderer);
    }
    if (cache != nullptr)
    {
        cache->EndSkinFrame();
    }
}

void NotePoseChange(SkinnedPose &skinned)
{
    const std::span<const glm::mat4> palette = skinned.palette;
    const uint64_t hash = Core::ContentHash64(std::as_bytes(palette));
    skinned.poseChanged = skinned.paletteHash != 0 && hash != skinned.paletteHash;
    skinned.paletteHash = hash;
}

void ReleaseOrphanedPoses(ECS::Scene &scene)
{
    for (auto [entity, skinned] : scene.Query<Mut<SkinnedPose>>())
    {
        const bool holding = skinned.boundMeshId != kUnboundMesh || skinned.posed.Id() != 0;
        if (holding && !scene.Has<SkinnedMesh>(entity))
        {
            UnbindSkinnedMesh(skinned);
        }
    }
}

void UnbindSkinnedMesh(SkinnedPose &skinned)
{
    skinned.pose.clear();
    skinned.finalPose.clear();
    skinned.jointModel.clear();
    skinned.palette.clear();
    skinned.boundMeshId = kUnboundMesh;
    skinned.posed = Render::MeshBuffer{};
    skinned.paletteHash = 0;
    skinned.poseChanged = false;
}

const Render::MeshBuffer *DrawnMesh(const MeshRenderer &renderer, const SkinnedPose *skinned)
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
    for (auto [entity, skinned, marker, renderer] : scene.Query<SkinnedPose, SkinnedMesh, MeshRenderer>())
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
