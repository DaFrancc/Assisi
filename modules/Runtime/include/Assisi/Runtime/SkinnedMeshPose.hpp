/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SkinnedMeshPose.hpp
/// @brief Binds a SkinnedMesh to the skeleton its MeshRenderer resolved, and
///        turns its pose into model-space joints and a skinning palette.
///
/// Binding is idempotent and cheap when nothing changed, so every caller binds
/// before it reads rather than relying on someone having bound first.

#include <cstdint>

#include <Assisi/ECS/Scene.hpp>
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
const Geometry::Skeleton *BindPose(SkinnedMesh &skinned, const Geometry::Skeleton &skeleton, uint32_t meshId);

/// @brief BindPose against the mesh @p renderer resolved; unbinds while it has
///        none.
const Geometry::Skeleton *BindSkinnedMesh(SkinnedMesh &skinned, const MeshRenderer &renderer);

/// @brief Binds, then writes `jointModel` and `palette` from `pose`.
void EvaluateSkinnedMesh(SkinnedMesh &skinned, const MeshRenderer &renderer);

/// @brief Hashes @p skinned's palette and sets `poseChanged` to whether it
///        differs from the last evaluation's. The first evaluation after a bind
///        is no change: nothing has been drawn at another pose yet.
void NotePoseChange(SkinnedMesh &skinned);

/// @brief EvaluateSkinnedMesh for every entity in @p scene that has one, and
///        keeps each one's posed copy holding a vertex range of its own in
///        @p cache. Runs every frame before transforms propagate.
///
/// Every frame, because a range nothing keeps for a frame is given away.
void EvaluateScenePoses(ECS::Scene &scene, Render::AssetCache &cache);

/// @brief Empties @p skinned and marks it unbound, for a mesh about to be freed.
void UnbindSkinnedMesh(SkinnedMesh &skinned);

/// @brief The mesh an entity draws as: its posed copy once that holds a range,
///        otherwise @p renderer's mesh. @p skinned may be null.
///
/// Everything that draws, culls or casts a shadow from an entity's mesh reads it
/// through here, so a posed instance is never drawn from the bind pose or merged
/// with another instance of the same mesh.
[[nodiscard]] const Render::MeshBuffer *DrawnMesh(const MeshRenderer &renderer, const SkinnedMesh *skinned);

/// @brief Fills @p batch with one skinning dispatch per entity whose posed copy
///        holds a range. Returns one of their meshes, which names the arena
///        every one of them lives in, or null when there were none.
const Render::MeshBuffer *GatherPosedInstances(const ECS::Scene &scene, Render::SkinBatch &batch);

} // namespace Assisi::Runtime
