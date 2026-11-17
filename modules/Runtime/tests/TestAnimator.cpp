/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestAnimator.cpp
/// @brief An Animator running the book's character on a player with no mesh:
/// it starts each layer at its first state, fires transitions from triggers and
/// params with their own fades, waits for a fade unless a transition may
/// interrupt, moves on when a clip played once ends, keeps its place when the
/// file changes, and writes rate, blend position and weight every frame. All of
/// it goes into the player's request; the player's own fields are not touched.

#include "AnimatorTesting.hpp"

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/AnimatorStep.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace Assisi;
using namespace Assisi::Runtime::Testing;

namespace
{

/// The fades the character's transitions are written with.
constexpr float kJumpFade = 0.05f;
constexpr float kLandFade = 0.1f;

std::shared_ptr<const Runtime::AnimatorGraph> Cooked(std::string_view source)
{
    const TestTree tree;
    std::expected<Runtime::AnimatorGraph, std::string> graph = Runtime::Import::CompileAnimator(source, tree);
    REQUIRE_MESSAGE(graph.has_value(), graph.error());
    return std::make_shared<const Runtime::AnimatorGraph>(std::move(*graph));
}

struct Character
{
    Runtime::Animator animator;
    Runtime::AnimatorRun run;
    Runtime::AnimationPlayer player;
    Runtime::AnimationPlayback playback;
};

Character Bound(std::string_view source = kCharacter)
{
    Character character;
    Runtime::BindAnimator(character.animator, character.run, character.playback, Cooked(source));
    return character;
}

Core::AssetId ClipId(std::string_view name)
{
    return Core::DerivedAssetId(Clip(name));
}

/// The player's base track as AnimationPlayers would leave it: playing
/// @p animation at @p phase, fading or not.
void Playing(Runtime::AnimationPlayback &playback, Core::AssetId animation, float phase, bool fading = false)
{
    playback.track.boundAnimation = animation;
    playback.track.current.Phase = phase;
    playback.track.fading = fading;
}

/// Jumps from locomotion and steps once, landing in jump_start.
void Jump(Character &character)
{
    REQUIRE(Runtime::SetAnimatorBool(character.run, "grounded", true));
    REQUIRE(Runtime::FireAnimatorTrigger(character.run, "jump"));
    Runtime::StepAnimator(character.animator, character.run, character.playback);
    REQUIRE(Runtime::AnimatorStateNames(character.run,0) == std::vector<std::string>{"airborne", "jump_start"});
}

} // namespace

TEST_CASE("Animator: each layer starts at its first state, and the player plays it" * NeedsUal1())
{
    const Character character = Bound();
    CHECK(Runtime::AnimatorStateNames(character.run,0) == std::vector<std::string>{"locomotion"});
    CHECK(Runtime::AnimatorStateNames(character.run,1) == std::vector<std::string>{"aim"});
    CHECK(character.playback.request.base.animation == Core::DerivedAssetId(kSpace));
    CHECK(character.playback.request.base.loop);

    REQUIRE(character.playback.request.layers.size() == 1);
    const Runtime::LayerRequest &upper = character.playback.request.layers[0];
    CHECK(upper.play.animation == ClipId("Pistol_Aim_Neutral"));
    CHECK(upper.maskRoot.View() == "spine_01");
    CHECK_FALSE(upper.play.loop);
}

TEST_CASE("Animator: the player's own fields stay as the author set them" * NeedsUal1())
{
    Character character;
    const Core::AssetId authored = ClipId("Idle_Loop");
    character.player.animation = authored;
    character.player.speed = 2.f;
    character.player.loop = false;
    Runtime::BindAnimator(character.animator, character.run, character.playback, Cooked(kCharacter));
    Jump(character);

    CHECK(character.player.animation == authored);
    CHECK(character.player.speed == 2.f);
    CHECK(character.player.fade == doctest::Approx(Runtime::AnimationPlayer{}.fade));
    CHECK_FALSE(character.player.loop);
    CHECK(character.player.layers.empty());
    CHECK(character.playback.request.base.animation == ClipId("Jump_Start"));
}

TEST_CASE("Animator: a transition fires into a state holding states, with its own fade" * NeedsUal1())
{
    Character character = Bound();
    Jump(character);
    CHECK(character.playback.request.base.animation == ClipId("Jump_Start"));
    CHECK(character.playback.request.base.fade == doctest::Approx(kJumpFade));
    CHECK_FALSE(character.playback.request.base.loop);
}

TEST_CASE("Animator: a trigger lasts one frame, used or not" * NeedsUal1())
{
    Character character = Bound();
    REQUIRE(Runtime::SetAnimatorBool(character.run, "grounded", false));
    REQUIRE(Runtime::FireAnimatorTrigger(character.run, "jump"));
    Runtime::StepAnimator(character.animator, character.run, character.playback);
    REQUIRE(Runtime::SetAnimatorBool(character.run, "grounded", true));
    Runtime::StepAnimator(character.animator, character.run, character.playback);
    CHECK(Runtime::AnimatorStateNames(character.run,0) == std::vector<std::string>{"locomotion"});
}

