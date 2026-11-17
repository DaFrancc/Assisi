/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPlayerSessionAnimation.cpp
/// @brief Animation on a host and two clients, each running its own frame
/// systems with its own asset store over the same character built in code.
///
/// Ordinary animation is cosmetic and every machine works out its own, so what
/// is checked is that each machine animates the character through the same
/// states from what it was sent, not that the poses match to the frame.

#include "PlayerSessionTesting.hpp"

#include "AnimatorTesting.hpp"
#include "TestRig.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Geometry/AnimationClip.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/Render/MeshBuffer.hpp>
#include <Assisi/Runtime/AnimatorGraph.hpp>
#include <Assisi/Runtime/AnimatorStep.hpp>
#include <Assisi/Runtime/Components.hpp>

using namespace Assisi;
using namespace Assisi::App::Testing;

namespace
{

constexpr std::string_view kModelPath = "tests/quadruped.glb";
constexpr std::string_view kStridePath = "tests/stride.glb";
constexpr std::string_view kLoopPath = "tests/loop.glb";
constexpr std::string_view kAnimatorPath = "tests/quadruped.sgl";

/// What the GPU would have given the mesh; any id but the unbound one.
constexpr uint32_t kMeshId = 1;

/// The stride plays once, then the loop: long enough, at the default fade, for
/// every machine to have reached the loop by kAnimateFrames however late its
/// copy of the character arrived.
constexpr float kStrideSeconds = 0.5f;
constexpr float kLoopSeconds = 1.f;
constexpr float kLoopDegrees = 20.f;
constexpr int32_t kAnimateFrames = 120;

/// A stride that moves the body forward once, then a loop that swings a fore limb.
constexpr std::string_view kAnimator = R"(use animation;
skeleton "tests/quadruped.glb";
layer base {
    state stride { play "tests/stride.glb"; then loop; }
    state loop { play "tests/loop.glb"; }
}
)";

/// What every machine is given in place of files: the character's mesh, its
/// clips and its animator.
struct Character
{
    Render::MeshBuffer mesh;
    std::shared_ptr<const Geometry::AnimationClip> stride;
    std::shared_ptr<const Geometry::AnimationClip> loop;
    std::shared_ptr<const Runtime::AnimatorGraph> animator;
};

void Build(Character &character)
{
    const Geometry::Skeleton skeleton = Runtime::Testing::BuildSkeleton(Runtime::Testing::FourLimbs());
    const glm::vec3 bodyRest = skeleton.RestLocal[Runtime::Testing::kBodyJoint].Translation;
    character.stride = Runtime::Testing::MoveClip("body", bodyRest, glm::vec3(0.f, 0.f, 0.5f), kStrideSeconds);
    character.loop =
        Runtime::Testing::CycleClip("fore_l_0", glm::vec3(1.f, 0.f, 0.f), kLoopDegrees, kLoopSeconds);

    Runtime::Testing::RigTree tree;
    tree.Model(std::string{kModelPath}, skeleton);
    tree.Clip(std::string{kStridePath});
    tree.Clip(std::string{kLoopPath});
    std::expected<Runtime::AnimatorGraph, std::string> graph = Runtime::Import::CompileAnimator(kAnimator, tree);
    REQUIRE_MESSAGE(graph.has_value(), graph.error());
    character.animator = std::make_shared<const Runtime::AnimatorGraph>(std::move(*graph));

    character.mesh.KeepWithoutGpu(Runtime::Testing::BuildSkinnedBoxes(skeleton));
    character.mesh.SetId(kMeshId);
}

/// Gives @p machine's store what a file would have: the clips and the animator.
void Provide(Machine &machine, const Character &character)
{
    machine.assets.Provide(Core::DerivedAssetId(kStridePath), character.stride);
    machine.assets.Provide(Core::DerivedAssetId(kLoopPath), character.loop);
    machine.assets.Provide(Core::DerivedAssetId(kAnimatorPath), character.animator);
}

