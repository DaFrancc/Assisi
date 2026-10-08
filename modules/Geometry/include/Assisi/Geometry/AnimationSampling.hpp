/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AnimationSampling.hpp
/// @brief Reads a clip at a time into a pose.
///
/// Sampling writes only what the clip keys, over whatever the pose already
/// holds, so a blend or a mask can build on a pose from somewhere else.

#include <cstdint>
#include <span>
#include <vector>

#include <Assisi/Geometry/AnimationClip.hpp>
#include <Assisi/Geometry/MeshData.hpp>

namespace Assisi::Geometry
{

/// @brief Which skeleton joint each of a clip's tracks drives, by position in
///        AnimationClip::Tracks; kNoJoint (Pose.hpp) for a joint the skeleton
///        lacks.
struct ClipBinding
{
    std::vector<int32_t> JointOfTrack;
};

/// @brief Matches @p clip's tracks to @p skeleton's joints by name.
[[nodiscard]] ClipBinding BindClip(const AnimationClip &clip, const Skeleton &skeleton);

/// @brief @p time brought into a clip of @p duration seconds: wrapped when it
///        loops, in either direction, and clamped to [0, duration] when it
///        does not. 0 for a clip of no length.
[[nodiscard]] float WrapClipTime(float time, float duration, bool loop);

/// @brief Writes @p clip at @p time into @p out, one entry per skeleton joint.
///
/// Only channels with keys are written, and only for tracks bound to a joint;
/// everything else in @p out is left as it was. Before a channel's first key
/// and after its last it holds that key.
void SampleClip(const AnimationClip &clip, const ClipBinding &binding, float time, std::span<JointTransform> out);

} // namespace Assisi::Geometry
