/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AnimationBlend.hpp
/// @brief Mixing clips into one pose: weights from a blend space's points, one
///        timeline the clips share, and a fade between two poses.
///
/// A single clip is a blend of one clip at full weight, so whatever plays a
/// clip and whatever plays a space are the same code.

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <Assisi/Geometry/AnimationClip.hpp>
#include <Assisi/Geometry/AnimationSampling.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::Geometry
{

/// @brief A clip whose weight is below this is left out of a blend: it would
///        move nothing a player could see, and sampling it costs the same as
///        any other.
inline constexpr float kNegligibleWeight = 1e-4f;

/// @brief Writes into @p weights how much each point of @p positions counts
///        for at @p parameter; they sum to 1.
///
/// Each point's weight falls from 1 at the point to 0 at every other point,
/// along the line towards it, and is the least of those. On a line this is the
/// straight mix of the two neighbours around the parameter; past the last point
/// that point has it all; inside a polygon of points they share it. @p weights
/// holds one entry per position.
void BlendWeights(std::span<const glm::vec2> positions, glm::vec2 parameter, std::span<float> weights);

/// @brief One clip of a blend and its tracks matched to the skeleton.
struct BlendSource
{
    std::shared_ptr<const AnimationClip> Clip;
    ClipBinding Binding;
};

/// @brief Clips mixed into one pose, kept in step by one timeline.
///
/// `Phase` runs from 0 to 1 through every clip at once, so clips of different
/// lengths reach the same moment of their cycle together: a walk and a run
/// put the same foot down at the same time.
struct ClipBlend
{
    std::vector<BlendSource> Sources;
    std::vector<float> Weights; ///< One per source, summing to 1.
    std::vector<JointTransform> Scratch;
    std::vector<uint8_t> Touched; ///< Per joint, whether any weighted source keys it.
    float Phase = 0.f;
};

/// @brief Moves @p blend's phase on by @p dt seconds.
///
/// The blend lasts as long as its weighted clips' average length, so a step
/// takes as long as the clips the weights favour. Wraps when @p loop, holds at
/// either end when not. Unchanged while the blend has no length.
void AdvancePhase(ClipBlend &blend, float dt, bool loop);

/// @brief Writes @p blend at its phase into @p out, one entry per joint.
///
/// Joints that no weighted source keys keep what @p out held. A source that
/// does not key a joint another one does counts as @p skeleton's rest for it.
/// Rotations are put on one side of the sphere before they are summed, as q
/// and -q are one rotation and their plain sum is nothing.
void SampleBlend(ClipBlend &blend, const Skeleton &skeleton, std::span<JointTransform> out);

/// @brief Writes @p blend at @p phase into @p out, as SampleBlend does at its
///        own phase, which it leaves as it was.
void SampleBlendAt(ClipBlend &blend, float phase, const Skeleton &skeleton, std::span<JointTransform> out);

/// @brief Writes into @p out the mix of @p from and @p to, @p weight of the
///        way from one to the other, for every joint.
void MixPoses(std::span<const JointTransform> from, std::span<const JointTransform> to, float weight,
              std::span<JointTransform> out);

/// @brief One joint of MixPoses: @p weight of the way from @p from to @p to.
[[nodiscard]] JointTransform MixJoint(const JointTransform &from, const JointTransform &to, float weight);

/// @brief Adds to @p below @p weight of the change from @p reference to
///        @p sample: the turn between them applied before below's own, in its
///        parent's frame, the move added, and the scale multiplied.
void AddJoint(JointTransform &below, const JointTransform &sample, const JointTransform &reference, float weight);

/// @brief How far a fade has got.
struct CrossFade
{
    float Elapsed = 0.f;
    float Duration = 0.f;

    /// How much of the pose is the one faded to, 0 to 1. 1 for a fade of no length.
    [[nodiscard]] float Weight() const;
    [[nodiscard]] bool Done() const;
    void Advance(float dt);
};

} // namespace Assisi::Geometry
