/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AnimationPlayback.hpp
/// @brief One frame of an AnimationPlayer: move each of its animations on and
///        write them into the pose, the base first and each layer over it,
///        fading when an animation changed. The system that runs it each
///        Update only finds the clips and the mesh.

#include <Assisi/Core/InternedString.hpp>
#include <Assisi/Geometry/AnimationClip.hpp>
#include <Assisi/Geometry/BlendSpace.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Runtime/Components.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Assisi::Runtime
{

/// @brief What an animation resolved to: one clip, a blend space and the clip
///        of each of its points in order, or no clips when it is nil.
struct ResolvedAnimation
{
    std::shared_ptr<const Geometry::BlendSpace> space; ///< Null for a clip on its own.
    std::span<const std::shared_ptr<const Geometry::AnimationClip>> clips;
};

/// @brief What a track plays by this frame: the player's own settings or a layer's.
struct TrackSettings
{
    /// What a joint the animation does not move is given when the track
    /// writes it: the rest pose for the base, the pose below for a layer.
    std::span<const Geometry::JointTransform> underneath;
    Core::AssetId animation;
    glm::vec2 parameter{0.f, 0.f};
    float speed = 1.f;
    float fade = 0.f;
    float dt = 0.f;
    uint32_t meshId = kUnboundMesh; ///< The mesh the pose is for.
    bool loop = true;
    /// Whether the joints the animation does not move take `underneath` every
    /// frame, as a layer's do, or only when it rebinds, as the base's do so
    /// that code can set them.
    bool refill = false;
};

/// @brief Plays @p animation on @p track by @p settings, writing it into @p out.
///
/// Rebinds first when the animation, any of its loaded clips or the mesh
/// changed since last time: matches the clips' tracks to @p skeleton
/// and starts from the top when it is a different animation. A different
/// animation fades in over the fade from what @p out holds; anything else
/// starts @p out from `underneath`. A nil animation is one that moves nothing.
/// Returns whether it rebound. @p out holds one entry per joint.
bool AdvanceTrack(AnimationTrack &track, const TrackSettings &settings, const ResolvedAnimation &animation,
                  const Geometry::Skeleton &skeleton, std::span<Geometry::JointTransform> out);

/// @brief Plays @p animation on @p skinned for @p dt seconds, as @p player's
///        base: its own animation, under any layers.
///
/// Clearing the animation returns the pose to rest and then leaves it to code.
/// Returns whether it rebound, which is when the caller reports missing joints.
/// Does nothing while @p skinned is not sized for @p skeleton.
bool AdvanceAnimationPlayer(AnimationPlayer &player, const ResolvedAnimation &animation,
                            const Geometry::Skeleton &skeleton, float dt, SkinnedMesh &skinned);

/// @brief Which names of a mask matched no joint.
struct MaskResult
{
    bool rootMissing = false;
    bool exclusionMissing = false;
};

/// @brief Marks in @p out, one entry per joint of @p skeleton, the joints a
///        mask takes: @p root and every joint under it, or every joint for an
///        empty root, less each of @p exclusions and every joint under it.
///
/// A root the skeleton lacks takes nothing; an exclusion it lacks takes
/// nothing out. Joints may come in any order.
MaskResult BuildJointMask(const Geometry::Skeleton &skeleton, Core::InternedString root,
                          std::span<const Core::InternedString> exclusions, std::vector<uint8_t> &out);

/// @brief Plays @p layer's animation for @p dt seconds and combines it with
///        @p skinned's pose inside its mask, by its weight.
///
/// The weight moves towards the layer's over its weightFade, as weightSlide
/// says. Its timeline runs on at any weight, so it doesn't restart when the
/// layer comes back. Names its mask lacks are left in @p state for the caller
/// to report. Does nothing while @p skinned is not sized for @p skeleton.
void AdvanceAnimationLayer(const AnimationLayer &layer, LayerState &state, const ResolvedAnimation &animation,
                           const Geometry::Skeleton &skeleton, float dt, SkinnedMesh &skinned);

} // namespace Assisi::Runtime
