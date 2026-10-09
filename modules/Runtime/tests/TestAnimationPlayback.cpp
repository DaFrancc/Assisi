/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestAnimationPlayback.cpp
/// @brief An AnimationPlayer moves its animation on by the frame's time at its
/// speed, wraps or holds at the end, writes the joints its clips move, mixes a
/// blend space's clips by its parameter, fades from the pose on screen when the
/// animation changes, and starts again from rest only when the animation or the
/// mesh changes without a fade.

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Geometry/AnimationClip.hpp>
#include <Assisi/Geometry/AnimationSampling.hpp>
#include <Assisi/Geometry/BlendSpace.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Geometry/Pose.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Runtime/AnimationPlayback.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/SkinnedMeshPose.hpp>

using namespace Assisi;
using Geometry::AnimationClip;
using Geometry::BlendPoint;
using Geometry::BlendSpace;
using Geometry::JointTrack;
using Geometry::Skeleton;
using Runtime::AdvanceAnimationPlayer;
using Runtime::AnimationPlayer;
using Runtime::SkinnedMesh;

namespace
{

constexpr uint32_t kMesh = 1;
constexpr uint32_t kOtherMesh = 2;
constexpr float kDuration = 2.f;
constexpr float kFrame = 0.25f;

/// "hip", "knee" and "toe"; at rest the toe sits one unit along Z.
Skeleton Leg()
{
    Skeleton skeleton;
    skeleton.Names = {"hip", "knee", "toe"};
    skeleton.Parents = {Geometry::kNoParent, 0, 1};
    skeleton.RestLocal.resize(3);
    skeleton.RestLocal[2].Translation = {0.f, 0.f, 1.f};
    skeleton.InverseBind.resize(3, glm::mat4(1.f));
    skeleton.JointBounds.resize(3);
    return skeleton;
}

/// The knee moving from x = 0 to x = 2 over two seconds, and a joint the leg lacks.
std::shared_ptr<const AnimationClip> KneeSlide()
{
    AnimationClip clip;
    clip.Name = "slide";
    JointTrack knee;
    knee.Joint = "knee";
    knee.Translation.Times = {0.f, kDuration};
    knee.Translation.Values = {glm::vec3(0.f), glm::vec3(2.f, 0.f, 0.f)};
    JointTrack tail;
    tail.Joint = "tail";
    tail.Translation.Times = {0.f};
    tail.Translation.Values = {glm::vec3(5.f)};
    clip.Tracks = {knee, tail};
    clip.Duration = kDuration;
    return std::make_shared<const AnimationClip>(std::move(clip));
}

/// A clip id told apart by its last byte, outside the reserved built-in range.
Core::AssetId ClipId(uint8_t last)
{
    Core::AssetId id;
    id.bytes[0] = 0xAA;
    id.bytes[15] = last;
    return id;
}

/// The knee held at x = @p x, over a clip of @p duration seconds.
std::shared_ptr<const AnimationClip> KneeHeld(float x, float duration)
{
    AnimationClip clip;
    clip.Name = "held";
    JointTrack knee;
    knee.Joint = "knee";
    knee.Translation.Times = {0.f};
    knee.Translation.Values = {glm::vec3(x, 0.f, 0.f)};
    clip.Tracks = {knee};
    clip.Duration = duration;
    return std::make_shared<const AnimationClip>(std::move(clip));
}

/// A held knee at (0, 0) and KneeSlide at (2, 0): a 1D space along x.
std::shared_ptr<const BlendSpace> StandToSlide()
{
    BlendSpace space;
    BlendPoint stand;
    stand.Clip = ClipId(10);
    stand.Position = {0.f, 0.f};
    BlendPoint slide;
    slide.Clip = ClipId(11);
    slide.Position = {2.f, 0.f};
    space.Points = {stand, slide};
    return std::make_shared<const BlendSpace>(std::move(space));
}

struct Rig
{
    Skeleton skeleton = Leg();
    SkinnedMesh skinned;
    AnimationPlayer player;
    std::vector<std::shared_ptr<const AnimationClip>> clips{KneeSlide()};
    std::shared_ptr<const BlendSpace> space;

    Rig()
    {
        (void)Runtime::BindPose(skinned, skeleton, kMesh);
        player.animation = ClipId(1);
    }

