/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Geometry/AnimationSampling.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Geometry/Pose.hpp>

#include <cmath>
#include <cstddef>

namespace Assisi::Geometry
{

namespace
{

/// Where a time falls among a channel's keys: the key at or before it, and how
/// far towards the next one it is, from 0 to 1.
struct Segment
{
    std::size_t first = 0;
    float along = 0.f;
};

/// The segment of @p times holding @p time, held at the first and last keys.
/// @p times has at least two keys.
Segment Locate(std::span<const float> times, float time)
{
    const std::size_t last = times.size() - 1;
    if (time <= times[0])
    {
        return Segment{.first = 0, .along = 0.f};
    }
    if (time >= times[last])
    {
        return Segment{.first = last - 1, .along = 1.f};
    }
    std::size_t low = 0;
    std::size_t high = last;
    while (high - low > 1)
    {
        const std::size_t middle = (low + high) / 2;
        if (times[middle] <= time)
        {
            low = middle;
        }
        else
        {
            high = middle;
        }
    }
    return Segment{.first = low, .along = (time - times[low]) / (times[high] - times[low])};
}

glm::vec3 Blend(const glm::vec3 &from, const glm::vec3 &to, float along)
{
    return glm::mix(from, to, along);
}

/// Along the shorter arc: q and -q are the same rotation, and glm::slerp turns
/// towards whichever of them is nearer. glm::mix does not, and goes the long
/// way round half the time.
glm::quat Blend(const glm::quat &from, const glm::quat &to, float along)
{
    return glm::normalize(glm::slerp(from, to, along));
}

template <typename T> T SampleChannel(const Channel<T> &channel, float time)
{
    ASSISI_ASSERT(channel.Times.size() == channel.Values.size(), "a value per key time");
    if (channel.Times.size() == 1)
    {
        return channel.Values[0];
    }
    const Segment segment = Locate(channel.Times, time);
    if (channel.Mode == Interpolation::Step)
    {
        return segment.along < 1.f ? channel.Values[segment.first] : channel.Values[segment.first + 1];
    }
    return Blend(channel.Values[segment.first], channel.Values[segment.first + 1], segment.along);
}

} // namespace

ClipBinding BindClip(const AnimationClip &clip, const Skeleton &skeleton)
{
    ClipBinding binding;
    binding.JointOfTrack.reserve(clip.Tracks.size());
    for (const JointTrack &track : clip.Tracks)
    {
        binding.JointOfTrack.push_back(FindJoint(skeleton, track.Joint));
    }
    return binding;
}

float WrapClipTime(float time, float duration, bool loop)
{
    if (duration <= 0.f)
    {
        return 0.f;
    }
    if (!loop)
    {
        return std::fmin(std::fmax(time, 0.f), duration);
    }
    // Floor, not fmod: fmod keeps the sign, and a clip played backwards would
    // leave its range below zero.
    return time - std::floor(time / duration) * duration;
}

void SampleClip(const AnimationClip &clip, const ClipBinding &binding, float time, std::span<JointTransform> out)
{
    ASSISI_ASSERT(binding.JointOfTrack.size() == clip.Tracks.size(), "the binding was made for this clip");
    for (std::size_t track = 0; track < clip.Tracks.size(); ++track)
    {
        const int32_t joint = binding.JointOfTrack[track];
        if (joint == kNoJoint || static_cast<std::size_t>(joint) >= out.size())
        {
            continue;
        }
        const JointTrack &keys = clip.Tracks[track];
        JointTransform &local = out[static_cast<std::size_t>(joint)];
        if (!keys.Rotation.Empty())
        {
            local.Rotation = SampleChannel(keys.Rotation, time);
        }
        if (!keys.Translation.Empty())
        {
            local.Translation = SampleChannel(keys.Translation, time);
        }
        if (!keys.Scale.Empty())
        {
            local.Scale = SampleChannel(keys.Scale, time);
        }
    }
}

} // namespace Assisi::Geometry
