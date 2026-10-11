/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCharacterReplay.cpp
/// @brief Putting a character back to a frame it was in, and stepping it alone
///        through the intents it was given since, reproduces where it went.
///
/// A client predicting its own character relies on this: told it was wrong at
/// some past step, it puts the character where the host says and steps it
/// again through every input it has sent since. A frame that leaves out a timer
/// or a replay that steps differently from the live step turns every such
/// correction into a visible jump.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

#include "PhysicsTestScene.hpp"

using namespace Assisi;
using Assisi::PhysicsTests::kStep;

namespace
{

/// How far a replayed step may land from the live one (m, m/s, s). Both run
/// the same arithmetic on the same inputs, so this only absorbs rounding.
constexpr float kReplayTolerance = 1e-5f;

/// Steps a settling character is given to come to rest on the floor.
constexpr int32_t kSettleSteps = 60;

/// Steps walked crouched at the start of the run, so the stance and the eye
/// height change inside the frames replayed.
constexpr int32_t kCrouchedSteps = 8;

/// Steps in the air before the jump is pressed: inside the coyote window.
constexpr int32_t kStepsBeforeJump = 2;

/// Steps of flight recorded after the jump.
constexpr int32_t kFlightSteps = 12;

/// The most steps the walk to the ledge may take before the test gives up.
constexpr int32_t kMaxWalkSteps = 240;

/// How far a run from somewhere else may stray from the same run shifted (m,
/// m/s, s). The sweep rounds differently at a different place, by hundredths
/// of a millimetre; a client tolerates two centimetres.
constexpr float kShiftedTolerance = 1e-3f;

/// How far sideways a rewind places the character from where it was (m).
constexpr float kSideways = 0.5f;

/// The x the floor ends at; nothing lies beyond it.
constexpr float kLedgeX = 0.f;

struct Run
{
    std::vector<Physics::CharacterFrame> frames;
    ECS::Entity character{ECS::NullEntity};
};

Physics::Character Tuning()
{
    Physics::Character tuning{};
    tuning.coyoteTime = 0.1f;
    tuning.jumpBufferTime = 0.1f;
    return tuning;
}

/// One live step with @p move, @p stance and @p jump asked for, and the frame
/// it left the character in.
void LiveStep(PhysicsTests::TestScene &test, Run &run, glm::vec3 move, Physics::Stance stance, bool jump)
{
    Physics::CharacterIntent &intent = *test.scene.GetMut<Physics::CharacterIntent>(run.character);
    intent.move = move;
    intent.stance = stance;
    intent.jump = jump;
    test.world.Update(kStep);
    run.frames.push_back(test.world.CaptureCharacter(run.character));
}

/// A character walking off a ledge: crouched for a few steps, then standing,
/// then a jump pressed just after the floor ends, and a while in the air.
Run WalkOffLedge(PhysicsTests::TestScene &test)
{
    (void)PhysicsTests::AddBody(test.scene, {kLedgeX - 4.f, -0.5f, 0.f},
                                PhysicsTests::Box({4.f, 0.5f, 4.f}, /*isStatic=*/ true));
    Run run;
    run.character = PhysicsTests::AddCharacter(test.scene, {kLedgeX - 2.f, 0.f, 0.f}, Tuning());
    test.world.Reconcile();
    REQUIRE(test.world.HasBody(run.character));
    for (int32_t i = 0; i < kSettleSteps; ++i)
    {
        test.world.Update(kStep);
    }

    const glm::vec3 forward{1.f, 0.f, 0.f};
    for (int32_t i = 0; i < kCrouchedSteps; ++i)
    {
        LiveStep(test, run, forward, Physics::Stance::Crouching, /*jump=*/ false);
    }
    for (int32_t i = 0; PhysicsTests::StateOf(test.scene, run.character).ground == Physics::GroundState::OnGround;
         ++i)
    {
        REQUIRE(i < kMaxWalkSteps);
        LiveStep(test, run, forward, Physics::Stance::Standing, /*jump=*/ false);
    }
    for (int32_t i = 0; i < kStepsBeforeJump; ++i)
    {
        LiveStep(test, run, forward, Physics::Stance::Standing, /*jump=*/ false);
    }
    LiveStep(test, run, forward, Physics::Stance::Standing, /*jump=*/ true);
    for (int32_t i = 0; i < kFlightSteps; ++i)
    {
        LiveStep(test, run, forward, Physics::Stance::Standing, /*jump=*/ false);
    }
    return run;
}

void CheckSameFrame(const Physics::CharacterFrame &replayed, const Physics::CharacterFrame &live, glm::vec3 shift,
                    float tolerance)
{
    CHECK(glm::length(replayed.position - (live.position + shift)) < tolerance);
    CHECK(glm::length(replayed.velocity - live.velocity) < tolerance);
    CHECK(replayed.timeSinceGrounded == doctest::Approx(live.timeSinceGrounded).epsilon(tolerance));
    CHECK(replayed.jumpBufferRemaining == doctest::Approx(live.jumpBufferRemaining).epsilon(tolerance));
    CHECK(replayed.eyeHeight == doctest::Approx(live.eyeHeight).epsilon(tolerance));
    CHECK(replayed.stance == live.stance);
    CHECK(replayed.jumpedSinceGrounded == live.jumpedSinceGrounded);
}

/// Rewinds to @p run's frame @p from and replays every later frame's intent,
/// checking each step lands on the live run's frame.
void ReplayFrom(PhysicsTests::TestScene &test, const Run &run, std::size_t from)
{
    test.world.RewindCharacter(run.character, run.frames[from]);
    for (std::size_t i = from + 1; i < run.frames.size(); ++i)
    {
        CAPTURE(i);
        test.world.ReplayCharacter(run.character, run.frames[i].intent, run.frames[i].facing, kStep);
        CheckSameFrame(test.world.CaptureCharacter(run.character), run.frames[i], glm::vec3(0.f), kReplayTolerance);
    }
}

} // namespace

