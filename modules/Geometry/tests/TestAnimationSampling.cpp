/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestAnimationSampling.cpp
/// @brief A clip read at a time gives its keys at their times and the straight
/// line or shorter arc between them, holds past its ends, wraps or clamps as
/// told, and drives joints by name.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include <Assisi/Geometry/AnimationClip.hpp>
#include <Assisi/Geometry/AnimationSampling.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Geometry/Pose.hpp>
#include <Assisi/Math/GLM.hpp>

using Assisi::Geometry::AnimationClip;
using Assisi::Geometry::BindClip;
using Assisi::Geometry::ClipBinding;
using Assisi::Geometry::Interpolation;
using Assisi::Geometry::JointTrack;
using Assisi::Geometry::JointTransform;
using Assisi::Geometry::kNoJoint;
using Assisi::Geometry::kNoParent;
using Assisi::Geometry::SampleClip;
using Assisi::Geometry::Skeleton;
using Assisi::Geometry::WrapClipTime;

namespace
{

constexpr float kDuration = 2.f;
const glm::vec3 kUpAxis{0.f, 1.f, 0.f};

/// Two joints, "hip" then "knee".
Skeleton HipAndKnee()
{
    Skeleton skeleton;
    skeleton.Names = {"hip", "knee"};
    skeleton.Parents = {kNoParent, 0};
    skeleton.RestLocal.resize(2);
    skeleton.InverseBind.resize(2, glm::mat4(1.f));
    skeleton.JointBounds.resize(2);
    return skeleton;
}

/// "knee" moving from x = 0 to x = 2 and scaling from 1 to 3 over two seconds,
/// keyed at 0, 1 and 2.
JointTrack KneeTrack()
{
    JointTrack track;
    track.Joint = "knee";
    track.Translation.Times = {0.f, 1.f, 2.f};
    track.Translation.Values = {glm::vec3(0.f), glm::vec3(1.f, 0.f, 0.f), glm::vec3(2.f, 0.f, 0.f)};
    track.Scale.Times = {0.f, 2.f};
    track.Scale.Values = {glm::vec3(1.f), glm::vec3(3.f)};
    return track;
}

AnimationClip ClipOf(std::vector<JointTrack> tracks)
{
    AnimationClip clip;
    clip.Name = "test";
    clip.Tracks = std::move(tracks);
    clip.Duration = kDuration;
    return clip;
}

/// A pose with every joint marked by an unusual translation, so a joint the
/// clip should not touch shows if it was.
std::vector<JointTransform> MarkedPose()
{
    JointTransform marked;
    marked.Translation = {7.f, 7.f, 7.f};
    return std::vector<JointTransform>(2, marked);
}

std::vector<JointTransform> Sample(const AnimationClip &clip, float time)
{
    std::vector<JointTransform> pose = MarkedPose();
    SampleClip(clip, BindClip(clip, HipAndKnee()), time, pose);
    return pose;
}

/// Where @p rotation sends the +X axis.
glm::vec3 TurnedX(const glm::quat &rotation)
{
    return rotation * glm::vec3(1.f, 0.f, 0.f);
}

} // namespace

TEST_CASE("Animation sampling: at a key's time a channel is that key")
{
    const AnimationClip clip = ClipOf({KneeTrack()});
    for (float time : {0.f, 1.f, 2.f})
    {
        CAPTURE(time);
        CHECK(Sample(clip, time)[1].Translation.x == doctest::Approx(time));
    }
    CHECK(Sample(clip, 2.f)[1].Scale.y == doctest::Approx(3.f));
}

TEST_CASE("Animation sampling: between two keys a channel is the straight line between them")
{
    const AnimationClip clip = ClipOf({KneeTrack()});
    const std::vector<JointTransform> pose = Sample(clip, 0.25f);
    CHECK(pose[1].Translation.x == doctest::Approx(0.25f));
    // Scale's keys are two seconds apart, so a quarter second is an eighth of the way.
    CHECK(pose[1].Scale.x == doctest::Approx(1.25f));
}

TEST_CASE("Animation sampling: a rotation between keys turns part of the way")
{
    JointTrack track;
    track.Joint = "knee";
    track.Rotation.Times = {0.f, 2.f};
    track.Rotation.Values = {glm::quat(1.f, 0.f, 0.f, 0.f), glm::angleAxis(glm::half_pi<float>(), kUpAxis)};
    const glm::vec3 x = TurnedX(Sample(ClipOf({track}), 1.f)[1].Rotation);
    // Halfway through a quarter turn about Y is an eighth of a turn: +X towards -Z.
    CHECK(x.x == doctest::Approx(std::cos(glm::quarter_pi<float>())));
    CHECK(x.z == doctest::Approx(-std::sin(glm::quarter_pi<float>())));
}

