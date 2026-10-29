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

#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/ECS/WorldMatrix.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Math/Matrix.hpp>
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

Physics::CharacterLocomotion Tuning()
{
    Physics::CharacterLocomotion tuning{};
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

/// A character walking off a ledge by @p movement: crouched for a few steps,
/// then standing, then a jump pressed just after the floor ends, and a while in
/// the air.
template <typename Movement = Physics::SourceMovement>
Run WalkOffLedge(PhysicsTests::TestScene &test, const Movement &movement = Movement{})
{
    (void)PhysicsTests::AddBody(test.scene, {kLedgeX - 4.f, -0.5f, 0.f},
                                PhysicsTests::Box({4.f, 0.5f, 4.f}, /*isStatic=*/ true));
    Run run;
    run.character =
        PhysicsTests::AddCharacter(test.scene, {kLedgeX - 2.f, 0.f, 0.f}, Physics::Character{}, Tuning(), movement);
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

TEST_CASE("A character stepped again against another one as a follower meets it where the live step did")
{
    // The other is a player this machine does not step again: it crosses the
    // walker's path and cannot be shoved, as another machine's player is. Left
    // where it ends up, the walker would step again into empty floor.
    // The crosser starts off the walker's path and walks across it.
    constexpr glm::vec3 kWalkerStart{-1.f, 0.f, 0.f};
    constexpr glm::vec3 kCrosserStart{0.f, 0.f, -1.5f};
    // Steps of the replayed window the two must touch in, so the replay has
    // something to meet.
    constexpr int32_t kLeastStepsTouching = 5;
    PhysicsTests::TestScene test;
    (void)PhysicsTests::AddFloor(test.scene);
    const ECS::Entity walker = PhysicsTests::AddCharacter(test.scene, kWalkerStart);
    const ECS::Entity crosser = PhysicsTests::AddCharacter(test.scene, kCrosserStart);
    test.world.SetCharacterOrderKey(walker, 0);
    test.world.SetCharacterOrderKey(crosser, 1);
    PhysicsTests::Step(test.world, kSettleSteps);
    test.world.SetCharacterPushable(crosser, false);

    std::vector<Physics::CharacterFrame> frames;
    std::vector<Physics::FollowerFrame> crossings;
    int32_t stepsTouching = 0;
    for (int32_t i = 0; i < kRideSteps; ++i)
    {
        test.scene.GetMut<Physics::CharacterIntent>(walker)->move = {1.f, 0.f, 0.f};
        test.scene.GetMut<Physics::CharacterIntent>(crosser)->move = {0.f, 0.f, 1.f};
        test.world.Update(kStep);
        frames.push_back(test.world.CaptureCharacter(walker));
        crossings.push_back(test.world.CaptureFollower(crosser));
        if (static_cast<std::size_t>(i) > kRewindTo && test.world.IsTouching(walker, crosser))
        {
            ++stepsTouching;
        }
    }
    REQUIRE(stepsTouching >= kLeastStepsTouching);

    test.world.RewindCharacter(walker, frames[kRewindTo]);
    const std::array<ECS::Entity, 1> only{walker};
    for (std::size_t i = kRewindTo + 1; i < frames.size(); ++i)
    {
        CAPTURE(i);
        *test.scene.GetMut<Physics::CharacterIntent>(walker) = frames[i].intent;
        const std::array<Physics::FollowerStep, 1> crossing{
            Physics::FollowerStep{.from = crossings[i - 1], .to = crossings[i], .entity = crosser}};
        test.world.StepCharacters(only, crossing, kStep);
        CheckSameFrame(test.world.CaptureCharacter(walker), frames[i], glm::vec3(0.f), kReplayTolerance);
    }
}

TEST_CASE("A follower that is also stepped is stepped, not put back")
{
    // A world lists every character as a follower; the one a call steps must
    // not be placed back where its record says on top of its own step.
    PhysicsTests::TestScene test;
    (void)PhysicsTests::AddFloor(test.scene);
    const ECS::Entity walker = PhysicsTests::AddCharacter(test.scene, {0.f, 0.f, 0.f});
    PhysicsTests::Step(test.world, kSettleSteps);

    const Physics::FollowerFrame before = test.world.CaptureFollower(walker);
    test.scene.GetMut<Physics::CharacterIntent>(walker)->move = {1.f, 0.f, 0.f};
    test.world.Update(kStep);
    const Physics::CharacterFrame live = test.world.CaptureCharacter(walker);

    test.world.RewindCharacter(walker, live);
    const std::array<ECS::Entity, 1> only{walker};
    const std::array<Physics::FollowerStep, 1> itself{
        Physics::FollowerStep{.from = before, .to = before, .entity = walker}};
    test.world.StepCharacters(only, itself, kStep);
    CHECK(test.world.CaptureCharacter(walker).position.x > live.position.x);
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
    SUBCASE("for a character moving by ArcadeMovement")
    {
        PhysicsTests::TestScene test;
        const Run run = WalkOffLedge(test, Physics::ArcadeMovement{});
        ReplayFrom(test, run, 0);
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

namespace
{

/// A light crate's half size (m) and mass (kg): one a character shoves along
/// at a walk.
constexpr float kCrateHalf = 0.4f;
constexpr float kCrateMass = 10.f;

/// Steps a character walks into a crate in front of it, the push included.
constexpr int32_t kPushSteps = 60;

/// The step of the push a replay goes back to: the character already
/// pressing against the crate.
constexpr std::size_t kPushRewindTo = 30;

/// A character walking a crate along the floor: each step's frame, and the
/// crate's after it.
struct PushRun
{
    std::vector<Physics::CharacterFrame> frames;
    std::vector<Physics::FollowerFrame> crate;
    ECS::Entity pusher{ECS::NullEntity};
    ECS::Entity crateEntity{ECS::NullEntity};
};

PushRun PushCrate(PhysicsTests::TestScene &test)
{
    (void)PhysicsTests::AddFloor(test.scene);
    PhysicsTests::BodySpec spec = PhysicsTests::Box(glm::vec3(kCrateHalf), /*isStatic=*/ false);
    spec.rigidBody->mass = kCrateMass;
    PushRun run;
    run.crateEntity = PhysicsTests::AddBody(test.scene, {1.f, kCrateHalf, 0.f}, spec);
    run.pusher = PhysicsTests::AddCharacter(test.scene, {0.f, 0.f, 0.f});
    PhysicsTests::Step(test.world, kSettleSteps);
    for (int32_t i = 0; i < kPushSteps; ++i)
    {
        test.scene.GetMut<Physics::CharacterIntent>(run.pusher)->move = {1.f, 0.f, 0.f};
        test.world.Update(kStep);
        run.frames.push_back(test.world.CaptureCharacter(run.pusher));
        run.crate.push_back(test.world.CaptureFollower(run.crateEntity));
    }
    return run;
}

} // namespace

TEST_CASE("A character stepped again into a crate is stopped by it and does not push it")
{
    // The crate took the push when the step ran live. Pushed again by the step
    // stepped again, it would carry a second push into the next step.
    PhysicsTests::TestScene test;
    const PushRun run = PushCrate(test);
    REQUIRE(run.crate.back().pose.position.x > run.crate.front().pose.position.x);

    test.world.ApplyCorrection(run.crateEntity, run.crate[kPushRewindTo].pose, Physics::BodyState{});
    test.world.RewindCharacter(run.pusher, run.frames[kPushRewindTo - 1]);
    const std::array<ECS::Entity, 1> only{run.pusher};
    for (std::size_t i = kPushRewindTo; i < run.frames.size(); ++i)
    {
        *test.scene.GetMut<Physics::CharacterIntent>(run.pusher) = run.frames[i].intent;
        test.world.StepCharacters(only, {}, kStep);
    }
    CHECK(glm::length(test.world.GetBodyState(run.crateEntity).linearVelocity) == 0.f);
    CHECK(test.world.CaptureCharacter(run.pusher).position.x < run.crate[kPushRewindTo].pose.position.x);
}

TEST_CASE("A character stepped again against a crate as a follower meets it where the live step did")
{
    PhysicsTests::TestScene test;
    const PushRun run = PushCrate(test);
    REQUIRE(test.world.IsTouching(run.pusher, run.crateEntity));

    test.world.RewindCharacter(run.pusher, run.frames[kPushRewindTo]);
    const std::array<ECS::Entity, 1> only{run.pusher};
    for (std::size_t i = kPushRewindTo + 1; i < run.frames.size(); ++i)
    {
        CAPTURE(i);
        *test.scene.GetMut<Physics::CharacterIntent>(run.pusher) = run.frames[i].intent;
        const std::array<Physics::FollowerStep, 1> crate{
            Physics::FollowerStep{.from = run.crate[i - 1], .to = run.crate[i], .entity = run.crateEntity}};
        test.world.StepCharacters(only, crate, kStep);
        CheckSameFrame(test.world.CaptureCharacter(run.pusher), run.frames[i], glm::vec3(0.f), kReplayTolerance);
    }
    const Physics::Pose crate = test.world.GetBodyPose(run.crateEntity);
    CHECK(glm::length(crate.position - run.crate.back().pose.position) < kReplayTolerance);
}

namespace
{

/// Steps a character walks past a crate's corner, from a standing start well
/// short of it.
constexpr int32_t kGlanceSteps = 90;

/// How far the crate stands ahead, and to the side so the walk catches its
/// corner (m).
constexpr glm::vec3 kGlanceCrateAt{1.5f, kCrateHalf, 0.4f};

/// The walk, a little to the side of the crate's centre.
constexpr glm::vec3 kGlanceMove{1.f, 0.f, 0.25f};

} // namespace

TEST_CASE("A character glancing off a crate as a follower, stepped again from each step, lands where it did live")
{
    // The sync test's shape: each step stepped again on its own from the frame
    // before it, so the first touch is one of them.
    PhysicsTests::TestScene test;
    (void)PhysicsTests::AddFloor(test.scene);
    PhysicsTests::BodySpec spec = PhysicsTests::Box(glm::vec3(kCrateHalf), /*isStatic=*/ false);
    spec.rigidBody->mass = kCrateMass;
    const ECS::Entity crate = PhysicsTests::AddBody(test.scene, kGlanceCrateAt, spec);
    const ECS::Entity walker = PhysicsTests::AddCharacter(test.scene, {0.f, 0.f, 0.f});
    PhysicsTests::Step(test.world, kSettleSteps);
    std::vector<Physics::CharacterFrame> frames{test.world.CaptureCharacter(walker)};
    std::vector<Physics::FollowerFrame> crateFrames{test.world.CaptureFollower(crate)};
    bool touched = false;
    for (int32_t i = 0; i < kGlanceSteps; ++i)
    {
        test.scene.GetMut<Physics::CharacterIntent>(walker)->move = glm::normalize(kGlanceMove);
        test.world.Update(kStep);
        frames.push_back(test.world.CaptureCharacter(walker));
        crateFrames.push_back(test.world.CaptureFollower(crate));
        touched = touched || test.world.IsTouching(walker, crate);
    }
    REQUIRE(touched);

    const std::array<ECS::Entity, 1> only{walker};
    for (std::size_t i = 1; i < frames.size(); ++i)
    {
        CAPTURE(i);
        test.world.RewindCharacter(walker, frames[i - 1]);
        *test.scene.GetMut<Physics::CharacterIntent>(walker) = frames[i].intent;
        const std::array<Physics::FollowerStep, 1> follower{
            Physics::FollowerStep{.from = crateFrames[i - 1], .to = crateFrames[i], .entity = crate}};
        test.world.StepCharacters(only, follower, kStep);
        CheckSameFrame(test.world.CaptureCharacter(walker), frames[i], glm::vec3(0.f), kReplayTolerance);
    }
}

namespace
{

/// A step lower than a character climbs, across its way ahead: walked onto and
/// off its far edge in the steps given.
constexpr glm::vec3 kLedgeStepAt{1.5f, 0.1f, 0.f};
constexpr glm::vec3 kLedgeStepHalf{1.f, 0.1f, 2.f};
constexpr int32_t kLedgeSteps = 120;

/// Higher than this (m), the character stands on the step.
constexpr float kOnLedgeStep = 0.15f;

} // namespace

TEST_CASE("A character climbing onto a step and off it, stepped again from each step, lands where it did live")
{
    // Climbing, it stands on the step's edge even where the edge alone reads
    // as too steep, which a rewind that only placed it would not find again.
    PhysicsTests::TestScene test;
    (void)PhysicsTests::AddFloor(test.scene);
    (void)PhysicsTests::AddBody(test.scene, kLedgeStepAt, PhysicsTests::Box(kLedgeStepHalf, /*isStatic=*/ true));
    const ECS::Entity walker = PhysicsTests::AddCharacter(test.scene, {0.f, 0.f, 0.f});
    PhysicsTests::Step(test.world, kSettleSteps);
    std::vector<Physics::CharacterFrame> frames{test.world.CaptureCharacter(walker)};
    bool climbed = false;
    for (int32_t i = 0; i < kLedgeSteps; ++i)
    {
        test.scene.GetMut<Physics::CharacterIntent>(walker)->move = {1.f, 0.f, 0.f};
        test.world.Update(kStep);
        frames.push_back(test.world.CaptureCharacter(walker));
        climbed = climbed || frames.back().position.y > kOnLedgeStep;
    }
    REQUIRE(climbed);
    REQUIRE(frames.back().position.x > kLedgeStepAt.x + kLedgeStepHalf.x);

    for (std::size_t i = 1; i < frames.size(); ++i)
    {
        CAPTURE(i);
        test.world.RewindCharacter(walker, frames[i - 1]);
        StepAgain(test, walker, frames[i]);
        CheckSameFrame(test.world.CaptureCharacter(walker), frames[i], glm::vec3(0.f), kReplayTolerance);
    }
}

namespace
{

/// How far a crate solved again with the character pushing it may land from
/// where the live step put it (m): the solve starts without the contacts the
/// live one carried over from the step before.
constexpr float kSolvedTolerance = 0.01f;

/// Puts @p run's crate and character back to how step @p index left them.
void RewindPush(PhysicsTests::TestScene &test, const PushRun &run, std::size_t index)
{
    const Physics::FollowerFrame &crate = run.crate[index];
    test.world.ApplyCorrection(run.crateEntity, crate.pose,
                               Physics::BodyState{.linearVelocity = crate.linearVelocity,
                                                  .angularVelocity = crate.angularVelocity,
                                                  .asleep = crate.asleep});
    test.world.RewindCharacter(run.pusher, run.frames[index]);
}

} // namespace

TEST_CASE("A crate solved again with the character stepped again into it moves as it did live")
{
    PhysicsTests::TestScene test;
    const PushRun run = PushCrate(test);
    RewindPush(test, run, kPushRewindTo - 1);
    const std::array<ECS::Entity, 1> only{run.pusher};
    const std::array<ECS::Entity, 1> solved{run.crateEntity};
    for (std::size_t i = kPushRewindTo; i < run.frames.size(); ++i)
    {
        CAPTURE(i);
        *test.scene.GetMut<Physics::CharacterIntent>(run.pusher) = run.frames[i].intent;
        test.world.StepCharacters(only, {}, kStep, solved);
        const Physics::Pose crate = test.world.GetBodyPose(run.crateEntity);
        CHECK(glm::length(crate.position - run.crate[i].pose.position) < kSolvedTolerance);
        const ECS::Transform *transform = test.scene.Get<ECS::Transform>(run.crateEntity);
        REQUIRE(transform != nullptr);
        CHECK(glm::length(transform->position - crate.position) < kReplayTolerance);
    }
    CHECK(test.world.ContactEvents().empty());
}

TEST_CASE("A crate solved again is drawn where the solve left it, not slid there from where it was")
{
    // A rollback eases the difference on screen itself; a blend on top would
    // draw the crate part way back to where the rollback began.
    constexpr float kPartWay = 0.25f;
    PhysicsTests::TestScene test;
    const PushRun run = PushCrate(test);
    RewindPush(test, run, kPushRewindTo - 1);
    std::uint64_t tick = ECS::PropagateTransforms(test.scene, 0);
    const std::array<ECS::Entity, 1> only{run.pusher};
    const std::array<ECS::Entity, 1> solved{run.crateEntity};
    {
        const ECS::FixedStepScope step(test.scene);
        *test.scene.GetMut<Physics::CharacterIntent>(run.pusher) = run.frames[kPushRewindTo].intent;
        test.world.StepCharacters(only, {}, kStep, solved);
    }
    REQUIRE(glm::length(test.world.GetBodyPose(run.crateEntity).position - run.crate[kPushRewindTo - 1].pose.position) >
            kSolvedTolerance);
    ECS::SetBlendAlpha(test.scene, kPartWay);
    tick = ECS::PropagateTransforms(test.scene, tick);
    const glm::vec3 drawn = Math::TranslationOf(test.scene.Get<ECS::WorldMatrix>(run.crateEntity)->matrix);
    CHECK(glm::length(drawn - test.world.GetBodyPose(run.crateEntity).position) < kReplayTolerance);
}

TEST_CASE("A follower that is also solved is solved, not put where its step left it")
{
    PhysicsTests::TestScene test;
    const PushRun run = PushCrate(test);
    RewindPush(test, run, kPushRewindTo - 1);
    const std::array<ECS::Entity, 1> only{run.pusher};
    const std::array<ECS::Entity, 1> solved{run.crateEntity};
    Physics::FollowerFrame away = run.crate[kPushRewindTo];
    away.pose.position.z += 5.f;
    const std::array<Physics::FollowerStep, 1> crate{
        Physics::FollowerStep{.from = run.crate[kPushRewindTo - 1], .to = away, .entity = run.crateEntity}};
    *test.scene.GetMut<Physics::CharacterIntent>(run.pusher) = run.frames[kPushRewindTo].intent;
    test.world.StepCharacters(only, crate, kStep, solved);
    CHECK(glm::length(test.world.GetBodyPose(run.crateEntity).position - run.crate[kPushRewindTo].pose.position) <
          kSolvedTolerance);
}

TEST_CASE("A crate solved again on a moving deck is carried by it as it was live")
{
    // The deck, a follower, moves through the solve as it moved live: held
    // still, it would drag the crate to a stop by its friction.
    PhysicsTests::TestScene test;
    PhysicsTests::BodySpec deckSpec =
        PhysicsTests::Box({kDeckHalfWidth, kDeckHalfThickness, kDeckHalfWidth}, /*isStatic=*/ false);
    deckSpec.rigidBody->motion = Physics::MotionType::Kinematic;
    const glm::vec3 deckStart{0.f, -kDeckHalfThickness, 0.f};
    const ECS::Entity deck = PhysicsTests::AddBody(test.scene, deckStart, deckSpec);
    PhysicsTests::BodySpec crateSpec = PhysicsTests::Box(glm::vec3(kCrateHalf), /*isStatic=*/ false);
    crateSpec.rigidBody->mass = kCrateMass;
    const ECS::Entity crate = PhysicsTests::AddBody(test.scene, {0.f, kCrateHalf, 0.f}, crateSpec);
    PhysicsTests::Step(test.world, kSettleSteps);

    std::vector<Physics::FollowerFrame> deckFrames{test.world.CaptureFollower(deck)};
    std::vector<Physics::FollowerFrame> crateFrames{test.world.CaptureFollower(crate)};
    for (int32_t i = 0; i < kRideSteps; ++i)
    {
        test.scene.GetMut<ECS::Transform>(deck)->position =
            deckStart + glm::vec3(kDeckVelocity.x, 0.f, 0.f) * (static_cast<float>(i + 1) * kStep);
        test.world.Update(kStep);
        deckFrames.push_back(test.world.CaptureFollower(deck));
        crateFrames.push_back(test.world.CaptureFollower(crate));
    }

    const Physics::FollowerFrame &rewound = crateFrames[kRewindTo];
    test.world.ApplyCorrection(crate, rewound.pose,
                               Physics::BodyState{.linearVelocity = rewound.linearVelocity,
                                                  .angularVelocity = rewound.angularVelocity});
    const std::array<ECS::Entity, 1> solved{crate};
    for (std::size_t i = kRewindTo + 1; i < crateFrames.size(); ++i)
    {
        CAPTURE(i);
        const std::array<Physics::FollowerStep, 1> carrier{
            Physics::FollowerStep{.from = deckFrames[i - 1], .to = deckFrames[i], .entity = deck}};
        test.world.StepCharacters({}, carrier, kStep, solved);
        CHECK(glm::length(test.world.GetBodyPose(crate).position - crateFrames[i].pose.position) < kSolvedTolerance);
    }
}

TEST_CASE("A solve stepped again moves no awake body but its own, and wakes none")
{
    PhysicsTests::TestScene test;
    PhysicsTests::BodySpec spec = PhysicsTests::Box(glm::vec3(kCrateHalf), /*isStatic=*/ false);
    spec.rigidBody->mass = kCrateMass;
    // One falling far off, and one put to sleep just past the crate's far
    // side, which the crate is pushed against.
    constexpr glm::vec3 kFallingAt{-10.f, 20.f, 0.f};
    constexpr glm::vec3 kFallingVelocity{0.f, -3.f, 0.f};
    constexpr glm::vec3 kAsideAt{0.f, kCrateHalf, 10.f};
    constexpr float kGap = 0.01f;
    const glm::quat upright(1.f, 0.f, 0.f, 0.f);
    const ECS::Entity falling = PhysicsTests::AddBody(test.scene, kFallingAt, spec);
    const ECS::Entity sleeping = PhysicsTests::AddBody(test.scene, kAsideAt, spec);
    const PushRun run = PushCrate(test);
    const glm::vec3 pastCrate = run.crate.back().pose.position + glm::vec3(kCrateHalf * 2.f + kGap, 0.f, 0.f);
    test.world.ApplyCorrection(sleeping, Physics::Pose{upright, pastCrate}, Physics::BodyState{.asleep = true});
    test.world.ApplyCorrection(falling, Physics::Pose{upright, kFallingAt},
                               Physics::BodyState{.linearVelocity = kFallingVelocity});
    REQUIRE(test.world.IsBodyActive(falling));
    REQUIRE_FALSE(test.world.IsBodyActive(sleeping));
    const Physics::Pose slept = test.world.GetBodyPose(sleeping);

    RewindPush(test, run, run.frames.size() - 1);
    const std::array<ECS::Entity, 1> only{run.pusher};
    const std::array<ECS::Entity, 1> solved{run.crateEntity};
    for (std::size_t i = 0; i < kPushSteps / 2; ++i)
    {
        CAPTURE(i);
        test.scene.GetMut<Physics::CharacterIntent>(run.pusher)->move = {1.f, 0.f, 0.f};
        test.world.StepCharacters(only, {}, kStep, solved);
        CHECK(test.world.GetBodyPose(falling).position == kFallingAt);
        CHECK(test.world.GetBodyState(falling).linearVelocity == kFallingVelocity);
        CHECK_FALSE(test.world.IsBodyActive(sleeping));
        CHECK(glm::length(test.world.GetBodyPose(sleeping).position - slept.position) < kReplayTolerance);
    }
    // Pressed against it, not through it: sunk as deep as Jolt lets two
    // bodies sink before it pushes them apart, and the little the sleeping
    // one gives in each solve before it is put back.
    constexpr float kSunk = 0.05f;
    CHECK(test.world.GetBodyPose(run.crateEntity).position.x < slept.position.x - kCrateHalf * 2.f + kSunk);
}

TEST_CASE("A crate at rest placed as a follower stays asleep, and one moving is woken")
{
    PhysicsTests::TestScene test;
    (void)PhysicsTests::AddFloor(test.scene);
    const ECS::Entity crate =
        PhysicsTests::AddBody(test.scene, {0.f, kCrateHalf, 0.f}, PhysicsTests::Box(glm::vec3(kCrateHalf), false));
    PhysicsTests::Step(test.world, kSettleSteps * 4);
    REQUIRE_FALSE(test.world.IsBodyActive(crate));

    const Physics::FollowerFrame resting = test.world.CaptureFollower(crate);
    CHECK(resting.asleep);
    test.world.PlaceFollower(crate, resting);
    CHECK_FALSE(test.world.IsBodyActive(crate));

    Physics::FollowerFrame sliding = resting;
    sliding.asleep = false;
    sliding.linearVelocity = {1.f, 0.f, 0.f};
    test.world.PlaceFollower(crate, sliding);
    CHECK(test.world.IsBodyActive(crate));
    CHECK(test.world.GetBodyState(crate).linearVelocity.x == doctest::Approx(1.f));

    test.world.PlaceFollower(crate, resting);
    CHECK_FALSE(test.world.IsBodyActive(crate));
    CHECK(glm::length(test.world.GetBodyState(crate).linearVelocity) == 0.f);
}
