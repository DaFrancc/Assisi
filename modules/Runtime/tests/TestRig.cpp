/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestRig.cpp
/// @brief The characters TestRig.hpp builds: the joints asked for, limbs on the
/// ground whichever way their bones point, a mesh that skins to nothing at
/// rest, clips with exact numbers, and a character that binds and poses with
/// no renderer.

#include "TestRig.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Geometry/AnimationSampling.hpp>
#include <Assisi/Geometry/Pose.hpp>
#include <Assisi/Render/MeshBuffer.hpp>
#include <Assisi/Runtime/AnimationPlayback.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/SkinnedMeshPose.hpp>

using namespace Assisi;
using namespace Assisi::Runtime::Testing;

namespace
{

constexpr uint32_t kMeshId = 1;
constexpr float kEpsilon = 1e-5f;

/// Every rig the tests cover: both shapes, both segment counts, both axes.
std::vector<RigShape> EveryRig()
{
    std::vector<RigShape> rigs;
    for (const BoneAxis axis : {BoneAxis::PlusY, BoneAxis::PlusX})
    {
        for (const uint32_t segments : {2u, 3u})
        {
            rigs.push_back(TwoLimbs(segments, axis));
            rigs.push_back(FourLimbs(segments, axis));
        }
    }
    return rigs;
}

bool Near(const glm::vec3 &a, const glm::vec3 &b)
{
    return glm::all(glm::lessThan(glm::abs(a - b), glm::vec3(kEpsilon)));
}

/// @p clip's pose on @p skeleton at @p time, from the rest pose.
std::vector<Geometry::JointTransform> Sample(const Geometry::AnimationClip &clip, const Geometry::Skeleton &skeleton,
                                             float time)
{
    std::vector<Geometry::JointTransform> pose = skeleton.RestLocal;
    Geometry::SampleClip(clip, Geometry::BindClip(clip, skeleton), time, pose);
    return pose;
}

} // namespace

TEST_CASE("Test rig: a skeleton has the joints its shape asks for, with any number of limbs and segments")
{
    const Geometry::Skeleton biped = BuildSkeleton(TwoLimbs(2));
    CHECK(biped.JointCount() == 8);
    CHECK(Geometry::FindJoint(biped, "limb_l_0") != Geometry::kNoJoint);
    CHECK(Geometry::FindJoint(biped, "limb_r_1") != Geometry::kNoJoint);
    CHECK(Geometry::FindJoint(biped, "limb_r_end") != Geometry::kNoJoint);
    CHECK(Geometry::FindJoint(biped, "limb_r_2") == Geometry::kNoJoint);

    const RigShape quadruped = FourLimbs(3);
    const Geometry::Skeleton skeleton = BuildSkeleton(quadruped);
    CHECK(skeleton.JointCount() == JointCountOf(quadruped));
    CHECK(skeleton.JointCount() == 18);
    CHECK(Geometry::FindJoint(skeleton, "fore_l_2") != Geometry::kNoJoint);
    CHECK(Geometry::FindJoint(skeleton, "hind_r_end") != Geometry::kNoJoint);
    CHECK(skeleton.InverseBind.size() == skeleton.JointCount());
    CHECK(skeleton.JointBounds.size() == skeleton.JointCount());
}

TEST_CASE("Test rig: every limb reaches the ground under where it hangs, whichever way its bones point")
{
    for (const RigShape &rig : EveryRig())
    {
        const Geometry::Skeleton skeleton = BuildSkeleton(rig);
        for (const LimbShape &limb : rig.limbs)
        {
            CAPTURE(limb.name);
            const glm::vec3 end = RestPosition(skeleton, limb.name + "_end");
            CHECK(Near(end, glm::vec3(limb.attach.x, 0.f, limb.attach.z)));
        }
    }
}

TEST_CASE("Test rig: a limb's bones point along the axis asked for, in their own frames")
{
    const Geometry::Skeleton alongY = BuildSkeleton(TwoLimbs(2, BoneAxis::PlusY));
    const Geometry::Skeleton alongX = BuildSkeleton(TwoLimbs(2, BoneAxis::PlusX));
    const std::size_t limbMiddle = static_cast<std::size_t>(Geometry::FindJoint(alongY, "limb_l_1"));
    CHECK(Near(alongY.RestLocal[limbMiddle].Translation, glm::vec3(0.f, kSegmentLength, 0.f)));
    CHECK(Near(alongX.RestLocal[limbMiddle].Translation, glm::vec3(kSegmentLength, 0.f, 0.f)));
    // Different frames, the same place: only the convention changed.
    CHECK(Near(RestPosition(alongY, "limb_l_1"), RestPosition(alongX, "limb_l_1")));
}

