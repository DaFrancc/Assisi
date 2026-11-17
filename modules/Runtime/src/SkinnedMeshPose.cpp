/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/SkinnedMeshPose.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <Assisi/Core/ContentHash.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/ParentFrame.hpp>
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

/// A scale this close to zero has no direction to take a rotation from.
constexpr float kNoScale = 1e-8f;

/// @p model, a joint's transform in its mesh's space, as a frame: its
/// position, its rotation and its scale along each of its axes.
ECS::ParentFrame FrameOf(const glm::mat4 &model)
{
    const glm::vec3 x(model[0]);
    const glm::vec3 y(model[1]);
    const glm::vec3 z(model[2]);
    const glm::vec3 scale(glm::length(x), glm::length(y), glm::length(z));
    ECS::ParentFrame frame{.position = glm::vec3(model[3]), .scale = scale};
    if (scale.x > kNoScale && scale.y > kNoScale && scale.z > kNoScale)
    {
        frame.rotation = glm::normalize(glm::quat_cast(glm::mat3(x / scale.x, y / scale.y, z / scale.z)));
    }
    return frame;
}

bool SameFrame(const ECS::ParentFrame &a, const ECS::ParentFrame &b)
{
    return a.rotation == b.rotation && a.position == b.position && a.scale == b.scale;
}

/// Where @p attachment's joint is in this frame's pose of @p entity's parent,
/// looking its name up again when the name or the mesh changed. The parent's
/// origin while the parent has no pose to read, a parent that is not skinned
/// or whose mesh has not loaded, so a pose read from another parent or mesh
/// never stays; and for a joint its mesh lacks, said once for each lookup that
/// misses.
ECS::ParentFrame JointFrame(const ECS::Scene &scene, ECS::Entity entity, const JointAttachment &attachment,
                            JointAttachmentRun &run)
{
    const ECS::Parent *parent = scene.Get<ECS::Parent>(entity);
    const SkinnedPose *skinned = parent != nullptr ? scene.Get<SkinnedPose>(parent->parent) : nullptr;
    const MeshRenderer *renderer = parent != nullptr ? scene.Get<MeshRenderer>(parent->parent) : nullptr;
    if (skinned == nullptr || renderer == nullptr || renderer->meshBuffer == nullptr ||
        skinned->boundMeshId == kUnboundMesh || skinned->jointModel.empty())
    {
        // Looked up again once there is a pose, even in the same mesh.
        run.meshId = kUnboundMesh;
        return ECS::ParentFrame{};
    }
    if (run.joint != attachment.joint || run.meshId != skinned->boundMeshId)
    {
        run.joint = attachment.joint;
        run.meshId = skinned->boundMeshId;
        run.index = Geometry::FindJoint(renderer->meshBuffer->Skeleton(), attachment.joint.View());
        if (run.index == Geometry::kNoJoint)
        {
            Core::Log::Warn("JointAttachment: the mesh of the parent of entity (index {}, gen {}) has no joint \"{}\", "
                            "so the entity hangs from its parent's origin.",
                            entity.index, entity.generation, attachment.joint.View());
        }
    }
    if (run.index == Geometry::kNoJoint || static_cast<std::size_t>(run.index) >= skinned->jointModel.size())
    {
        return ECS::ParentFrame{};
    }
    return FrameOf(skinned->jointModel[static_cast<std::size_t>(run.index)]);
}

/// Takes the frame and the lookup off every entity whose JointAttachment was
/// taken off, so it hangs from its parent's origin again.
void ReleaseDetached(ECS::Scene &scene)
{
    std::vector<ECS::Entity> detached;
    for (auto [entity, run] : scene.Query<JointAttachmentRun>())
    {
        (void)run;
        if (!scene.Has<JointAttachment>(entity))
        {
            detached.push_back(entity);
        }
    }
    for (const ECS::Entity entity : detached)
    {
        (void)scene.Remove<ECS::ParentFrame>(entity);
        (void)scene.Remove<JointAttachmentRun>(entity);
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

void FollowJoints(ECS::Scene &scene)
{
    for (auto [entity, attachment, run] : scene.Query<JointAttachment, Mut<JointAttachmentRun>>())
    {
        const ECS::ParentFrame frame = JointFrame(scene, entity, attachment, run);
        const ECS::ParentFrame *held = scene.Get<ECS::ParentFrame>(entity);
        if (held != nullptr && !SameFrame(*held, frame))
        {
            *scene.GetMut<ECS::ParentFrame>(entity) = frame;
        }
    }
    ReleaseDetached(scene);
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
