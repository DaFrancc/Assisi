/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestAnimationBlend.cpp
/// @brief Blend weights are the straight mix of neighbours on a line and stay
/// within the points off it; clips in a blend keep in step by one phase;
/// opposite-signed rotations mix as the rotation they both are; and a fade's
/// weight runs from 0 to 1 over its length.

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

#include <Assisi/Geometry/AnimationBlend.hpp>
#include <Assisi/Geometry/AnimationClip.hpp>
#include <Assisi/Geometry/AnimationSampling.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Geometry/Pose.hpp>
#include <Assisi/Math/GLM.hpp>

using Assisi::Geometry::AdvancePhase;
using Assisi::Geometry::AnimationClip;
using Assisi::Geometry::BindClip;
using Assisi::Geometry::BlendSource;
using Assisi::Geometry::BlendWeights;
using Assisi::Geometry::ClipBlend;
using Assisi::Geometry::CrossFade;
using Assisi::Geometry::JointTrack;
using Assisi::Geometry::JointTransform;
using Assisi::Geometry::kNoParent;
using Assisi::Geometry::MixPoses;
using Assisi::Geometry::SampleBlend;
using Assisi::Geometry::Skeleton;

namespace
{

const glm::vec3 kUpAxis{0.f, 0.f, 1.f};

/// "knee" then "toe"; at rest the toe sits one unit along Z.
Skeleton KneeAndToe()
{
    Skeleton skeleton;
    skeleton.Names = {"knee", "toe"};
    skeleton.Parents = {kNoParent, 0};
    skeleton.RestLocal.resize(2);
    skeleton.RestLocal[1].Translation = {0.f, 0.f, 1.f};
    skeleton.InverseBind.resize(2, glm::mat4(1.f));
    skeleton.JointBounds.resize(2);
    return skeleton;
}

/// The knee moving from x = 0 to x = 1 over @p duration seconds.
std::shared_ptr<const AnimationClip> KneeSlide(float duration)
{
    AnimationClip clip;
    clip.Name = "slide";
    JointTrack knee;
    knee.Joint = "knee";
    knee.Translation.Times = {0.f, duration};
    knee.Translation.Values = {glm::vec3(0.f), glm::vec3(1.f, 0.f, 0.f)};
    clip.Tracks = {knee};
    clip.Duration = duration;
    return std::make_shared<const AnimationClip>(std::move(clip));
}

/// The knee turned by @p rotation and moved to @p translation, held for a second.
std::shared_ptr<const AnimationClip> KneeHeld(const glm::quat &rotation, const glm::vec3 &translation)
{
    AnimationClip clip;
    clip.Name = "held";
    JointTrack knee;
    knee.Joint = "knee";
    knee.Rotation.Times = {0.f};
    knee.Rotation.Values = {rotation};
    knee.Translation.Times = {0.f};
    knee.Translation.Values = {translation};
    clip.Tracks = {knee};
    clip.Duration = 1.f;
    return std::make_shared<const AnimationClip>(std::move(clip));
}

ClipBlend BlendOf(std::vector<std::shared_ptr<const AnimationClip>> clips, std::vector<float> weights,
                  const Skeleton &skeleton)
{
    ClipBlend blend;
    for (std::shared_ptr<const AnimationClip> &clip : clips)
    {
        BlendSource source;
        source.Binding = BindClip(*clip, skeleton);
        source.Clip = std::move(clip);
        blend.Sources.push_back(std::move(source));
    }
    blend.Weights = std::move(weights);
    return blend;
}

glm::quat AboutUp(float radians)
{
    return glm::angleAxis(radians, kUpAxis);
}

} // namespace

TEST_CASE("Blend weights: points on a line mix the two neighbours around the parameter")
{
    const std::array<glm::vec2, 3> positions{glm::vec2(0.f, 0.f), glm::vec2(2.f, 0.f), glm::vec2(6.f, 0.f)};
    std::array<float, 3> weights{};

    BlendWeights(positions, glm::vec2(1.f, 0.f), weights);
    CHECK(weights[0] == doctest::Approx(0.5f));
    CHECK(weights[1] == doctest::Approx(0.5f));
    CHECK(weights[2] == doctest::Approx(0.f));

    BlendWeights(positions, glm::vec2(5.f, 0.f), weights);
    CHECK(weights[0] == doctest::Approx(0.f));
    CHECK(weights[1] == doctest::Approx(0.25f));
    CHECK(weights[2] == doctest::Approx(0.75f));

    // Off the line the parameter counts as where it stands along it.
    BlendWeights(positions, glm::vec2(1.f, 3.f), weights);
    CHECK(weights[0] == doctest::Approx(0.5f));
    CHECK(weights[1] == doctest::Approx(0.5f));
}