TEST_CASE("Test rig: clips give their numbers exactly at the times they state")
{
    const Geometry::Skeleton skeleton = BuildSkeleton(FourLimbs());
    constexpr float kSeconds = 2.f;
    constexpr float kDegrees = 30.f;
    const std::size_t limbTop = static_cast<std::size_t>(Geometry::FindJoint(skeleton, "fore_l_0"));
    const std::size_t body = static_cast<std::size_t>(kBodyJoint);

    // A cycle: at a quarter it has turned the whole angle, at three quarters the other way.
    const std::shared_ptr<const Geometry::AnimationClip> cycle =
        CycleClip("fore_l_0", glm::vec3(1.f, 0.f, 0.f), kDegrees, kSeconds);
    CHECK(glm::degrees(glm::angle(Sample(*cycle, skeleton, kSeconds / 4.f)[limbTop].Rotation)) ==
          doctest::Approx(kDegrees));
    CHECK(glm::degrees(glm::angle(Sample(*cycle, skeleton, kSeconds / 2.f)[limbTop].Rotation)) ==
          doctest::Approx(0.f).epsilon(kEpsilon));
    CHECK(glm::axis(Sample(*cycle, skeleton, 3.f * kSeconds / 4.f)[limbTop].Rotation).x == doctest::Approx(-1.f));

    // A move: the stated distance in the stated time, a straight line between.
    const glm::vec3 from = skeleton.RestLocal[body].Translation;
    const glm::vec3 distance{0.f, 0.f, 3.f};
    const std::shared_ptr<const Geometry::AnimationClip> move = MoveClip("body", from, distance, kSeconds);
    CHECK(Near(Sample(*move, skeleton, kSeconds)[body].Translation, from + distance));
    CHECK(Near(Sample(*move, skeleton, kSeconds / 2.f)[body].Translation, from + distance * 0.5f));

    // A held pose: the same all the way through.
    Geometry::JointTransform pose;
    pose.Translation = {0.f, 2.f, 0.f};
    const std::shared_ptr<const Geometry::AnimationClip> held = HeldClip("body", pose, kSeconds);
    CHECK(Near(Sample(*held, skeleton, 0.f)[body].Translation, pose.Translation));
    CHECK(Near(Sample(*held, skeleton, kSeconds)[body].Translation, pose.Translation));
}

TEST_CASE("Test rig: a four-limbed character binds, plays and poses with no renderer")
{
    const RigShape rig = FourLimbs(3, BoneAxis::PlusX);
    Geometry::MeshData boxes = BuildSkinnedBoxes(BuildSkeleton(rig));
    CHECK(boxes.Vertices.size() == JointCountOf(rig) * kBoxCorners);
    CHECK(boxes.Skin.size() == boxes.Vertices.size());
    Render::MeshBuffer mesh;
    mesh.KeepWithoutGpu(std::move(boxes));
    mesh.SetId(kMeshId);

    ECS::Scene scene;
    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add(entity, Runtime::SkinnedMesh{}) != nullptr);
    scene.Get<Runtime::MeshRenderer>(entity)->meshBuffer = &mesh;
    Runtime::SkinnedPose &skinned = *scene.Get<Runtime::SkinnedPose>(entity);
    const Runtime::MeshRenderer &renderer = *scene.Get<Runtime::MeshRenderer>(entity);

    // At rest skinning moves nothing: every joint is where its inverse bind undoes.
    Runtime::EvaluateSkinnedMesh(skinned, renderer);
    REQUIRE(skinned.palette.size() == JointCountOf(rig));
    for (const glm::mat4 &joint : skinned.palette)
    {
        CHECK(Near(glm::vec3(joint * glm::vec4(1.f, 2.f, 3.f, 1.f)), glm::vec3(1.f, 2.f, 3.f)));
    }

    // The body moves a metre forward over a second; everything hanging from it follows.
    const Geometry::Skeleton &skeleton = mesh.Skeleton();
    const glm::vec3 forward{0.f, 0.f, 1.f};
    const std::vector<std::shared_ptr<const Geometry::AnimationClip>> clips{
        MoveClip("body", skeleton.RestLocal[kBodyJoint].Translation, forward, 1.f)};
    Runtime::AnimationPlayer player;
    Runtime::AnimationPlayback playback;
    player.animation.bytes[0] = 1;
    player.loop = false;
    Runtime::RequestFromAuthored(player, playback.request);
    (void)Runtime::AdvanceAnimationPlayer(playback, Runtime::ResolvedAnimation{.space = nullptr, .clips = clips},
                                          skeleton, 1.f, skinned);
    Runtime::EvaluateSkinnedMesh(skinned, renderer);

    const std::size_t limbEnd = static_cast<std::size_t>(Geometry::FindJoint(skeleton, "hind_r_end"));
    CHECK(Near(glm::vec3(skinned.jointModel[kBodyJoint][3]), RestPosition(skeleton, "body") + forward));
    CHECK(Near(glm::vec3(skinned.jointModel[limbEnd][3]), RestPosition(skeleton, "hind_r_end") + forward));
}