/// Points every skinned entity on @p machine at the mesh, as the GPU cache
/// would once it had loaded. Written through Get, which marks nothing changed:
/// the resolved pointer is local and is never sent.
void BindMeshes(Machine &machine, const Character &character)
{
    for (auto [entity, skinned] : machine.world.scene.Query<Runtime::SkinnedMesh>())
    {
        (void)skinned;
        machine.world.scene.Get<Runtime::MeshRenderer>(entity)->meshBuffer = &character.mesh;
    }
}

ECS::Entity SpawnCharacter(ECS::Scene &scene)
{
    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add(entity, ECS::Transform{}) != nullptr);
    REQUIRE(scene.Add(entity, NetSync::Replicated{}) != nullptr);
    Runtime::MeshRenderer renderer;
    renderer.mesh = Core::DerivedAssetId(kModelPath);
    REQUIRE(scene.Add(entity, std::move(renderer)) != nullptr);
    REQUIRE(scene.Add(entity, Runtime::SkinnedMesh{}) != nullptr);
    Runtime::Animator animator;
    animator.machine = Core::DerivedAssetId(kAnimatorPath);
    REQUIRE(scene.Add(entity, std::move(animator)) != nullptr);
    return entity;
}

/// The one character on @p scene, or NullEntity when there is not exactly one.
ECS::Entity CharacterOf(ECS::Scene &scene)
{
    ECS::Entity found = ECS::NullEntity;
    int32_t count = 0;
    for (auto [entity, animator] : scene.Query<Runtime::Animator>())
    {
        (void)animator;
        found = entity;
        ++count;
    }
    return count == 1 ? found : ECS::NullEntity;
}

} // namespace

TEST_CASE("Player session: a host and two clients each animate the same character through its states")
{
    const std::vector<std::string> animation{"Animators", "AnimationPlayers"};
    Character character;
    Build(character);

    Session session;
    Machine &host = session.Add({}, animation);
    Machine &first = session.Add({}, animation);
    Machine &second = session.Add({}, animation);
    for (const std::unique_ptr<Machine> &machine : session.machines)
    {
        Provide(*machine, character);
    }

    const std::uint16_t port = HostOnFreePort(host.play);
    REQUIRE_MESSAGE(port != 0, "could not bind a port: ", host.play.Session().LastError());
    REQUIRE_MESSAGE(first.play.Join("127.0.0.1", port), first.play.Session().LastError());
    REQUIRE_MESSAGE(second.play.Join("127.0.0.1", port), second.play.Session().LastError());
    session.Run(kJoinSteps);

    (void)SpawnCharacter(host.world.scene);
    session.Run(kAnimateFrames,
                [&]
                {
                    for (const std::unique_ptr<Machine> &machine : session.machines)
                    {
                        BindMeshes(*machine, character);
                    }
                });

    for (const std::unique_ptr<Machine> &machine : session.machines)
    {
        ECS::Scene &scene = machine->world.scene;
        const ECS::Entity entity = CharacterOf(scene);
        REQUIRE(entity != ECS::NullEntity);
        const Runtime::AnimatorRun *run = scene.Get<Runtime::AnimatorRun>(entity);
        const Runtime::AnimationPlayback *playback = scene.Get<Runtime::AnimationPlayback>(entity);
        const Runtime::SkinnedPose *skinned = scene.Get<Runtime::SkinnedPose>(entity);
        REQUIRE(run != nullptr);
        REQUIRE(playback != nullptr);
        REQUIRE(skinned != nullptr);
        // The stride played once and the animator moved on, from its own clock.
        CHECK(Runtime::AnimatorStateNames(*run, 0) == std::vector<std::string>{"loop"});
        CHECK(playback->track.boundAnimation == Core::DerivedAssetId(kLoopPath));
        // And it was posed: the skeleton is bound and has joints to show.
        CHECK(skinned->boundMeshId == kMeshId);
        CHECK(skinned->pose.size() == character.mesh.Skeleton().JointCount());
    }
}
