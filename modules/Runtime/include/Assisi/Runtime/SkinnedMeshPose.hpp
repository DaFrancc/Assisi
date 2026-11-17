/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SkinnedMeshPose.hpp
/// @brief Binds a SkinnedMesh's SkinnedPose to the skeleton its MeshRenderer
///        resolved, and turns the pose into model-space joints and a skinning
///        palette.
///
/// Binding is idempotent and cheap when nothing changed, so every caller binds
/// before it reads rather than relying on someone having bound first.

#include <cstdint>

#include <Assisi/ECS/SceneFwd.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Runtime/Components.hpp>

namespace Assisi::Render
{
class AssetCache;
class SkinBatch;
} // namespace Assisi::Render

namespace Assisi::Runtime
{

/// @brief Sizes @p skinned for @p skeleton, which belongs to mesh @p meshId.
///
/// A new mesh resets the pose to the skeleton's rest pose; the same mesh again
/// leaves the pose as it was. An empty skeleton unbinds. Returns the skeleton
/// bound, or null when none is.
const Geometry::Skeleton *BindPose(SkinnedPose &skinned, const Geometry::Skeleton &skeleton, uint32_t meshId);

/// @brief BindPose against the mesh @p renderer resolved; unbinds while it has
///        none.
const Geometry::Skeleton *BindSkinnedMesh(SkinnedPose &skinned, const MeshRenderer &renderer);

/// @brief Binds, copies `pose` into `finalPose`, then writes `jointModel` and
///        `palette` from it.
void EvaluateSkinnedMesh(SkinnedPose &skinned, const MeshRenderer &renderer);

/// @brief EvaluateSkinnedMesh for every SkinnedMesh in @p scene: the pose
///        stage's part that turns each animated pose into joint matrices.
///        Needs no renderer, only meshes that have loaded.
///
/// For a scene that is drawn this frame, pass the renderer's @p cache: each
/// posed copy is kept holding a vertex range of its own in it, and fitted to
/// the pose, in the same pass. Every frame the scene is drawn, because a range
/// nothing keeps for a frame is given away; one cache serves one scene a
/// frame. It starts with ReleaseOrphanedPoses, so a range given away is never
/// drawn.
void FinishPoses(ECS::Scene &scene, Render::AssetCache *cache = nullptr);

/// @brief The pose stage's last part: writes into each JointAttachment's
///        ParentFrame where its joint is in this frame's pose, after FinishPoses.
///
/// Writes a frame only when it moved, so a held item that is still costs
/// nothing to place. Looks a joint's name up again only when the name or the
/// parent's mesh changed. Takes the ParentFrame and JointAttachmentRun off an
/// entity whose JointAttachment was taken off, so it hangs from its parent's
/// origin again.
void FollowJoints(ECS::Scene &scene);

/// @brief Hashes @p skinned's palette and sets `poseChanged` to whether it
///        differs from the last evaluation's. The first evaluation after a bind
///        is no change: nothing has been drawn at another pose yet.
void NotePoseChange(SkinnedPose &skinned);

/// @brief Unbinds every SkinnedPose in @p scene whose SkinnedMesh was removed.
///
/// Removing a SkinnedMesh leaves the SkinnedPose it brought. Unbound, that pose
/// lets go of its posed copy, so the entity draws its MeshRenderer's mesh again
/// rather than a vertex range that is no longer kept for it.
void ReleaseOrphanedPoses(ECS::Scene &scene);

/// @brief Empties @p skinned and marks it unbound, for a mesh about to be freed.
void UnbindSkinnedMesh(SkinnedPose &skinned);

/// @brief The mesh an entity draws as: its posed copy once that holds a range,
///        otherwise @p renderer's mesh. @p skinned may be null.
///
/// Everything that draws, culls or casts a shadow from an entity's mesh reads it
/// through here, so a posed instance is never drawn from the bind pose or merged
/// with another instance of the same mesh.
[[nodiscard]] const Render::MeshBuffer *DrawnMesh(const MeshRenderer &renderer, const SkinnedPose *skinned);

/// @brief Fills @p batch with one skinning dispatch per entity whose posed copy
///        holds a range. Returns one of their meshes, which names the arena
///        every one of them lives in, or null when there were none.
const Render::MeshBuffer *GatherPosedInstances(const ECS::Scene &scene, Render::SkinBatch &batch);

} // namespace Assisi::Runtime
