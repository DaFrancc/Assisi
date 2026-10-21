/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCharacterReplay.cpp
/// @brief Putting a character back to a frame it was in, and stepping it again
///        through the intents it was given since, reproduces where it went.
///
/// A client predicting its own character relies on this: told it was wrong at
/// some past step, it puts the character where the host says and steps it
/// again through every input it has sent since. A frame that leaves out a timer
/// or a replay that steps differently from the live step turns every such
/// correction into a visible jump.

#include <doctest/doctest.h>

#include <array>
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
    CHECK(replayed.ground == live.ground);
    CHECK(replayed.jumpedSinceGrounded == live.jumpedSinceGrounded);
}

/// Steps @p character again, alone, on the intent @p frame's step consumed.
void StepAgain(PhysicsTests::TestScene &test, ECS::Entity character, const Physics::CharacterFrame &frame)
{
    *test.scene.GetMut<Physics::CharacterIntent>(character) = frame.intent;
    const std::array<ECS::Entity, 1> only{character};
    test.world.StepCharacters(only, {}, kStep);
}

/// Rewinds to @p run's frame @p from and replays every later frame's intent,
/// checking each step lands on the live run's frame.
void ReplayFrom(PhysicsTests::TestScene &test, const Run &run, std::size_t from)
{
    test.world.RewindCharacter(run.character, run.frames[from]);
    for (std::size_t i = from + 1; i < run.frames.size(); ++i)
    {
        CAPTURE(i);
        StepAgain(test, run.character, run.frames[i]);
        CheckSameFrame(test.world.CaptureCharacter(run.character), run.frames[i], glm::vec3(0.f), kReplayTolerance);
    }
}

} // namespace

namespace
{

/// Half the deck's width and length (m), and its thickness.
constexpr float kDeckHalfWidth = 6.f;
constexpr float kDeckHalfThickness = 0.25f;

/// How the deck travels and turns while the rider walks on it.
constexpr glm::vec3 kDeckVelocity{1.5f, 0.f, 0.5f};
constexpr float kDeckYawRate = 1.f; ///< rad/s.

/// Steps the rider walks on the moving deck.
constexpr int32_t kRideSteps = 40;

/// The step of the ride a replay goes back to.
constexpr std::size_t kRewindTo = 10;

/// A rider walking across a travelling, turning deck: each step's frame, the
/// rider's facing after it, and the deck's motion over it.
struct DeckRun
{
    std::vector<Physics::CharacterFrame> frames;
    std::vector<glm::quat> facings;
    std::vector<Physics::FollowerFrame> deck;
    ECS::Entity rider{ECS::NullEntity};
    ECS::Entity deckEntity{ECS::NullEntity};
};

DeckRun RideAcrossDeck(PhysicsTests::TestScene &test)
{
    PhysicsTests::BodySpec spec = PhysicsTests::Box({kDeckHalfWidth, kDeckHalfThickness, kDeckHalfWidth}, false);
    spec.rigidBody->motion = Physics::MotionType::Kinematic;
    DeckRun run;
    const glm::vec3 deckStart{0.f, -kDeckHalfThickness, 0.f};
    run.deckEntity = PhysicsTests::AddBody(test.scene, deckStart, spec);
    REQUIRE(test.scene.Add(run.deckEntity, Physics::Carrier{}) != nullptr);
    run.rider = PhysicsTests::AddCharacter(test.scene, {1.f, 0.f, 2.f});
    PhysicsTests::Step(test.world, kSettleSteps);

    for (int32_t i = 0; i < kRideSteps; ++i)
    {
        const float seconds = static_cast<float>(i + 1) * kStep;
        ECS::Transform &deck = *test.scene.GetMut<ECS::Transform>(run.deckEntity);
        deck.position = deckStart + kDeckVelocity * seconds;
        deck.rotation = glm::angleAxis(kDeckYawRate * seconds, glm::vec3(0.f, 1.f, 0.f));
        test.scene.GetMut<Physics::CharacterIntent>(run.rider)->move = {0.f, 0.f, -1.f};
        test.world.Update(kStep);
        run.frames.push_back(test.world.CaptureCharacter(run.rider));
        run.facings.push_back(test.scene.Get<ECS::Transform>(run.rider)->rotation);
        run.deck.push_back(test.world.CaptureFollower(run.deckEntity));
    }
    return run;
}

} // namespace

