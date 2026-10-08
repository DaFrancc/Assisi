/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestAnimationExtract.cpp
/// @brief Extracting a glTF's animations writes one clip file per animation,
/// each an "animation" asset whose id survives extracting again; cooking and
/// loading a clip file gives back its keys; and what cannot play is left out
/// or refused.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <Assisi/Core/AssetSidecar.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Geometry/AnimationClip.hpp>
#include <Assisi/Geometry/AnimationFile.hpp>
#include <Assisi/Geometry/AnimationImport.hpp>
#include <Assisi/Math/GLM.hpp>

#include "GltfFixture.hpp"

using Assisi::Core::AssetSystem;
using Assisi::Geometry::AnimationClip;
using Assisi::Geometry::AnimationExtractError;
using Assisi::Geometry::CookAnimation;
using Assisi::Geometry::ExtractedAnimations;
using Assisi::Geometry::ExtractGltfAnimations;
using Assisi::Geometry::Interpolation;
using Assisi::Geometry::JointTrack;
using Assisi::Geometry::LoadAnimation;
using GltfFixture::GltfBuffer;
using GltfFixture::kFloat;

namespace fs = std::filesystem;

namespace
{

constexpr std::string_view kRootName = "assisi_animation_extract_test";
constexpr std::string_view kStem = "rig";
constexpr std::string_view kSource = "rig.gltf";

/// A quarter turn about Y as a glTF quaternion, x y z w.
constexpr float kHalfOfRootTwo = 0.70710678f;

/// Nodes: an unanimated "Armature" over "hip" over "knee", and an unnamed
/// node beside them. Returns the "nodes" member.
std::string RigNodes()
{
    return R"("scenes":[{"nodes":[0,3]}],)"
           R"("nodes":[{"name":"Armature","translation":[0,0,5],"children":[1]},)"
           R"({"name":"hip","translation":[0,1,0],"children":[2]},)"
           R"({"name":"knee","translation":[0,0.5,0]},)"
           R"({"translation":[3,0,0]}])";
}

/// Two animations. "walk" moves the knee out along X over two seconds and turns
/// it a quarter turn about Y; "idle" steps the hip's rotation and scales the knee.
std::string TwoAnimations(GltfBuffer &buffer, float lastWalkKey = 2.f)
{
    const uint32_t walkTimes =
        buffer.Add(std::vector<float>{0.f, 1.f, lastWalkKey}, kFloat, "SCALAR",
                   std::format(R"(,"min":[0],"max":[{}])", lastWalkKey));
    const uint32_t walkMoves =
        buffer.Add(std::vector<float>{0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 2.f, 0.f, 0.f}, kFloat, "VEC3");
    const uint32_t turnTimes = buffer.Add(std::vector<float>{0.f, 2.f}, kFloat, "SCALAR", R"(,"min":[0],"max":[2])");
    const uint32_t turns = buffer.Add(std::vector<float>{0.f, 0.f, 0.f, 1.f, 0.f, kHalfOfRootTwo, 0.f, kHalfOfRootTwo},
                                      kFloat, "VEC4");
    const uint32_t idleTimes = buffer.Add(std::vector<float>{0.f, 0.5f}, kFloat, "SCALAR", R"(,"min":[0],"max":[0.5])");
    const uint32_t hipTurns = buffer.Add(std::vector<float>{0.f, 0.f, 0.f, 1.f, 0.f, kHalfOfRootTwo, 0.f, kHalfOfRootTwo},
                                         kFloat, "VEC4");
    const uint32_t kneeScales =
        buffer.Add(std::vector<float>{1.f, 1.f, 1.f, 2.f, 2.f, 2.f}, kFloat, "VEC3");
    return std::format(
        R"("animations":[)"
        R"({{"name":"walk","samplers":[{{"input":{},"output":{}}},{{"input":{},"output":{}}}],)"
        R"("channels":[{{"sampler":0,"target":{{"node":2,"path":"translation"}}}},)"
        R"({{"sampler":1,"target":{{"node":2,"path":"rotation"}}}}]}},)"
        R"({{"name":"idle","samplers":[{{"input":{},"output":{},"interpolation":"STEP"}},{{"input":{},"output":{}}}],)"
        R"("channels":[{{"sampler":0,"target":{{"node":1,"path":"rotation"}}}},)"
        R"({{"sampler":1,"target":{{"node":2,"path":"scale"}}}}]}}])",
        walkTimes, walkMoves, turnTimes, turns, idleTimes, hipTurns, idleTimes, kneeScales);
}

/// Writes the two-animation rig and returns its root.
fs::path WriteTwoAnimationRig(float lastWalkKey = 2.f)
{
    GltfBuffer buffer;
    const std::string animations = TwoAnimations(buffer, lastWalkKey);
    return buffer.Write(kRootName, kStem, RigNodes() + "," + animations);
}

std::vector<std::byte> Read(std::string_view vpath)
{
    std::expected<std::vector<std::byte>, Assisi::Core::AssetError> bytes = AssetSystem::ReadBinary(vpath);
    REQUIRE(bytes.has_value());
    return std::move(*bytes);
}

std::optional<Assisi::Core::AssetSidecar> SidecarOf(const fs::path &file)
{
    fs::path path = file;
    path += ".aast";
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return std::nullopt;
    }
    const std::string text{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    std::expected<Assisi::Core::AssetSidecar, Assisi::Core::AssetSidecarError> sidecar =
        Assisi::Core::DeserializeSidecar(text);
    return sidecar.has_value() ? std::optional<Assisi::Core::AssetSidecar>{*sidecar} : std::nullopt;
}

/// The clip a clip file cooks and loads into.
AnimationClip CookAndLoad(std::string_view vpath)
{
    const std::vector<std::byte> glb = Read(vpath);
    std::expected<std::vector<std::byte>, Assisi::Core::AssetError> cooked = CookAnimation(glb);
    REQUIRE(cooked.has_value());
    std::expected<AnimationClip, Assisi::Core::AssetError> clip = LoadAnimation(*cooked);
    REQUIRE(clip.has_value());
    return std::move(*clip);
}

const JointTrack *TrackFor(const AnimationClip &clip, std::string_view joint)
{
    for (const JointTrack &track : clip.Tracks)
    {
        if (track.Joint == joint)
        {
            return &track;
        }
    }
    return nullptr;
}

/// The JSON chunk of a `.glb`: the 4-byte length after the 12-byte header.
std::string JsonChunkOf(std::span<const std::byte> glb)
{
    constexpr std::size_t kHeaderBytes = 12;
    constexpr std::size_t kChunkHeaderBytes = 8;
    REQUIRE(glb.size() > kHeaderBytes + kChunkHeaderBytes);
    uint32_t length = 0;
    std::memcpy(&length, glb.data() + kHeaderBytes, sizeof(length));
    REQUIRE(glb.size() >= kHeaderBytes + kChunkHeaderBytes + length);
    const char *json = reinterpret_cast<const char *>(glb.data() + kHeaderBytes + kChunkHeaderBytes);
    return std::string{json, length};
}

} // namespace