    /// Plays @p clip from now on, as another animation.
    void Play(std::shared_ptr<const AnimationClip> clip, uint8_t id)
    {
        clips = {std::move(clip)};
        space = nullptr;
        player.animation = ClipId(id);
    }

    bool Advance(float dt)
    {
        const Runtime::ResolvedAnimation animation{.space = space, .clips = clips};
        return AdvanceAnimationPlayer(player, animation, skeleton, dt, skinned);
    }
    float Knee() const { return skinned.pose[1].Translation.x; }
    float Time() const { return player.current.Phase * kDuration; }
};

} // namespace

TEST_CASE("Animation playback: time moves on by the frame's time times the speed")
{
    Rig rig;
    rig.player.speed = 2.f;
    CHECK(rig.Advance(kFrame));
    CHECK(rig.Time() == doctest::Approx(2.f * kFrame));
    CHECK(rig.Knee() == doctest::Approx(2.f * kFrame));
    CHECK_FALSE(rig.Advance(kFrame));
    CHECK(rig.Knee() == doctest::Approx(4.f * kFrame));
}

TEST_CASE("Animation playback: a looping clip wraps past its end, a one-shot one holds its last frame")
{
    Rig looping;
    (void)looping.Advance(kDuration + kFrame);
    CHECK(looping.Time() == doctest::Approx(kFrame));
    CHECK(looping.Knee() == doctest::Approx(kFrame));

    Rig once;
    once.player.loop = false;
    (void)once.Advance(kDuration + kFrame);
    CHECK(once.Time() == doctest::Approx(kDuration));
    CHECK(once.Knee() == doctest::Approx(2.f));
    (void)once.Advance(kFrame);
    CHECK(once.Knee() == doctest::Approx(2.f));
}

TEST_CASE("Animation playback: binding puts the pose at rest, and joints the clip leaves alone stay as code sets them")
{
    Rig rig;
    // Left over from before the clip started: it must not show.
    rig.skinned.pose[2].Translation = {9.f, 9.f, 9.f};
    (void)rig.Advance(kFrame);
    CHECK(rig.skinned.pose[2].Translation.z == doctest::Approx(1.f));
    CHECK(rig.skinned.pose[2].Translation.x == doctest::Approx(0.f));

    // Set by code while the clip plays: the clip does not move the toe, so it stays.
    rig.skinned.pose[2].Translation = {0.f, 3.f, 0.f};
    (void)rig.Advance(kFrame);
    CHECK(rig.skinned.pose[2].Translation.y == doctest::Approx(3.f));
}

TEST_CASE("Animation playback: another clip starts from the top; a reload or another mesh rebinds and keeps the time")
{
    Rig rig;
    (void)rig.Advance(kFrame);
    (void)rig.Advance(kFrame);

    // The same clip loaded again, as after an asset reload.
    rig.clips = {KneeSlide()};
    CHECK(rig.Advance(kFrame));
    CHECK(rig.Time() == doctest::Approx(3.f * kFrame));

    (void)Runtime::BindPose(rig.skinned, rig.skeleton, kOtherMesh);
    CHECK(rig.Advance(kFrame));
    CHECK(rig.Time() == doctest::Approx(4.f * kFrame));

    rig.player.animation = ClipId(2);
    CHECK(rig.Advance(kFrame));
    CHECK(rig.Time() == doctest::Approx(kFrame));
}

TEST_CASE("Animation playback: a joint the mesh lacks is skipped and the rest still play")
{
    Rig rig;
    CHECK(rig.Advance(kFrame));
    REQUIRE(rig.player.current.Sources.size() == 1);
    REQUIRE(rig.player.current.Sources[0].Binding.JointOfTrack.size() == 2);
    CHECK(rig.player.current.Sources[0].Binding.JointOfTrack[1] == Geometry::kNoJoint);
    CHECK(rig.Knee() == doctest::Approx(kFrame));
}

TEST_CASE("Animation playback: nothing happens before the mesh is bound")
{
    Rig rig;
    Runtime::UnbindSkinnedMesh(rig.skinned);
    CHECK_FALSE(rig.Advance(kFrame));
    CHECK(rig.skinned.pose.empty());
    CHECK(rig.Time() == doctest::Approx(0.f));
}

