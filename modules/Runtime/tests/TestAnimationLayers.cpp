/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestAnimationLayers.cpp
/// @brief Layers over an AnimationPlayer's base: a mask takes a joint and its
/// branch less the branches it excludes, an override layer replaces only what
/// its animation moves, an additive one adds its change since its first frame,
/// both by a weight that slides as the layer says, each fading over its own
/// fade, with timelines that run on at weight 0.

#include <doctest/doctest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/InternedString.hpp>
#include <Assisi/Geometry/AnimationClip.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Runtime/AnimationPlayback.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/SkinnedMeshPose.hpp>

using namespace Assisi;
using Core::InternedString;
using Geometry::AnimationClip;
using Geometry::JointTrack;
using Geometry::Skeleton;
using Runtime::AnimationLayer;
using Runtime::LayerMode;

namespace
{

constexpr uint32_t kMesh = 1;
constexpr float kDuration = 2.f;

enum Joint : uint8_t
{
    Hip,
    Knee,
    Spine,
    Head,
    Hand,
    Finger,
    Count_,
};

/// A hip with a leg and a spine; the spine holds a head and a hand, the hand a finger.
Skeleton Body()
{
    Skeleton skeleton;
    skeleton.Names = {"hip", "knee", "spine", "head", "hand", "finger"};
    skeleton.Parents = {Geometry::kNoParent, Hip, Hip, Spine, Spine, Hand};
    skeleton.RestLocal.resize(Joint::Count_);
    skeleton.InverseBind.resize(Joint::Count_, glm::mat4(1.f));
    skeleton.JointBounds.resize(Joint::Count_);
    return skeleton;
}

/// A track moving @p joint along x through @p values, evenly over the clip.
JointTrack Moving(const char *joint, std::vector<float> values)
{
    JointTrack track;
    track.Joint = joint;
    for (std::size_t key = 0; key < values.size(); ++key)
    {
        const float share = values.size() == 1 ? 0.f : static_cast<float>(key) / static_cast<float>(values.size() - 1);
        track.Translation.Times.push_back(share * kDuration);
        track.Translation.Values.push_back(glm::vec3(values[key], 0.f, 0.f));
    }
    return track;
}

std::shared_ptr<const AnimationClip> Clip(std::vector<JointTrack> tracks)
{
    AnimationClip clip;
    clip.Name = "clip";
    clip.Tracks = std::move(tracks);
    clip.Duration = kDuration;
    return std::make_shared<const AnimationClip>(std::move(clip));
}

Core::AssetId ClipId(uint8_t last)
{
    Core::AssetId id;
    id.bytes[0] = 0xBB;
    id.bytes[15] = last;
    return id;
}

constexpr uint8_t kRun = 1;
constexpr uint8_t kAim = 2;
constexpr uint8_t kAimHigh = 3;
constexpr uint8_t kFlinch = 4;

/// The legs run, and the finger moves from 7 to 9, which no layer keys.
std::shared_ptr<const AnimationClip> Run()
{
    return Clip({Moving("knee", {0.f, 2.f}), Moving("finger", {7.f, 9.f})});
}

/// The spine and the hand held at @p x.
std::shared_ptr<const AnimationClip> Aim(float x)
{
    return Clip({Moving("spine", {x}), Moving("hand", {x})});
}

/// The hand and the knee start away from rest, jolt by 1 halfway, and come back.
std::shared_ptr<const AnimationClip> Flinch()
{
    return Clip({Moving("hand", {1.f, 2.f, 1.f}), Moving("knee", {5.f, 6.f, 5.f})});
}

struct Rig
{
    Skeleton skeleton = Body();
    Runtime::SkinnedMesh skinned;
    Runtime::AnimationPlayer player;
    std::map<uint8_t, std::vector<std::shared_ptr<const AnimationClip>>> clips{
        {kRun, {Run()}}, {kAim, {Aim(3.f)}}, {kAimHigh, {Aim(5.f)}}, {kFlinch, {Flinch()}}};

    Rig()
    {
        (void)Runtime::BindPose(skinned, skeleton, kMesh);
        player.animation = ClipId(kRun);
        player.fade = 0.f;
    }