TEST_CASE("Animation extract: each animation becomes a clip file of its own, an animation asset")
{
    const fs::path root = WriteTwoAnimationRig();

    const std::expected<ExtractedAnimations, AnimationExtractError> extracted = ExtractGltfAnimations(kSource);
    REQUIRE(extracted.has_value());
    CHECK(extracted->written == 2);
    CHECK(extracted->skipped == 0);

    const std::optional<Assisi::Core::AssetSidecar> walk = SidecarOf(root / "rig_animations/walk.glb");
    const std::optional<Assisi::Core::AssetSidecar> idle = SidecarOf(root / "rig_animations/idle.glb");
    REQUIRE(walk.has_value());
    REQUIRE(idle.has_value());
    REQUIRE(walk->uses.size() == 1);
    CHECK(walk->uses[0].kind == "animation");
    CHECK(idle->uses[0].kind == "animation");
    CHECK(walk->guid != idle->guid);
    fs::remove_all(root);
}

TEST_CASE("Animation extract: a clip file carries the joints it moves, their parents, and no others")
{
    const fs::path root = WriteTwoAnimationRig();
    REQUIRE(ExtractGltfAnimations(kSource).has_value());

    const std::string json = JsonChunkOf(Read("rig_animations/walk.glb"));
    // The knee, and every node above it, so the clip is a whole rig in Blender.
    CHECK(json.find(R"("name":"knee")") != std::string::npos);
    CHECK(json.find(R"("name":"hip")") != std::string::npos);
    CHECK(json.find(R"("name":"Armature")") != std::string::npos);
    // The unnamed node beside the rig moves in neither clip.
    CHECK(json.find("[3,0,0]") == std::string::npos);
    // glTF requires the bounds of every key time accessor; Blender refuses a file without them.
    CHECK(json.find(R"("min")") != std::string::npos);
    CHECK(json.find(R"("max")") != std::string::npos);
    CHECK(json.find(R"("meshes")") == std::string::npos);
    fs::remove_all(root);
}

