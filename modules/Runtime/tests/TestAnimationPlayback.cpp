/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestAnimationPlayback.cpp
/// @brief An AnimationPlayer moves its clip on by the frame's time at its
/// speed, wraps or holds at the end, writes the joints its clip moves, and
/// starts again from rest only when the clip or the mesh changes.

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Geometry/AnimationClip.hpp>
#include <Assisi/Geometry/AnimationSampling.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Geometry/Pose.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Runtime/AnimationPlayback.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/SkinnedMeshPose.hpp>

using namespace Assisi;
using Geometry::AnimationClip;
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

struct Rig
{
    Skeleton skeleton = Leg();
    SkinnedMesh skinned;
    AnimationPlayer player;
    std::shared_ptr<const AnimationClip> clip = KneeSlide();

    Rig()
    {
        (void)Runtime::BindPose(skinned, skeleton, kMesh);
        player.clip = ClipId(1);
    }

    bool Advance(float dt) { return AdvanceAnimationPlayer(player, clip, skeleton, dt, skinned); }
    float Knee() const { return skinned.pose[1].Translation.x; }
};

} // namespace

TEST_CASE("Animation playback: time moves on by the frame's time times the speed")
{
    Rig rig;
    rig.player.speed = 2.f;
    CHECK(rig.Advance(kFrame));
    CHECK(rig.player.time == doctest::Approx(2.f * kFrame));
    CHECK(rig.Knee() == doctest::Approx(2.f * kFrame));
    CHECK_FALSE(rig.Advance(kFrame));
    CHECK(rig.Knee() == doctest::Approx(4.f * kFrame));
}

TEST_CASE("Animation playback: a looping clip wraps past its end, a one-shot one holds its last frame")
{
    Rig looping;
    (void)looping.Advance(kDuration + kFrame);
    CHECK(looping.player.time == doctest::Approx(kFrame));
    CHECK(looping.Knee() == doctest::Approx(kFrame));

    Rig once;
    once.player.loop = false;
    (void)once.Advance(kDuration + kFrame);
    CHECK(once.player.time == doctest::Approx(kDuration));
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
    rig.clip = KneeSlide();
    CHECK(rig.Advance(kFrame));
    CHECK(rig.player.time == doctest::Approx(3.f * kFrame));

    (void)Runtime::BindPose(rig.skinned, rig.skeleton, kOtherMesh);
    CHECK(rig.Advance(kFrame));
    CHECK(rig.player.time == doctest::Approx(4.f * kFrame));

    rig.player.clip = ClipId(2);
    CHECK(rig.Advance(kFrame));
    CHECK(rig.player.time == doctest::Approx(kFrame));
}

TEST_CASE("Animation playback: a joint the mesh lacks is skipped and the rest still play")
{
    Rig rig;
    CHECK(rig.Advance(kFrame));
    REQUIRE(rig.player.binding.JointOfTrack.size() == 2);
    CHECK(rig.player.binding.JointOfTrack[1] == Geometry::kNoJoint);
    CHECK(rig.Knee() == doctest::Approx(kFrame));
}

TEST_CASE("Animation playback: nothing happens before the mesh is bound")
{
    Rig rig;
    Runtime::UnbindSkinnedMesh(rig.skinned);
    CHECK_FALSE(rig.Advance(kFrame));
    CHECK(rig.skinned.pose.empty());
    CHECK(rig.player.time == doctest::Approx(0.f));
}