TEST_CASE("Blend weights: on a point it has them all, and past the ends the end point does")
{
    const std::array<glm::vec2, 3> positions{glm::vec2(6.f, 0.f), glm::vec2(0.f, 0.f), glm::vec2(2.f, 0.f)};
    std::array<float, 3> weights{};

    BlendWeights(positions, glm::vec2(2.f, 0.f), weights);
    CHECK(weights[2] == doctest::Approx(1.f));

    BlendWeights(positions, glm::vec2(-5.f, 0.f), weights);
    CHECK(weights[1] == doctest::Approx(1.f));

    BlendWeights(positions, glm::vec2(9.f, 0.f), weights);
    CHECK(weights[0] == doctest::Approx(1.f));
}

TEST_CASE("Blend weights: four corners share the centre, and anywhere outside the weights still sum to 1")
{
    const std::array<glm::vec2, 4> corners{glm::vec2(-1.f, -1.f), glm::vec2(1.f, -1.f), glm::vec2(1.f, 1.f),
                                           glm::vec2(-1.f, 1.f)};
    std::array<float, 4> weights{};

    BlendWeights(corners, glm::vec2(0.f, 0.f), weights);
    for (const float weight : weights)
    {
        CHECK(weight == doctest::Approx(0.25f));
    }

    BlendWeights(corners, glm::vec2(7.f, -3.f), weights);
    float total = 0.f;
    for (const float weight : weights)
    {
        CHECK(weight >= 0.f);
        total += weight;
    }
    CHECK(total == doctest::Approx(1.f));
}

TEST_CASE("Blend weights: a single point always has all of it")
{
    const std::array<glm::vec2, 1> positions{glm::vec2(3.f, 4.f)};
    std::array<float, 1> weights{};
    BlendWeights(positions, glm::vec2(-8.f, 1.f), weights);
    CHECK(weights[0] == doctest::Approx(1.f));
}

TEST_CASE("Blend phase: moves on by the frame over the weighted average length, wrapping or holding")
{
    const Skeleton skeleton = KneeAndToe();
    ClipBlend blend = BlendOf({KneeSlide(1.f), KneeSlide(3.f)}, {0.5f, 0.5f}, skeleton);

    AdvancePhase(blend, 1.f, true);
    CHECK(blend.Phase == doctest::Approx(0.5f));
    AdvancePhase(blend, 1.5f, true);
    CHECK(blend.Phase == doctest::Approx(0.25f));

    blend.Phase = 0.5f;
    AdvancePhase(blend, 1.5f, false);
    CHECK(blend.Phase == doctest::Approx(1.f));
}

TEST_CASE("Blend phase: a blend of no length stays where it is")
{
    const Skeleton skeleton = KneeAndToe();
    ClipBlend blend = BlendOf({KneeSlide(0.f)}, {1.f}, skeleton);
    blend.Phase = 0.25f;
    AdvancePhase(blend, 1.f, true);
    CHECK(blend.Phase == doctest::Approx(0.25f));
}

TEST_CASE("Blend sampling: clips of different lengths are read at the same point of their cycles")
{
    const Skeleton skeleton = KneeAndToe();
    ClipBlend blend = BlendOf({KneeSlide(1.f), KneeSlide(2.f)}, {0.5f, 0.5f}, skeleton);
    blend.Phase = 0.5f;
    std::vector<JointTransform> pose = skeleton.RestLocal;
    SampleBlend(blend, skeleton, pose);
    // Both halfway: 0.5. Read at one time in seconds instead, 0.75 s, they
    // would be at 0.75 and 0.375.
    CHECK(pose[0].Translation.x == doctest::Approx(0.5f));
}