TEST_CASE("Animation extract: a clip file cooks and loads back into its keys")
{
    const fs::path root = WriteTwoAnimationRig();
    REQUIRE(ExtractGltfAnimations(kSource).has_value());

    const AnimationClip walk = CookAndLoad("rig_animations/walk.glb");
    CHECK(walk.Name == "walk");
    CHECK(walk.Duration == doctest::Approx(2.f));
    const JointTrack *knee = TrackFor(walk, "knee");
    REQUIRE(knee != nullptr);
    REQUIRE(knee->Translation.Times.size() == 3);
    CHECK(knee->Translation.Times[1] == doctest::Approx(1.f));
    CHECK(knee->Translation.Values[2].x == doctest::Approx(2.f));
    CHECK(knee->Scale.Empty());
    REQUIRE(knee->Rotation.Values.size() == 2);
    // glTF stores x y z w; a quarter turn about Y sends +X to -Z.
    const glm::vec3 x = knee->Rotation.Values[1] * glm::vec3(1.f, 0.f, 0.f);
    CHECK(x.z == doctest::Approx(-1.f));
    CHECK(x.x == doctest::Approx(0.f).epsilon(1e-5));

    const AnimationClip idle = CookAndLoad("rig_animations/idle.glb");
    CHECK(idle.Duration == doctest::Approx(0.5f));
    const JointTrack *hip = TrackFor(idle, "hip");
    REQUIRE(hip != nullptr);
    CHECK(hip->Rotation.Mode == Interpolation::Step);
    const JointTrack *idleKnee = TrackFor(idle, "knee");
    REQUIRE(idleKnee != nullptr);
    CHECK(idleKnee->Scale.Values[1].y == doctest::Approx(2.f));
    fs::remove_all(root);
}

TEST_CASE("Animation extract: extracting again rewrites the clips and keeps their ids")
{
    // Kept, every player naming a clip still finds it after the model is re-exported.
    const fs::path root = WriteTwoAnimationRig();
    REQUIRE(ExtractGltfAnimations(kSource).has_value());
    const std::optional<Assisi::Core::AssetSidecar> before = SidecarOf(root / "rig_animations/walk.glb");
    REQUIRE(before.has_value());

    GltfBuffer buffer;
    const std::string animations = TwoAnimations(buffer, 3.f);
    buffer.WriteInto(root, kStem, RigNodes() + "," + animations);
    REQUIRE(ExtractGltfAnimations(kSource).has_value());

    const std::optional<Assisi::Core::AssetSidecar> after = SidecarOf(root / "rig_animations/walk.glb");
    REQUIRE(after.has_value());
    CHECK(after->guid == before->guid);
    CHECK(CookAndLoad("rig_animations/walk.glb").Duration == doctest::Approx(3.f));
    fs::remove_all(root);
}

TEST_CASE("Animation extract: two animations of one name get two files")
{
    GltfBuffer buffer;
    std::string animations = TwoAnimations(buffer);
    const std::string::size_type idle = animations.find(R"("name":"idle")");
    REQUIRE(idle != std::string::npos);
    animations.replace(idle, std::string_view{R"("name":"idle")"}.size(), R"("name":"walk")");
    const fs::path root = buffer.Write(kRootName, kStem, RigNodes() + "," + animations);

    const std::expected<ExtractedAnimations, AnimationExtractError> extracted = ExtractGltfAnimations(kSource);
    REQUIRE(extracted.has_value());
    CHECK(extracted->written == 2);
    CHECK(fs::exists(root / "rig_animations/walk.glb"));
    CHECK(fs::exists(root / "rig_animations/walk_1.glb"));
    fs::remove_all(root);
}

TEST_CASE("Animation extract: an animation that cannot play is left out, and the rest still written")
{
    GltfBuffer buffer;
    std::string animations = TwoAnimations(buffer);
    // "idle" now moves the unnamed node, which no skeleton can match.
    const std::string::size_type target = animations.find(R"({"node":1,"path":"rotation"})");
    REQUIRE(target != std::string::npos);
    animations.replace(target, std::string_view{R"({"node":1,"path":"rotation"})"}.size(),
                       R"({"node":3,"path":"rotation"})");
    const fs::path root = buffer.Write(kRootName, kStem, RigNodes() + "," + animations);

    const std::expected<ExtractedAnimations, AnimationExtractError> extracted = ExtractGltfAnimations(kSource);
    REQUIRE(extracted.has_value());
    CHECK(extracted->written == 1);
    CHECK(extracted->skipped == 1);
    CHECK(fs::exists(root / "rig_animations/walk.glb"));
    CHECK_FALSE(fs::exists(root / "rig_animations/idle.glb"));
    fs::remove_all(root);
}