TEST_CASE("Animation sampling: a rotation takes the shorter way round")
{
    // 200 degrees one way is 160 the other. Halfway the long way faces +X 100
    // degrees round; the short way, 80 degrees back.
    JointTrack track;
    track.Joint = "knee";
    track.Rotation.Times = {0.f, 2.f};
    track.Rotation.Values = {glm::quat(1.f, 0.f, 0.f, 0.f), glm::angleAxis(glm::radians(200.f), kUpAxis)};
    const glm::vec3 x = TurnedX(Sample(ClipOf({track}), 1.f)[1].Rotation);
    const float shortWay = glm::radians(-80.f);
    CHECK(x.x == doctest::Approx(std::cos(shortWay)).epsilon(1e-4));
    CHECK(x.z == doctest::Approx(-std::sin(shortWay)).epsilon(1e-4));
}

TEST_CASE("Animation sampling: a stepped channel holds each key until the next")
{
    JointTrack track = KneeTrack();
    track.Translation.Mode = Interpolation::Step;
    const AnimationClip clip = ClipOf({track});
    CHECK(Sample(clip, 0.9f)[1].Translation.x == doctest::Approx(0.f));
    CHECK(Sample(clip, 1.5f)[1].Translation.x == doctest::Approx(1.f));
}

TEST_CASE("Animation sampling: before the first key and after the last, the end key holds")
{
    JointTrack track = KneeTrack();
    track.Translation.Times = {0.5f, 1.f, 1.5f};
    const AnimationClip clip = ClipOf({track});
    CHECK(Sample(clip, 0.f)[1].Translation.x == doctest::Approx(0.f));
    CHECK(Sample(clip, 1.9f)[1].Translation.x == doctest::Approx(2.f));
}

TEST_CASE("Animation sampling: a looping time wraps in both directions, a one-shot one clamps")
{
    constexpr float kEpsilon = 0.1f;
    CHECK(WrapClipTime(kDuration + kEpsilon, kDuration, true) == doctest::Approx(kEpsilon));
    CHECK(WrapClipTime(-kEpsilon, kDuration, true) == doctest::Approx(kDuration - kEpsilon));
    CHECK(WrapClipTime(3.f * kDuration + kEpsilon, kDuration, true) == doctest::Approx(kEpsilon));
    CHECK(WrapClipTime(kDuration + kEpsilon, kDuration, false) == doctest::Approx(kDuration));
    CHECK(WrapClipTime(-kEpsilon, kDuration, false) == doctest::Approx(0.f));
    CHECK(WrapClipTime(1.f, 0.f, true) == doctest::Approx(0.f));
}

TEST_CASE("Animation sampling: a loop whose ends match is continuous across its seam")
{
    // Out to x = 1 and back. Just before the end and just after the wrap are
    // both a hair from 0, so the joint does not jump when the clip comes round.
    JointTrack track;
    track.Joint = "knee";
    track.Translation.Times = {0.f, 1.f, 2.f};
    track.Translation.Values = {glm::vec3(0.f), glm::vec3(1.f, 0.f, 0.f), glm::vec3(0.f)};
    const AnimationClip clip = ClipOf({track});
    constexpr float kEpsilon = 0.01f;
    const float before = Sample(clip, WrapClipTime(kDuration - kEpsilon, kDuration, true))[1].Translation.x;
    const float after = Sample(clip, WrapClipTime(kDuration + kEpsilon, kDuration, true))[1].Translation.x;
    CHECK(before == doctest::Approx(kEpsilon));
    CHECK(after == doctest::Approx(kEpsilon));
}

TEST_CASE("Animation sampling: tracks drive joints by name, whatever order they come in")
{
    JointTrack hip;
    hip.Joint = "hip";
    hip.Translation.Times = {0.f};
    hip.Translation.Values = {glm::vec3(0.f, 5.f, 0.f)};
    JointTrack tail;
    tail.Joint = "tail";
    tail.Translation.Times = {0.f};
    tail.Translation.Values = {glm::vec3(9.f)};
    const AnimationClip clip = ClipOf({KneeTrack(), tail, hip});

    const ClipBinding binding = BindClip(clip, HipAndKnee());
    REQUIRE(binding.JointOfTrack.size() == 3);
    CHECK(binding.JointOfTrack[0] == 1);
    CHECK(binding.JointOfTrack[1] == kNoJoint);
    CHECK(binding.JointOfTrack[2] == 0);

    const std::vector<JointTransform> pose = Sample(clip, 1.f);
    CHECK(pose[0].Translation.y == doctest::Approx(5.f));
    CHECK(pose[1].Translation.x == doctest::Approx(1.f));
}

TEST_CASE("Animation sampling: what a clip does not key is left as it was")
{
    JointTrack track;
    track.Joint = "knee";
    track.Rotation.Times = {0.f};
    track.Rotation.Values = {glm::angleAxis(1.f, kUpAxis)};
    const std::vector<JointTransform> pose = Sample(ClipOf({track}), 1.f);
    // The knee's translation and scale, and the whole hip, are untouched.
    CHECK(pose[1].Translation.x == doctest::Approx(7.f));
    CHECK(pose[1].Scale.x == doctest::Approx(1.f));
    CHECK(pose[0].Translation.x == doctest::Approx(7.f));
    CHECK(pose[1].Rotation.w == doctest::Approx(std::cos(0.5f)));
}
