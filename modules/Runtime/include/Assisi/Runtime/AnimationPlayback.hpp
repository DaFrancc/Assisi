/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AnimationPlayback.hpp
/// @brief One frame of an AnimationPlayer: move its timeline on and write its
///        animation into the pose, fading when the animation changed. The
///        system that runs it each Update only finds the clips and the mesh.

#include <Assisi/Geometry/AnimationClip.hpp>
#include <Assisi/Geometry/BlendSpace.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Runtime/Components.hpp>

#include <memory>
#include <span>

namespace Assisi::Runtime
{

/// @brief What a player's `animation` resolved to: one clip, a blend space and
///        the clip of each of its points in order, or no clips when it is nil.
struct ResolvedAnimation
{
    std::shared_ptr<const Geometry::BlendSpace> space; ///< Null for a clip on its own.
    std::span<const std::shared_ptr<const Geometry::AnimationClip>> clips;
};

/// @brief Plays @p animation on @p skinned for @p dt seconds, for @p player.
///
/// Rebinds first when the animation, any of its loaded clips or the mesh
/// changed since last time: matches the clips' tracks to @p skeleton and starts
/// from the top when it is a different animation. A different animation fades
/// in over the player's `fade` from the pose as it is; anything else puts the
/// pose back to rest. A nil animation is one that moves nothing, so clearing it
/// returns the pose to rest and then leaves it to code. Returns whether it rebound, which is when the caller
/// reports missing joints. Does nothing while @p skinned is not sized for
/// @p skeleton.
bool AdvanceAnimationPlayer(AnimationPlayer &player, const ResolvedAnimation &animation,
                            const Geometry::Skeleton &skeleton, float dt, SkinnedMesh &skinned);

} // namespace Assisi::Runtime