TEST_CASE("Animation extract: cubic keys and morph weights are left out until they are supported")
{
    for (std::string_view change : {R"("interpolation":"CUBICSPLINE")", R"("path":"weights")"})
    {
        CAPTURE(change);
        GltfBuffer buffer;
        std::string animations = TwoAnimations(buffer);
        const std::string_view from = change.starts_with(R"("interp)") ? std::string_view{R"("interpolation":"STEP")"}
                                                                        : std::string_view{R"("path":"scale")"};
        animations.replace(animations.find(from), from.size(), change);
        const fs::path root = buffer.Write(kRootName, kStem, RigNodes() + "," + animations);

        const std::expected<ExtractedAnimations, AnimationExtractError> extracted = ExtractGltfAnimations(kSource);
        REQUIRE(extracted.has_value());
        CHECK(extracted->written == 1);
        CHECK(extracted->skipped == 1);
        fs::remove_all(root);
    }
}

TEST_CASE("Animation extract: key times that do not increase are left out")
{
    const fs::path root = WriteTwoAnimationRig(0.5f);
    const std::expected<ExtractedAnimations, AnimationExtractError> extracted = ExtractGltfAnimations(kSource);
    REQUIRE(extracted.has_value());
    CHECK(extracted->written == 1);
    CHECK(extracted->skipped == 1);
    CHECK_FALSE(fs::exists(root / "rig_animations/walk.glb"));
    fs::remove_all(root);
}

TEST_CASE("Animation extract: a glTF with no animations writes nothing")
{
    GltfBuffer buffer;
    (void)buffer.Add(std::vector<float>{0.f}, kFloat, "SCALAR");
    const fs::path root = buffer.Write(kRootName, kStem, RigNodes());

    const std::expected<ExtractedAnimations, AnimationExtractError> extracted = ExtractGltfAnimations(kSource);
    REQUIRE_FALSE(extracted.has_value());
    CHECK(extracted.error() == AnimationExtractError::NoAnimations);
    CHECK(std::distance(fs::directory_iterator{root}, fs::directory_iterator{}) == 2);
    fs::remove_all(root);
}

TEST_CASE("Animation cook: a file of several animations, or one whose buffers are outside it, is refused")
{
    // The source rig has two animations and keeps its keys in rig.bin.
    const fs::path root = WriteTwoAnimationRig();
    CHECK_FALSE(CookAnimation(Read(kSource)).has_value());

    GltfBuffer buffer;
    std::string animations = TwoAnimations(buffer);
    const std::string::size_type idle = animations.find(R"(,{"name":"idle")");
    REQUIRE(idle != std::string::npos);
    animations.erase(idle, animations.size() - 1 - idle);
    buffer.WriteInto(root, kStem, RigNodes() + "," + animations);
    const std::expected<std::vector<std::byte>, Assisi::Core::AssetError> external = CookAnimation(Read(kSource));
    REQUIRE_FALSE(external.has_value());
    CHECK(external.error().code == Assisi::Core::AssetErrorCode::UnsupportedEncoding);
    fs::remove_all(root);
}

TEST_CASE("Animation load: a payload cut short, or of another version, is refused without crashing")
{
    const fs::path root = WriteTwoAnimationRig();
    REQUIRE(ExtractGltfAnimations(kSource).has_value());
    std::expected<std::vector<std::byte>, Assisi::Core::AssetError> cooked = CookAnimation(Read("rig_animations/walk.glb"));
    REQUIRE(cooked.has_value());
    REQUIRE(LoadAnimation(*cooked).has_value());

    for (std::size_t length = 0; length < cooked->size(); length += 3)
    {
        CAPTURE(length);
        CHECK_FALSE(LoadAnimation(std::span<const std::byte>{cooked->data(), length}).has_value());
    }

    std::vector<std::byte> otherVersion = *cooked;
    otherVersion[0] = std::byte{0x7F};
    CHECK_FALSE(LoadAnimation(otherVersion).has_value());
    fs::remove_all(root);
}
