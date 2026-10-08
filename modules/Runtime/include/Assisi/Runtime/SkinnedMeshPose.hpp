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

/// @brief EvaluateSkinnedMesh for every entity in @p scene that has one. Runs
///        every frame before transforms propagate.
void EvaluateScenePoses(ECS::Scene &scene);

/// @brief Empties @p skinned and marks it unbound, for a mesh about to be freed.
void UnbindSkinnedMesh(SkinnedMesh &skinned);

} // namespace Assisi::Runtime