    AnimationLayer &Add(uint8_t clip, LayerMode mode, const char *root)
    {
        AnimationLayer layer;
        layer.animation = ClipId(clip);
        layer.mode = mode;
        layer.maskRoot = InternedString{root};
        layer.fade = 0.f;
        player.layers.push_back(layer);
        return player.layers.back();
    }

    Runtime::ResolvedAnimation Resolved(Core::AssetId id)
    {
        return Runtime::ResolvedAnimation{.space = nullptr, .clips = clips.at(id.bytes[15])};
    }

    void Advance(float dt)
    {
        (void)Runtime::AdvanceAnimationPlayer(player, Resolved(player.animation), skeleton, dt, skinned);
        player.layerStates.resize(player.layers.size());
        for (std::size_t layer = 0; layer < player.layers.size(); ++layer)
        {
            Runtime::AdvanceAnimationLayer(player.layers[layer], player.layerStates[layer],
                                           Resolved(player.layers[layer].animation), skeleton, dt, skinned);
        }
    }

    float X(Joint joint) const { return skinned.pose[joint].Translation.x; }
};

std::vector<uint8_t> Mask(const Skeleton &skeleton, const char *root, std::vector<InternedString> exclusions,
                          Runtime::MaskResult *result = nullptr)
{
    std::vector<uint8_t> mask;
    const Runtime::MaskResult built = Runtime::BuildJointMask(skeleton, InternedString{root}, exclusions, mask);
    if (result != nullptr)
    {
        *result = built;
    }
    return mask;
}

} // namespace

TEST_CASE("Animation layers: a mask takes its joint's branch, less each excluded branch, in any joint order")
{
    const Skeleton body = Body();
    CHECK(Mask(body, "spine", {}) == std::vector<uint8_t>{0, 0, 1, 1, 1, 1});
    CHECK(Mask(body, "spine", {InternedString{"hand"}}) == std::vector<uint8_t>{0, 0, 1, 1, 0, 0});
    CHECK(Mask(body, "", {InternedString{"spine"}}) == std::vector<uint8_t>{1, 1, 0, 0, 0, 0});

    // The finger listed first, its hand and spine after it.
    Skeleton shuffled;
    shuffled.Names = {"finger", "hand", "hip", "spine"};
    shuffled.Parents = {1, 3, Geometry::kNoParent, 2};
    shuffled.RestLocal.resize(4);
    CHECK(Mask(shuffled, "spine", {}) == std::vector<uint8_t>{1, 1, 0, 1});

    Runtime::MaskResult result;
    CHECK(Mask(body, "tail", {}, &result) == std::vector<uint8_t>{0, 0, 0, 0, 0, 0});
    CHECK(result.rootMissing);
    CHECK(Mask(body, "spine", {InternedString{"wing"}}, &result) == std::vector<uint8_t>{0, 0, 1, 1, 1, 1});
    CHECK(result.exclusionMissing);
    CHECK_FALSE(result.rootMissing);
}

TEST_CASE("Animation layers: an override replaces what its animation moves inside its mask, by its weight")
{
    Rig rig;
    AnimationLayer &aim = rig.Add(kAim, LayerMode::Override, "spine");
    rig.Advance(0.f);
    rig.Advance(1.f);
    CHECK(rig.X(Spine) == doctest::Approx(3.f));
    CHECK(rig.X(Hand) == doctest::Approx(3.f));
    // The legs run on underneath, and the finger, which aim doesn't key, keeps the run's.
    CHECK(rig.X(Knee) == doctest::Approx(1.f));
    CHECK(rig.X(Finger) == doctest::Approx(8.f));

    aim.weight = 0.5f;
    rig.Advance(0.f);
    CHECK(rig.X(Spine) == doctest::Approx(1.5f));
    CHECK(rig.X(Finger) == doctest::Approx(8.f));

    aim.weight = 1.f;
    aim.exclusions = {InternedString{"hand"}};
    rig.Advance(0.f);
    CHECK(rig.X(Spine) == doctest::Approx(3.f));
    CHECK(rig.X(Hand) == doctest::Approx(0.f));
}

