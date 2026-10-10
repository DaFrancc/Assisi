/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestAnimator.cpp
/// @brief An Animator running the book's character on a player with no mesh:
/// it starts each layer at its first state, fires transitions from triggers and
/// params with their own fades, waits for a fade unless a transition may
/// interrupt, moves on when a clip played once ends, keeps its place when the
/// file changes, and writes rate, blend position and weight every frame.

#include "AnimatorTesting.hpp"

#include <Assisi/Runtime/AnimatorStep.hpp>

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
    Runtime::AnimationPlayer player;
};

Character Bound(std::string_view source = kCharacter)
{
    Character character;
    Runtime::BindAnimator(character.animator, character.player, Cooked(source));
    return character;
}

Core::AssetId ClipId(std::string_view name)
{
    return Core::DerivedAssetId(Clip(name));
}

/// The player's base track as AnimationPlayers would leave it: playing
/// @p animation at @p phase, fading or not.
void Playing(Runtime::AnimationPlayer &player, Core::AssetId animation, float phase, bool fading = false)
{
    player.track.boundAnimation = animation;
    player.track.current.Phase = phase;
    player.track.fading = fading;
}

/// Jumps from locomotion and steps once, landing in jump_start.
void Jump(Character &character)
{
    REQUIRE(Runtime::SetAnimatorBool(character.animator, "grounded", true));
    REQUIRE(Runtime::FireAnimatorTrigger(character.animator, "jump"));
    Runtime::StepAnimator(character.animator, character.player);
    REQUIRE(Runtime::AnimatorStateNames(character.animator, 0) == std::vector<std::string>{"airborne", "jump_start"});
}

} // namespace

TEST_CASE("Animator: each layer starts at its first state, and the player plays it")
{
    const Character character = Bound();
    CHECK(Runtime::AnimatorStateNames(character.animator, 0) == std::vector<std::string>{"locomotion"});
    CHECK(Runtime::AnimatorStateNames(character.animator, 1) == std::vector<std::string>{"aim"});
    CHECK(character.player.animation == Core::DerivedAssetId(kSpace));
    CHECK(character.player.loop);

    REQUIRE(character.player.layers.size() == 1);
    const Runtime::AnimationLayer &upper = character.player.layers[0];
    CHECK(upper.animation == ClipId("Pistol_Aim_Neutral"));
    CHECK(upper.maskRoot.View() == "spine_01");
    CHECK_FALSE(upper.loop);
}

TEST_CASE("Animator: a transition fires into a state holding states, with its own fade")
{
    Character character = Bound();
    Jump(character);
    CHECK(character.player.animation == ClipId("Jump_Start"));
    CHECK(character.player.fade == doctest::Approx(kJumpFade));
    CHECK_FALSE(character.player.loop);
}

TEST_CASE("Animator: a trigger lasts one frame, used or not")
{
    Character character = Bound();
    REQUIRE(Runtime::SetAnimatorBool(character.animator, "grounded", false));
    REQUIRE(Runtime::FireAnimatorTrigger(character.animator, "jump"));
    Runtime::StepAnimator(character.animator, character.player);
    REQUIRE(Runtime::SetAnimatorBool(character.animator, "grounded", true));
    Runtime::StepAnimator(character.animator, character.player);
    CHECK(Runtime::AnimatorStateNames(character.animator, 0) == std::vector<std::string>{"locomotion"});
}

TEST_CASE("Animator: a clip played once moves on when it ends, and not a frame before")
{
    Character character = Bound();
    Jump(character);
    REQUIRE(Runtime::SetAnimatorBool(character.animator, "grounded", false));

    Playing(character.player, ClipId("Jump_Start"), 0.99f);
    Runtime::StepAnimator(character.animator, character.player);
    CHECK(Runtime::AnimatorStateNames(character.animator, 0) == std::vector<std::string>{"airborne", "jump_start"});

    // The track still on the clip before is no sign this one has ended.
    Playing(character.player, Core::DerivedAssetId(kSpace), 1.f);
    Runtime::StepAnimator(character.animator, character.player);
    CHECK(Runtime::AnimatorStateNames(character.animator, 0) == std::vector<std::string>{"airborne", "jump_start"});

    Playing(character.player, ClipId("Jump_Start"), 1.f);
    Runtime::StepAnimator(character.animator, character.player);
    CHECK(Runtime::AnimatorStateNames(character.animator, 0) == std::vector<std::string>{"airborne", "fall"});
    CHECK(character.player.animation == ClipId("Jump_Loop"));
    CHECK(character.player.fade == doctest::Approx(Runtime::kDefaultAnimatorFade));
    CHECK(character.player.loop);
}

TEST_CASE("Animator: an outer transition wins over the state inside it moving on")
{
    Character character = Bound();
    Jump(character);
    Playing(character.player, ClipId("Jump_Start"), 1.f);
    Runtime::StepAnimator(character.animator, character.player);
    CHECK(Runtime::AnimatorStateNames(character.animator, 0) == std::vector<std::string>{"land"});
    CHECK(character.player.fade == doctest::Approx(kLandFade));
}