TEST_CASE("Animator: a clip played once moves on when it ends, and not a frame before" * NeedsUal1())
{
    Character character = Bound();
    Jump(character);
    REQUIRE(Runtime::SetAnimatorBool(character.run, "grounded", false));

    Playing(character.playback, ClipId("Jump_Start"), 0.99f);
    Runtime::StepAnimator(character.animator, character.run, character.playback);
    CHECK(Runtime::AnimatorStateNames(character.run,0) == std::vector<std::string>{"airborne", "jump_start"});

    // The track still on the clip before is no sign this one has ended.
    Playing(character.playback, Core::DerivedAssetId(kSpace), 1.f);
    Runtime::StepAnimator(character.animator, character.run, character.playback);
    CHECK(Runtime::AnimatorStateNames(character.run,0) == std::vector<std::string>{"airborne", "jump_start"});

    Playing(character.playback, ClipId("Jump_Start"), 1.f);
    Runtime::StepAnimator(character.animator, character.run, character.playback);
    CHECK(Runtime::AnimatorStateNames(character.run,0) == std::vector<std::string>{"airborne", "fall"});
    CHECK(character.playback.request.base.animation == ClipId("Jump_Loop"));
    CHECK(character.playback.request.base.fade == doctest::Approx(Runtime::kDefaultAnimatorFade));
    CHECK(character.playback.request.base.loop);
}

TEST_CASE("Animator: an outer transition wins over the state inside it moving on" * NeedsUal1())
{
    Character character = Bound();
    Jump(character);
    Playing(character.playback, ClipId("Jump_Start"), 1.f);
    Runtime::StepAnimator(character.animator, character.run, character.playback);
    CHECK(Runtime::AnimatorStateNames(character.run,0) == std::vector<std::string>{"land"});
    CHECK(character.playback.request.base.fade == doctest::Approx(kLandFade));
}

TEST_CASE("Animator: during a fade only a transition that may interrupt fires" * NeedsUal1())
{
    Character character = Bound();
    Jump(character);
    Playing(character.playback, ClipId("Jump_Start"), 0.5f, true);
    Runtime::StepAnimator(character.animator, character.run, character.playback);
    // airborne -> land waits for the fade.
    CHECK(Runtime::AnimatorStateNames(character.run,0) == std::vector<std::string>{"airborne", "jump_start"});

    Playing(character.playback, ClipId("Jump_Start"), 0.5f, false);
    Runtime::StepAnimator(character.animator, character.run, character.playback);
    REQUIRE(Runtime::AnimatorStateNames(character.run,0) == std::vector<std::string>{"land"});

    REQUIRE(Runtime::SetAnimatorBool(character.run, "grounded", false));
    Playing(character.playback, ClipId("Jump_Land"), 0.f, true);
    Runtime::StepAnimator(character.animator, character.run, character.playback);
    // land -> airborne may interrupt.
    CHECK(Runtime::AnimatorStateNames(character.run,0) == std::vector<std::string>{"airborne", "jump_start"});
}

TEST_CASE("Animator: a state played once again from the start when entered on the same clip" * NeedsUal1())
{
    const std::string walk = Clip("Walk_Loop");
    Character character =
        Bound("use animation;\nskeleton \"" + std::string{kModel} + "\";\nlayer base {\n    state a { play \"" + walk +
              "\"; then b; }\n    state b { play \"" + walk + "\"; then a; }\n}\n");
    Playing(character.playback, ClipId("Walk_Loop"), 1.f);
    const uint32_t before = character.playback.request.base.restarts;
    Runtime::StepAnimator(character.animator, character.run, character.playback);
    CHECK(Runtime::AnimatorStateNames(character.run,0) == std::vector<std::string>{"b"});
    // Playback takes the clip back to the start phase when the count goes up.
    CHECK(character.playback.request.base.restarts == before + 1);
    CHECK(character.playback.request.base.startPhase == 0.f);
}

TEST_CASE("Animator: rate, blend position and weight are written every frame" * NeedsUal1())
{
    Character character = Bound();
    constexpr float kSpeed = 3.f;
    constexpr float kAiming = 0.4f;
    REQUIRE(Runtime::SetAnimatorFloat(character.run, "speed", kSpeed));
    REQUIRE(Runtime::SetAnimatorFloat(character.run, "aiming", kAiming));
    Runtime::StepAnimator(character.animator, character.run, character.playback);
    CHECK(character.playback.request.base.parameter.x == doctest::Approx(kSpeed));
    CHECK(character.playback.request.base.speed == doctest::Approx(1.f));
    REQUIRE(character.playback.request.layers.size() == 1);
    CHECK(character.playback.request.layers[0].weight == doctest::Approx(kAiming));
    // A pose holds still.
    CHECK(character.playback.request.layers[0].play.speed == 0.f);
}

