/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AnimationPlayback.hpp
/// @brief One frame of an AnimationPlayer: move its time on and write its clip
///        into the pose. The system that runs it each Update only finds the
///        clip and the mesh.

#include <Assisi/Geometry/AnimationClip.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Runtime/Components.hpp>

#include <memory>

namespace Assisi::Runtime
{

/// @brief Plays @p clip on @p skinned for @p dt seconds, for @p player.
///
/// Rebinds first when the clip, the loaded clip or the mesh changed since last
/// time: matches the clip's tracks to @p skeleton, puts the pose back to rest,
/// and starts from the top only when it is a different clip. Returns whether it
/// rebound, which is when the caller reports missing joints. Does nothing while
/// @p skinned is not sized for @p skeleton.
bool AdvanceAnimationPlayer(AnimationPlayer &player, std::shared_ptr<const Geometry::AnimationClip> clip,
                            const Geometry::Skeleton &skeleton, float dt, SkinnedMesh &skinned);

} // namespace Assisi::Runtime