TEST_CASE("A rider rewound on a moving deck, and stepped again with the deck's recorded motion, rides as it did")
{
    PhysicsTests::TestScene test;
    const DeckRun run = RideAcrossDeck(test);
    REQUIRE(run.frames[kRewindTo].base == run.deckEntity);

    test.world.RewindCharacter(run.rider, run.frames[kRewindTo]);
    test.scene.GetMut<ECS::Transform>(run.rider)->rotation = run.facings[kRewindTo];
    const std::array<ECS::Entity, 1> riders{run.rider};
    for (std::size_t i = kRewindTo + 1; i < run.frames.size(); ++i)
    {
        CAPTURE(i);
        *test.scene.GetMut<Physics::CharacterIntent>(run.rider) = run.frames[i].intent;
        const std::array<Physics::FollowerStep, 1> deck{
            Physics::FollowerStep{.from = run.deck[i - 1], .to = run.deck[i], .entity = run.deckEntity}};
        test.world.StepCharacters(riders, deck, kStep);
        CheckSameFrame(test.world.CaptureCharacter(run.rider), run.frames[i], glm::vec3(0.f), kReplayTolerance);
        CHECK(std::abs(glm::dot(test.scene.Get<ECS::Transform>(run.rider)->rotation, run.facings[i])) >
              1.f - kReplayTolerance);
    }
}

TEST_CASE("Two characters pushing each other, rewound and stepped again together, push as they did")
{
    constexpr float kApart = 1.2f;
    PhysicsTests::TestScene test;
    (void)PhysicsTests::AddFloor(test.scene);
    const ECS::Entity left = PhysicsTests::AddCharacter(test.scene, {-kApart / 2.f, 0.f, 0.f});
    const ECS::Entity right = PhysicsTests::AddCharacter(test.scene, {kApart / 2.f, 0.f, 0.f});
    PhysicsTests::Step(test.world, kSettleSteps);

    std::vector<std::array<Physics::CharacterFrame, 2>> frames;
    for (int32_t i = 0; i < kRideSteps; ++i)
    {
        test.scene.GetMut<Physics::CharacterIntent>(left)->move = {1.f, 0.f, 0.f};
        test.scene.GetMut<Physics::CharacterIntent>(right)->move = {-1.f, 0.f, 0.f};
        test.world.Update(kStep);
        frames.push_back({test.world.CaptureCharacter(left), test.world.CaptureCharacter(right)});
    }

    test.world.RewindCharacter(left, frames[kRewindTo][0]);
    test.world.RewindCharacter(right, frames[kRewindTo][1]);
    const std::array<ECS::Entity, 2> both{left, right};
    for (std::size_t i = kRewindTo + 1; i < frames.size(); ++i)
    {
        CAPTURE(i);
        *test.scene.GetMut<Physics::CharacterIntent>(left) = frames[i][0].intent;
        *test.scene.GetMut<Physics::CharacterIntent>(right) = frames[i][1].intent;
        test.world.StepCharacters(both, {}, kStep);
        CheckSameFrame(test.world.CaptureCharacter(left), frames[i][0], glm::vec3(0.f), kReplayTolerance);
        CheckSameFrame(test.world.CaptureCharacter(right), frames[i][1], glm::vec3(0.f), kReplayTolerance);
    }
}

TEST_CASE("A push given in a step stepped again lands as it did live, and only once")
{
    // A system shoves the character in one step, live and again in the replay
    // of it: the replay must take the shove, and leave none for the next step.
    constexpr glm::vec3 kShove{0.f, 0.f, 200.f};
    constexpr std::size_t kShovedStep = 3;
    PhysicsTests::TestScene test;
    (void)PhysicsTests::AddFloor(test.scene);
    const ECS::Entity character = PhysicsTests::AddCharacter(test.scene, {0.f, 0.f, 0.f});
    PhysicsTests::Step(test.world, kSettleSteps);

    const Physics::CharacterFrame start = test.world.CaptureCharacter(character);
    std::vector<Physics::CharacterFrame> frames;
    for (std::size_t i = 0; i < static_cast<std::size_t>(kFlightSteps); ++i)
    {
        if (i == kShovedStep)
        {
            test.world.AddImpulse(character, kShove);
        }
        test.world.Update(kStep);
        frames.push_back(test.world.CaptureCharacter(character));
    }

    test.world.RewindCharacter(character, start);
    const std::array<ECS::Entity, 1> only{character};
    for (std::size_t i = 0; i < frames.size(); ++i)
    {
        CAPTURE(i);
        if (i == kShovedStep)
        {
            test.world.AddImpulse(character, kShove);
        }
        *test.scene.GetMut<Physics::CharacterIntent>(character) = frames[i].intent;
        test.world.StepCharacters(only, {}, kStep);
        CheckSameFrame(test.world.CaptureCharacter(character), frames[i], glm::vec3(0.f), kReplayTolerance);
    }

    // Nothing left over for the live step after.
    const Physics::CharacterFrame replayed = test.world.CaptureCharacter(character);
    test.world.Update(kStep);
    CHECK(test.world.CaptureCharacter(character).velocity.z <= replayed.velocity.z);
}

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
        StepAgain(test, run.character, run.frames[i]);
        CheckSameFrame(test.world.CaptureCharacter(run.character), run.frames[i], shift, kShiftedTolerance);
    }
}