TEST_CASE("Animator: a changed file keeps each layer in the states of the same names, and the params" * NeedsUal1())
{
    Character character = Bound();
    Jump(character);

    // jump_start is renamed, so the layer falls back to airborne's first state.
    std::string renamed{kCharacter};
    for (std::size_t at = renamed.find("jump_start"); at != std::string::npos; at = renamed.find("jump_start", at))
    {
        renamed.replace(at, std::string_view{"jump_start"}.size(), "leap");
    }
    Runtime::BindAnimator(character.animator, character.run, character.playback, Cooked(renamed));
    CHECK(Runtime::AnimatorStateNames(character.run,0) == std::vector<std::string>{"airborne", "leap"});

    Runtime::BindAnimator(character.animator, character.run, character.playback, Cooked(kCharacter));
    CHECK(Runtime::AnimatorStateNames(character.run,0) == std::vector<std::string>{"airborne", "jump_start"});

    // Locomotion plays where speed puts it, so speed's value shows there.
    Character walking = Bound();
    REQUIRE(Runtime::SetAnimatorFloat(walking.run, "speed", 2.f));
    Runtime::BindAnimator(walking.animator, walking.run, walking.playback, Cooked(kCharacter));
    Runtime::StepAnimator(walking.animator, walking.run, walking.playback);
    CHECK(walking.playback.request.base.parameter.x == doctest::Approx(2.f));
}

TEST_CASE("Animator: a param is set only by its own name and type" * NeedsUal1())
{
    Character character = Bound();
    CHECK_FALSE(Runtime::SetAnimatorBool(character.run, "jump", true));
    CHECK_FALSE(Runtime::FireAnimatorTrigger(character.run, "grounded"));
    CHECK_FALSE(Runtime::SetAnimatorFloat(character.run, "grounded", 1.f));
    CHECK_FALSE(Runtime::SetAnimatorFloat(character.run, "sped", 1.f));
    Runtime::AnimatorRun unbound;
    CHECK_FALSE(Runtime::SetAnimatorFloat(unbound, "speed", 1.f));

    REQUIRE(Runtime::SetAnimatorFloat(character.run, "speed", 2.5f));
    CHECK(Runtime::AnimatorFloat(character.run, "speed") == std::optional<float>{2.5f});
    CHECK_FALSE(Runtime::AnimatorFloat(character.run, "grounded").has_value());
    CHECK_FALSE(Runtime::AnimatorFloat(unbound, "speed").has_value());
}

TEST_CASE("Animator: a level keeps its file and clip bindings, and not where it was in them" * NeedsUal1())
{
    ECS::Scene scene;
    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add(entity, Runtime::SkinnedMesh{}) != nullptr);
    REQUIRE(scene.Add(entity, Runtime::AnimationPlayer{}) != nullptr);
    Runtime::Animator animator;
    animator.machine = Core::DerivedAssetId("characters/hero.sgl");
    animator.clips.push_back(
        Runtime::ClipBinding{.clip = ClipId("Walk_Loop"), .name = Core::InternedString{"run_clip"}});
    REQUIRE(scene.Add(entity, std::move(animator)) != nullptr);
    Runtime::BindAnimator(*scene.Get<Runtime::Animator>(entity), *scene.Get<Runtime::AnimatorRun>(entity),
                          *scene.Get<Runtime::AnimationPlayback>(entity), Cooked(kCharacter));

    ECS::Scene loaded;
    REQUIRE(Runtime::SceneSerializer::Load(loaded, Runtime::SceneSerializer::Save(scene)).has_value());
    const ECS::Entity restoredEntity{.index = 0, .generation = 0};
    const Runtime::Animator *restored = loaded.Get<Runtime::Animator>(restoredEntity);
    REQUIRE(restored != nullptr);
    CHECK(restored->machine == Core::DerivedAssetId("characters/hero.sgl"));
    REQUIRE(restored->clips.size() == 1);
    CHECK(restored->clips[0].clip == ClipId("Walk_Loop"));
    CHECK(restored->clips[0].name.View() == "run_clip");
    // Bound again from the file when it loads.
    const Runtime::AnimatorRun *run = loaded.Get<Runtime::AnimatorRun>(restoredEntity);
    REQUIRE(run != nullptr);
    CHECK(run->graph == nullptr);
}

TEST_CASE("Animator: a clip param plays what the Animator binds to it" * NeedsUal1())
{
    const std::string source = "use animation;\nskeleton \"" + std::string{kModel} +
                               "\";\nparam run_clip: clip;\nlayer base {\n    state run { play run_clip; }\n}\n";
    Character character;
    character.animator.clips.push_back(
        Runtime::ClipBinding{.clip = ClipId("Walk_Loop"), .name = Core::InternedString{"run_clip"}});
    Runtime::BindAnimator(character.animator, character.run, character.playback, Cooked(source));
    CHECK(character.playback.request.base.animation == ClipId("Walk_Loop"));

    Character unboundClip;
    Runtime::BindAnimator(unboundClip.animator, unboundClip.run, unboundClip.playback, Cooked(source));
    CHECK(unboundClip.playback.request.base.animation == Core::AssetId{});
    CHECK(unboundClip.run.warned);
}