TEST_CASE("A frame records the intent its step consumed, the jump included")
{
    PhysicsTests::TestScene test;
    const Run run = WalkOffLedge(test);

    // A jump is cleared from CharacterIntent by the step that takes it, so the
    // frame is the only place it is still known afterwards.
    const std::size_t jumped = run.frames.size() - static_cast<std::size_t>(kFlightSteps) - 1;
    CHECK(run.frames[jumped].intent.jump);
    CHECK_FALSE(run.frames[jumped + 1].intent.jump);
    CHECK(run.frames[jumped].velocity.y > run.frames[jumped - 1].velocity.y);
    CHECK(run.frames.front().intent.stance == Physics::Stance::Crouching);
}

TEST_CASE("Rewinding to a frame and replaying its intents reproduces the live run")
{
    SUBCASE("from the first frame, through a stand-up, a ledge and a coyote jump")
    {
        PhysicsTests::TestScene test;
        const Run run = WalkOffLedge(test);
        ReplayFrom(test, run, 0);
    }
    SUBCASE("from a frame in the air, with the jump still ahead")
    {
        PhysicsTests::TestScene test;
        const Run run = WalkOffLedge(test);
        const std::size_t jumped = run.frames.size() - static_cast<std::size_t>(kFlightSteps) - 1;
        ReplayFrom(test, run, jumped - 1);
    }
}

TEST_CASE("A rewind places the character where the frame says, and the replay carries on from there")
{
    PhysicsTests::TestScene test;
    const Run run = WalkOffLedge(test);

    // The floor is wide enough for half a metre either side, so the same
    // inputs from half a metre over trace the same path half a metre over.
    const glm::vec3 shift{0.f, 0.f, kSideways};
    Physics::CharacterFrame moved = run.frames[0];
    moved.position += shift;
    test.world.RewindCharacter(run.character, moved);
    CHECK(glm::length(test.scene.Get<ECS::Transform>(run.character)->position - moved.position) < kReplayTolerance);

    for (std::size_t i = 1; i < run.frames.size(); ++i)
    {
        CAPTURE(i);
        test.world.ReplayCharacter(run.character, run.frames[i].intent, run.frames[i].facing, kStep);
        CheckSameFrame(test.world.CaptureCharacter(run.character), run.frames[i], shift, kShiftedTolerance);
    }
}