TEST_CASE("Animation playback: a blend space mixes its clips by the parameter, in step")
{
    Rig rig;
    rig.space = StandToSlide();
    rig.clips = {KneeHeld(0.f, kDuration), KneeSlide()};
    rig.player.parameter = {1.f, 0.f};
    CHECK(rig.Advance(2.f * kFrame));
    // Halfway between standing and a slide half a second in.
    CHECK(rig.Knee() == doctest::Approx(0.5f * 2.f * kFrame));

    rig.player.parameter = {2.f, 0.f};
    CHECK_FALSE(rig.Advance(2.f * kFrame));
    CHECK(rig.Knee() == doctest::Approx(4.f * kFrame));
}

TEST_CASE("Animation playback: a blend space waits until every one of its clips has loaded")
{
    Rig rig;
    rig.space = StandToSlide();
    rig.clips = {KneeHeld(0.f, kDuration), nullptr};
    rig.skinned.pose[1].Translation = {9.f, 0.f, 0.f};
    CHECK_FALSE(rig.Advance(kFrame));
    CHECK(rig.Knee() == doctest::Approx(9.f));
}

TEST_CASE("Animation playback: a fade mixes the outgoing clip, still playing, into the new one over its seconds")
{
    Rig rig;
    (void)rig.Advance(4.f * kFrame);
    REQUIRE(rig.Knee() == doctest::Approx(1.f));

    rig.player.fade = 1.f;
    rig.Play(KneeHeld(10.f, 1.f), 2);
    (void)rig.Advance(kFrame);
    // A quarter of the way to 10 from the slide, which has moved on to 1.25.
    CHECK(rig.Knee() == doctest::Approx(0.75f * 1.25f + 0.25f * 10.f));
    (void)rig.Advance(kFrame);
    CHECK(rig.Knee() == doctest::Approx(0.5f * 1.5f + 0.5f * 10.f));
    (void)rig.Advance(2.f * kFrame);
    CHECK(rig.Knee() == doctest::Approx(10.f));
    CHECK_FALSE(rig.player.fading);
    CHECK(rig.player.outgoing.Sources.empty());
}

TEST_CASE("Animation playback: a fade interrupted by another starts from the pose it had reached")
{
    Rig rig;
    (void)rig.Advance(4.f * kFrame);
    rig.player.fade = 1.f;
    rig.Play(KneeHeld(10.f, 1.f), 2);
    (void)rig.Advance(2.f * kFrame);
    const float reached = rig.Knee();
    REQUIRE(reached == doctest::Approx(0.5f * 1.5f + 0.5f * 10.f));

    rig.Play(KneeHeld(20.f, 1.f), 3);
    (void)rig.Advance(0.f);
    CHECK(rig.Knee() == doctest::Approx(reached));
    (void)rig.Advance(2.f * kFrame);
    CHECK(rig.Knee() == doctest::Approx(0.5f * reached + 0.5f * 20.f));
}

TEST_CASE("Animation playback: a blend space fading out keeps playing at the weights it had")
{
    Rig rig;
    rig.space = StandToSlide();
    rig.clips = {KneeHeld(0.f, kDuration), KneeSlide()};
    rig.player.parameter = {1.f, 0.f};
    (void)rig.Advance(2.f * kFrame);

    rig.player.fade = 1.f;
    rig.Play(KneeHeld(10.f, 1.f), 2);
    (void)rig.Advance(kFrame);
    // The space, three quarters of a second in: half of standing and half of
    // the slide at 0.75.
    const float space = 0.5f * 3.f * kFrame;
    CHECK(rig.Knee() == doctest::Approx(0.75f * space + 0.25f * 10.f));
}

TEST_CASE("Animation playback: another mesh during a fade ends it and starts from rest")
{
    Rig rig;
    (void)rig.Advance(4.f * kFrame);
    rig.player.fade = 1.f;
    rig.Play(KneeHeld(10.f, 1.f), 2);
    (void)rig.Advance(kFrame);
    REQUIRE(rig.player.fading);

    (void)Runtime::BindPose(rig.skinned, rig.skeleton, kOtherMesh);
    CHECK(rig.Advance(kFrame));
    CHECK_FALSE(rig.player.fading);
    CHECK(rig.Knee() == doctest::Approx(10.f));
}

TEST_CASE("Animation playback: with no fade a new animation cuts straight to itself")
{
    Rig rig;
    (void)rig.Advance(4.f * kFrame);
    rig.Play(KneeHeld(10.f, 1.f), 2);
    (void)rig.Advance(kFrame);
    CHECK(rig.Knee() == doctest::Approx(10.f));
    CHECK_FALSE(rig.player.fading);
}