TEST_CASE("Animator: during a fade only a transition that may interrupt fires")
{
    Character character = Bound();
    Jump(character);
    Playing(character.player, ClipId("Jump_Start"), 0.5f, true);
    Runtime::StepAnimator(character.animator, character.player);
    // airborne -> land waits for the fade.
    CHECK(Runtime::AnimatorStateNames(character.animator, 0) == std::vector<std::string>{"airborne", "jump_start"});

    Playing(character.player, ClipId("Jump_Start"), 0.5f, false);
    Runtime::StepAnimator(character.animator, character.player);
    REQUIRE(Runtime::AnimatorStateNames(character.animator, 0) == std::vector<std::string>{"land"});

    REQUIRE(Runtime::SetAnimatorBool(character.animator, "grounded", false));
    Playing(character.player, ClipId("Jump_Land"), 0.f, true);
    Runtime::StepAnimator(character.animator, character.player);
    // land -> airborne may interrupt.
    CHECK(Runtime::AnimatorStateNames(character.animator, 0) == std::vector<std::string>{"airborne", "jump_start"});
}

TEST_CASE("Animator: a state played once again from the start when entered on the same clip")
{
    const std::string walk = Clip("Walk_Loop");
    Character character =
        Bound("use animation;\nskeleton \"" + std::string{kModel} + "\";\nlayer base {\n    state a { play \"" + walk +
              "\"; then b; }\n    state b { play \"" + walk + "\"; then a; }\n}\n");
    Playing(character.player, ClipId("Walk_Loop"), 1.f);
    Runtime::StepAnimator(character.animator, character.player);
    CHECK(Runtime::AnimatorStateNames(character.animator, 0) == std::vector<std::string>{"b"});
    CHECK(character.player.track.current.Phase == 0.f);
}

TEST_CASE("Animator: rate, blend position and weight are written every frame")
{
    Character character = Bound();
    constexpr float kSpeed = 3.f;
    constexpr float kAiming = 0.4f;
    REQUIRE(Runtime::SetAnimatorFloat(character.animator, "speed", kSpeed));
    REQUIRE(Runtime::SetAnimatorFloat(character.animator, "aiming", kAiming));
    Runtime::StepAnimator(character.animator, character.player);
    CHECK(character.player.parameter.x == doctest::Approx(kSpeed));
    CHECK(character.player.speed == doctest::Approx(1.f));
    REQUIRE(character.player.layers.size() == 1);
    CHECK(character.player.layers[0].weight == doctest::Approx(kAiming));
    // A pose holds still.
    CHECK(character.player.layers[0].speed == 0.f);
}

TEST_CASE("Animator: a changed file keeps each layer in the states of the same names, and the params")
{
    Character character = Bound();
    Jump(character);

    // jump_start is renamed, so the layer falls back to airborne's first state.
    std::string renamed{kCharacter};
    for (std::size_t at = renamed.find("jump_start"); at != std::string::npos; at = renamed.find("jump_start", at))
    {
        renamed.replace(at, std::string_view{"jump_start"}.size(), "leap");
    }
    Runtime::BindAnimator(character.animator, character.player, Cooked(renamed));
    CHECK(Runtime::AnimatorStateNames(character.animator, 0) == std::vector<std::string>{"airborne", "leap"});

    Runtime::BindAnimator(character.animator, character.player, Cooked(kCharacter));
    CHECK(Runtime::AnimatorStateNames(character.animator, 0) == std::vector<std::string>{"airborne", "jump_start"});

    // Locomotion plays where speed puts it, so speed's value shows there.
    Character walking = Bound();
    REQUIRE(Runtime::SetAnimatorFloat(walking.animator, "speed", 2.f));
    Runtime::BindAnimator(walking.animator, walking.player, Cooked(kCharacter));
    Runtime::StepAnimator(walking.animator, walking.player);
    CHECK(walking.player.parameter.x == doctest::Approx(2.f));
}

TEST_CASE("Animator: a param is set only by its own name and type")
{
    Character character = Bound();
    CHECK_FALSE(Runtime::SetAnimatorBool(character.animator, "jump", true));
    CHECK_FALSE(Runtime::FireAnimatorTrigger(character.animator, "grounded"));
    CHECK_FALSE(Runtime::SetAnimatorFloat(character.animator, "grounded", 1.f));
    CHECK_FALSE(Runtime::SetAnimatorFloat(character.animator, "sped", 1.f));
    Runtime::Animator unbound;
    CHECK_FALSE(Runtime::SetAnimatorFloat(unbound, "speed", 1.f));
}

TEST_CASE("Animator: a clip param plays what the Animator binds to it")
{
    const std::string source = "use animation;\nskeleton \"" + std::string{kModel} +
                               "\";\nparam run_clip: clip;\nlayer base {\n    state run { play run_clip; }\n}\n";
    Character character;
    character.animator.clips.push_back(
        Runtime::ClipBinding{.clip = ClipId("Walk_Loop"), .name = Core::InternedString{"run_clip"}});
    Runtime::BindAnimator(character.animator, character.player, Cooked(source));
    CHECK(character.player.animation == ClipId("Walk_Loop"));

    Character unboundClip;
    Runtime::BindAnimator(unboundClip.animator, unboundClip.player, Cooked(source));
    CHECK(unboundClip.player.animation == Core::AssetId{});
    CHECK(unboundClip.animator.run.warned);
}