TEST_CASE("Animation layers: an additive layer adds its change since its first frame")
{
    Rig rig;
    (void)rig.Add(kFlinch, LayerMode::Additive, "");
    rig.Advance(0.f);
    // At its first frame it changes nothing, though that frame isn't rest.
    CHECK(rig.X(Hand) == doctest::Approx(0.f));
    CHECK(rig.X(Knee) == doctest::Approx(0.f));
    rig.Advance(1.f);
    CHECK(rig.X(Hand) == doctest::Approx(1.f));
    CHECK(rig.X(Knee) == doctest::Approx(2.f));

    // Frame after frame it adds to what the base gives, never to what it added
    // itself, though the base doesn't move the hand.
    rig.Advance(0.5f);
    rig.Advance(0.5f);
    CHECK(rig.X(Hand) == doctest::Approx(0.f));
    rig.Advance(1.f);
    CHECK(rig.X(Hand) == doctest::Approx(1.f));
}

TEST_CASE("Animation layers: running, aiming with the upper body and a flinch all play at once")
{
    Rig rig;
    (void)rig.Add(kAim, LayerMode::Override, "spine");
    (void)rig.Add(kFlinch, LayerMode::Additive, "");
    rig.Advance(0.f);
    rig.Advance(1.f);
    CHECK(rig.X(Knee) == doctest::Approx(1.f + 1.f));
    CHECK(rig.X(Spine) == doctest::Approx(3.f));
    CHECK(rig.X(Hand) == doctest::Approx(3.f + 1.f));
    CHECK(rig.X(Finger) == doctest::Approx(8.f));
}

TEST_CASE("Animation layers: the weight slides over weightFade, as a duration or as a speed")
{
    Rig rig;
    AnimationLayer &aim = rig.Add(kAim, LayerMode::Override, "spine");
    rig.Advance(0.f);
    REQUIRE(rig.X(Spine) == doctest::Approx(3.f));

    aim.weightFade = 0.5f;
    aim.weight = 0.f;
    rig.Advance(0.25f);
    CHECK(rig.player.layerStates[0].weight == doctest::Approx(0.5f));
    rig.Advance(0.25f);
    CHECK(rig.player.layerStates[0].weight == doctest::Approx(0.f));

    // As a speed, a change of a quarter takes a quarter of weightFade.
    aim.weightSlide = Runtime::WeightSlide::Speed;
    aim.weight = 0.25f;
    rig.Advance(0.0625f);
    CHECK(rig.player.layerStates[0].weight == doctest::Approx(0.125f));
    rig.Advance(0.0625f);
    CHECK(rig.player.layerStates[0].weight == doctest::Approx(0.25f));

    aim.weightFade = 0.f;
    aim.weight = 1.f;
    rig.Advance(0.f);
    CHECK(rig.player.layerStates[0].weight == doctest::Approx(1.f));
}

TEST_CASE("Animation layers: a layer's animation changes over the layer's fade, not the player's")
{
    Rig rig;
    AnimationLayer &aim = rig.Add(kAim, LayerMode::Override, "spine");
    rig.Advance(0.f);
    aim.animation = ClipId(kAimHigh);
    aim.fade = 1.f;
    rig.Advance(0.5f);
    CHECK(rig.X(Spine) == doctest::Approx(4.f));
    rig.Advance(0.5f);
    CHECK(rig.X(Spine) == doctest::Approx(5.f));
}

TEST_CASE("Animation layers: at weight 0 a layer's timeline still runs, and a mask with no root joint moves nothing")
{
    Rig rig;
    AnimationLayer &flinch = rig.Add(kFlinch, LayerMode::Additive, "");
    flinch.weight = 0.f;
    rig.Advance(0.f);
    rig.Advance(0.5f);
    CHECK(rig.player.layerStates[0].track.current.Phase == doctest::Approx(0.25f));
    CHECK(rig.X(Hand) == doctest::Approx(0.f));

    Rig missing;
    (void)missing.Add(kAim, LayerMode::Override, "tail");
    missing.Advance(0.f);
    CHECK(missing.X(Spine) == doctest::Approx(0.f));
    CHECK(missing.player.layerStates[0].rootMissing);
}
