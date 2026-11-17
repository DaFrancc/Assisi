/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AnimationPlayback.hpp
/// @brief One frame of an AnimationPlayer, in two steps. Advancing moves each
///        of its animations on, fading when one changed, and needs no
///        skeleton. Sampling writes them into the pose, the base first and
///        each layer over it. The system that runs them each Update only
///        finds the clips and the mesh.

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
///
/// AdvanceTrack reads `animation` through `loop`; SampleTrack reads only
/// `underneath`, `meshId` and `refill`. Each leaves the other's fields alone.
struct TrackSettings
{
    /// Sampling: what a joint the animation does not move is given when the
    /// track writes it: the rest pose for the base, the pose below for a layer.
    std::span<const Geometry::JointTransform> underneath;
    Core::AssetId animation;       ///< Advancing.
    glm::vec2 parameter{0.f, 0.f}; ///< Advancing.
    float speed = 1.f;             ///< Advancing.
    float startPhase = 0.f;        ///< Advancing: where a new start of the animation begins.
    float fade = 0.f;              ///< Advancing.
    float dt = 0.f;                ///< Advancing.
    uint32_t meshId = kUnboundMesh; ///< Sampling: the mesh the pose is for.
    uint32_t restarts = 0;          ///< Advancing: starts the animation again at `startPhase` when it moves.
    bool loop = true;               ///< Advancing.
    /// Sampling: whether the joints the animation does not move take
    /// `underneath` every frame, as a layer's do, or only when it rebinds, as
    /// the base's do so that code can set them.
    bool refill = false;
};

/// @brief Moves @p track on by @p settings: which clips of @p animation play,
///        their weights and phases, and the fade's clock. Needs no skeleton.
///
/// Takes the clips again when the animation or any of its loaded clips
/// changed, starting at `startPhase` when it is a different animation. A
/// different animation fades in over the fade, except the first, which cuts;
/// anything else cuts. A nil animation is one that moves nothing. Does nothing
/// while a clip is loading. Returns whether it took new clips.
bool AdvanceTrack(AnimationTrack &track, const TrackSettings &settings, const ResolvedAnimation &animation);

/// @brief Writes what @p track plays into @p out, over `underneath`.
///
/// Binds the clips to @p skeleton first when they or the mesh are new; a new
/// mesh ends any fade and starts @p out from `underneath`. A fade starts from
/// what @p out holds when it is first sampled. Writes nothing while the track
/// has nothing to play. Returns whether it bound. @p out holds one entry per
/// joint.
bool SampleTrack(AnimationTrack &track, const TrackSettings &settings, const Geometry::Skeleton &skeleton,
                 std::span<Geometry::JointTransform> out);

/// @brief Fills @p request from @p player's own fields, for a player no
///        Animator drives.
///
/// `restarts` is left as it is: only an Animator raises it, and a player it
/// stopped driving goes on from the count its track already took.
void RequestFromAuthored(const AnimationPlayer &player, PlayerRequest &request);

/// @brief Moves the base of @p playback's request on by @p dt seconds of
///        @p animation. Needs no skeleton, so an animator can run where
///        nothing is posed.
void AdvancePlayerTime(AnimationPlayback &playback, const ResolvedAnimation &animation, float dt);

/// @brief Writes the base of @p playback's request into @p skinned's pose,
///        under any layers.
///
/// Clearing the animation returns the pose to rest and then leaves it to code.
/// Returns whether it bound clips, which is when the caller reports missing
/// joints. Does nothing while @p skinned is not sized for @p skeleton.
bool SamplePlayerPose(AnimationPlayback &playback, const Geometry::Skeleton &skeleton, SkinnedPose &skinned);

/// @brief AdvancePlayerTime then SamplePlayerPose: one frame of the base. Does
///        nothing at all while @p skinned is not sized for @p skeleton.
bool AdvanceAnimationPlayer(AnimationPlayback &playback, const ResolvedAnimation &animation,
                            const Geometry::Skeleton &skeleton, float dt, SkinnedPose &skinned);

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

/// @brief Gives each layer of @p playback's request its state, before they play.
///
/// A state follows its layer when the list is reordered or a layer removed,
/// found by its animation and mode, so moving a row neither restarts nor fades
/// anything. A layer whose animation or mode changed in place keeps the state
/// at its place, so it fades from what it was playing; a new layer starts a
/// fresh one.
void MatchLayerStates(AnimationPlayback &playback);

/// @brief Moves @p layer on by @p dt seconds of @p animation: its weight
///        towards the layer's over its weightFade, as weightSlide says, and
///        its timeline, at any weight, so it doesn't restart when the layer
///        comes back. Needs no skeleton.
void AdvanceLayerTime(const LayerRequest &layer, LayerState &state, const ResolvedAnimation &animation, float dt);

/// @brief Combines @p layer's animation with @p skinned's pose inside its
///        mask, by its weight. A layer at weight 0 is not sampled at all.
///
/// Names its mask lacks are left in @p state for the caller to report. Returns
/// whether it bound clips, which is when the caller reports missing joints;
/// a layer that has never been sampled has none bound to report on. Does
/// nothing while @p skinned is not sized for @p skeleton.
bool SampleLayerPose(const LayerRequest &layer, LayerState &state, const Geometry::Skeleton &skeleton,
                     SkinnedPose &skinned);

/// @brief AdvanceLayerTime then SampleLayerPose: one frame of a layer. Does
///        nothing at all while @p skinned is not sized for @p skeleton.
void AdvanceAnimationLayer(const LayerRequest &layer, LayerState &state, const ResolvedAnimation &animation,
                           const Geometry::Skeleton &skeleton, float dt, SkinnedPose &skinned);

} // namespace Assisi::Runtime