TEST_CASE("Blend sampling: a rotation and its negative mix as the rotation they both are")
{
    const Skeleton skeleton = KneeAndToe();
    const glm::quat turned = AboutUp(1.f);
    ClipBlend blend = BlendOf({KneeHeld(turned, glm::vec3(0.f)), KneeHeld(-turned, glm::vec3(0.f))}, {0.5f, 0.5f},
                              skeleton);
    std::vector<JointTransform> pose = skeleton.RestLocal;
    SampleBlend(blend, skeleton, pose);
    CHECK(std::abs(glm::dot(pose[0].Rotation, turned)) == doctest::Approx(1.f));
}

TEST_CASE("Blend sampling: halfway between no turn and a quarter turn is an eighth")
{
    const Skeleton skeleton = KneeAndToe();
    const float quarter = std::numbers::pi_v<float> / 2.f;
    ClipBlend blend = BlendOf({KneeHeld(AboutUp(0.f), glm::vec3(0.f)), KneeHeld(AboutUp(quarter), glm::vec3(0.f))},
                              {0.5f, 0.5f}, skeleton);
    std::vector<JointTransform> pose = skeleton.RestLocal;
    SampleBlend(blend, skeleton, pose);
    CHECK(std::abs(glm::dot(pose[0].Rotation, AboutUp(quarter / 2.f))) == doctest::Approx(1.f));
}

TEST_CASE("Blend sampling: a source at no weight leaves nothing of itself")
{
    const Skeleton skeleton = KneeAndToe();
    const glm::quat turned = AboutUp(0.7f);
    ClipBlend blend = BlendOf({KneeHeld(turned, glm::vec3(1.f, 2.f, 3.f)), KneeHeld(AboutUp(2.f), glm::vec3(9.f))},
                              {1.f, 0.f}, skeleton);
    std::vector<JointTransform> pose = skeleton.RestLocal;
    SampleBlend(blend, skeleton, pose);
    CHECK(pose[0].Translation.x == doctest::Approx(1.f));
    CHECK(pose[0].Translation.y == doctest::Approx(2.f));
    CHECK(pose[0].Translation.z == doctest::Approx(3.f));
    CHECK(std::abs(glm::dot(pose[0].Rotation, turned)) == doctest::Approx(1.f));
}

TEST_CASE("Blend sampling: a joint no source keys keeps what the pose held")
{
    const Skeleton skeleton = KneeAndToe();
    ClipBlend blend = BlendOf({KneeSlide(1.f), KneeSlide(2.f)}, {0.5f, 0.5f}, skeleton);
    std::vector<JointTransform> pose = skeleton.RestLocal;
    pose[1].Translation = {0.f, 7.f, 0.f};
    SampleBlend(blend, skeleton, pose);
    CHECK(pose[1].Translation.y == doctest::Approx(7.f));
}

TEST_CASE("Mixing poses: a share of the way from one to the other, rotations by the shorter arc")
{
    const float quarter = std::numbers::pi_v<float> / 2.f;
    std::vector<JointTransform> from(1);
    std::vector<JointTransform> to(1);
    from[0].Translation = {0.f, 0.f, 0.f};
    to[0].Translation = {4.f, 0.f, 0.f};
    to[0].Rotation = -AboutUp(quarter);
    std::vector<JointTransform> out(1);
    MixPoses(from, to, 0.25f, out);
    CHECK(out[0].Translation.x == doctest::Approx(1.f));
    CHECK(std::abs(glm::dot(out[0].Rotation, AboutUp(quarter / 4.f))) == doctest::Approx(1.f));
}

TEST_CASE("Cross-fade: the weight runs from 0 to 1 over the fade's length and holds there")
{
    CrossFade fade{.Elapsed = 0.f, .Duration = 2.f};
    CHECK(fade.Weight() == doctest::Approx(0.f));
    fade.Advance(0.5f);
    CHECK(fade.Weight() == doctest::Approx(0.25f));
    CHECK_FALSE(fade.Done());
    fade.Advance(2.f);
    CHECK(fade.Weight() == doctest::Approx(1.f));
    CHECK(fade.Done());

    const CrossFade instant{.Elapsed = 0.f, .Duration = 0.f};
    CHECK(instant.Weight() == doctest::Approx(1.f));
    CHECK(instant.Done());
}
