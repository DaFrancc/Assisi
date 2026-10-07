/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCharacterBasing.cpp
/// @brief A character riding a carrier: moved in the carrier's frame while it
///        stands on it, and let go with the carrier's speed when it leaves.
///
/// Every carrier here is a kinematic deck the test moves by writing its
/// Transform each step, so its motion is exact and known; the rider is judged in
/// the deck's frame, which is where riding is supposed to make it still.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

#include "PhysicsTestScene.hpp"

using namespace Assisi;
using namespace Assisi::PhysicsTests;

namespace
{

/// Half the deck's width and length (m): room to stand well off its yaw axis
/// and walk a few metres without stepping off.
constexpr float kDeckHalfWidth = 6.f;

/// Half the deck's thickness (m).
constexpr float kDeckHalfThickness = 0.25f;

/// Steps a dropped character takes to land and come to rest before the deck
/// starts moving.
constexpr int32_t kSettleSteps = 30;

/// Steps in a second at the fixed step.
constexpr int32_t kStepsPerSecond = 60;

/// How far a standing rider may wander in the deck's frame over a ride (m).
constexpr float kStandingDrift = 0.05f;

/// How far off its straight line a rider walking on the deck may end up (m).
constexpr float kWalkingDrift = 0.1f;

/// How far a rider's facing may be from the deck's turn (radians).
const float kFacingTolerance = glm::radians(2.f);

/// How close a speed carried off the deck must be to the deck's (m/s).
constexpr float kCarriedSpeedTolerance = 0.2f;

const glm::vec3 kUp{0.f, 1.f, 0.f};
const glm::vec3 kForward{0.f, 0.f, -1.f};

/// A kinematic deck that moves at a constant velocity and turns at a constant
/// rate about world up, from @p start.
struct Deck
{
    glm::vec3 start{0.f};
    glm::vec3 velocity{0.f};
    ECS::Entity entity{ECS::NullEntity};
    float yawRate = 0.f; ///< rad/s.
};

/// A deck whose top face is at height 0, carrying riders unless @p carries is
/// false.
Deck AddDeck(TestScene &test, glm::vec3 velocity, float yawRate, bool carries)
{
    BodySpec spec = Box({kDeckHalfWidth, kDeckHalfThickness, kDeckHalfWidth}, false);
    spec.rigidBody->motion = Physics::MotionType::Kinematic;
    spec.rigidBody->carriesRiders = carries;
    Deck deck;
    deck.start = glm::vec3(0.f, -kDeckHalfThickness, 0.f);
    deck.velocity = velocity;
    deck.yawRate = yawRate;
    deck.entity = AddBody(test.scene, deck.start, spec);
    return deck;
}

/// Writes where @p deck is @p seconds after it started moving, which the next
/// step sweeps it to.
void PlaceDeck(TestScene &test, const Deck &deck, float seconds)
{
    ECS::Transform &transform = *test.scene.GetMut<ECS::Transform>(deck.entity);
    transform.position = deck.start + deck.velocity * seconds;
    transform.rotation = glm::angleAxis(deck.yawRate * seconds, kUp);
}

/// Runs @p steps steps with the deck moving, starting @p elapsed seconds into
/// its motion. @return the seconds elapsed after them.
float Ride(TestScene &test, const Deck &deck, int32_t steps, float elapsed)
{
    for (int32_t i = 0; i < steps; ++i)
    {
        elapsed += kStep;
        PlaceDeck(test, deck, elapsed);
        Step(test.world);
    }
    return elapsed;
}

/// A character dropped onto the deck at @p local in its frame, and left to land.
ECS::Entity AddRider(TestScene &test, glm::vec3 local, const Physics::Character &character = Physics::Character{})
{
    const ECS::Entity rider = AddCharacter(test.scene, local, character);
    Step(test.world, kSettleSteps);
    return rider;
}

glm::vec3 FeetOf(const TestScene &test, ECS::Entity rider)
{
    return test.scene.Get<ECS::Transform>(rider)->position;
}

/// @p point in the deck's frame, from the deck's body as the last step left it.
glm::vec3 OnDeck(const TestScene &test, const Deck &deck, glm::vec3 point)
{
    const Physics::Pose pose = test.world.GetBodyPose(deck.entity);
    return glm::inverse(pose.rotation) * (point - pose.position);
}

/// The way @p rider faces, flattened onto the ground.
glm::vec3 FacingOf(const TestScene &test, ECS::Entity rider)
{
    glm::vec3 facing = test.scene.Get<ECS::Transform>(rider)->rotation * kForward;
    facing.y = 0.f;
    return glm::normalize(facing);
}

/// The angle between two flat directions (radians).
float AngleBetween(glm::vec3 a, glm::vec3 b)
{
    return std::acos(glm::clamp(glm::dot(a, b), -1.f, 1.f));
}

} // namespace

TEST_CASE("A character standing on a turning, travelling carrier stays where it stands on the deck")
{
    const float kYawRate = glm::radians(90.f);
    const glm::vec3 deckVelocity{1.5f, 0.f, 0.5f};
    constexpr int32_t kRideSteps = 3 * kStepsPerSecond;

    TestScene test;
    const Deck deck = AddDeck(test, deckVelocity, kYawRate, /*carries=*/ true);
    const ECS::Entity rider = AddRider(test, {2.f, 0.f, 1.f});
    const glm::vec3 before = OnDeck(test, deck, FeetOf(test, rider));

    Ride(test, deck, kRideSteps, 0.f);

    const glm::vec3 after = OnDeck(test, deck, FeetOf(test, rider));
    CHECK(glm::length(after - before) < kStandingDrift);
    CHECK(StateOf(test.scene, rider).baseEntity == deck.entity);
}

TEST_CASE("Walking on a turning carrier goes straight across the deck, and the rider turns with it")
{
    const float kYawRate = glm::radians(60.f);
    const glm::vec3 deckVelocity{1.f, 0.f, 0.f};
    constexpr float kWalkSeconds = 1.f;
    constexpr int32_t kWalkSteps = static_cast<int32_t>(kWalkSeconds * kStepsPerSecond);

    /// Less than walkSpeed times the time walked, for the time spent speeding up.
    constexpr float kLeastProgress = 2.5f;

    TestScene test;
    const Deck deck = AddDeck(test, deckVelocity, kYawRate, /*carries=*/ true);
    const ECS::Entity rider = AddRider(test, {0.f, 0.f, 3.f});
    const float walkSpeed = test.scene.Get<Physics::Character>(rider)->walkSpeed;
    const glm::vec3 before = OnDeck(test, deck, FeetOf(test, rider));

    // Forward along the rider's own facing every step, as the input system does.
    float elapsed = 0.f;
    for (int32_t i = 0; i < kWalkSteps; ++i)
    {
        Drive(test.scene, rider, FacingOf(test, rider) * walkSpeed, /*jump=*/ false);
        elapsed = Ride(test, deck, 1, elapsed);
    }

    const glm::vec3 after = OnDeck(test, deck, FeetOf(test, rider));
    CHECK(std::abs(after.x - before.x) < kWalkingDrift);
    CHECK(before.z - after.z > kLeastProgress);

    const glm::vec3 deckForward = glm::angleAxis(kYawRate * elapsed, kUp) * kForward;
    CHECK(AngleBetween(FacingOf(test, rider), deckForward) < kFacingTolerance);
}

TEST_CASE("A rider that does not turn with its base is carried and keeps its facing")
{
    const float kYawRate = glm::radians(90.f);
    constexpr int32_t kRideSteps = kStepsPerSecond;

    Physics::Character character;
    character.options = Physics::AllCharacterOptions.Without(Physics::CharacterOption::TurnsWithBase);

    TestScene test;
    const Deck deck = AddDeck(test, glm::vec3(0.f), kYawRate, /*carries=*/ true);
    const ECS::Entity rider = AddRider(test, {2.f, 0.f, 0.f}, character);
    const glm::vec3 before = OnDeck(test, deck, FeetOf(test, rider));

    Ride(test, deck, kRideSteps, 0.f);

    CHECK(glm::length(OnDeck(test, deck, FeetOf(test, rider)) - before) < kStandingDrift);
    CHECK(AngleBetween(FacingOf(test, rider), kForward) < kFacingTolerance);
}

TEST_CASE("Jumping off a carrier keeps the carrier's velocity")
{
    const glm::vec3 deckVelocity{3.f, 0.f, 0.f};
    constexpr int32_t kRideSteps = kStepsPerSecond / 2;

    /// More than enough steps to leave the deck after the jump fires.
    constexpr int32_t kMaxTakeOffSteps = 10;

    /// Steps measured in the air.
    constexpr int32_t kAirSteps = 10;

    TestScene test;
    const Deck deck = AddDeck(test, deckVelocity, 0.f, /*carries=*/ true);
    const ECS::Entity rider = AddRider(test, {0.f, 0.f, 0.f});
    float elapsed = Ride(test, deck, kRideSteps, 0.f);
    REQUIRE(StateOf(test.scene, rider).baseEntity == deck.entity);

    Drive(test.scene, rider, glm::vec3(0.f), /*jump=*/ true);
    bool released = false;
    for (int32_t i = 0; i < kMaxTakeOffSteps && !released; ++i)
    {
        elapsed = Ride(test, deck, 1, elapsed);
        const Physics::CharacterState state = StateOf(test.scene, rider);
        released = state.baseEntity == ECS::NullEntity && state.ground == Physics::GroundState::InAir;
    }
    REQUIRE(released);

    CHECK(std::abs(StateOf(test.scene, rider).velocity.x - deckVelocity.x) < kCarriedSpeedTolerance);
    const float from = FeetOf(test, rider).x;
    Ride(test, deck, kAirSteps, elapsed);
    const float travelled = FeetOf(test, rider).x - from;
    CHECK(std::abs(travelled - deckVelocity.x * kStep * kAirSteps) < kCarriedSpeedTolerance * kStep * kAirSteps);
}

namespace
{

/// Where a rider dropped on a deck travelling at @p velocity stands after a
/// second of it, and whether it was ever based.
struct Outcome
{
    glm::vec3 feet{0.f};
    bool based = false;
};

Outcome RideASecond(glm::vec3 velocity, bool carries, const Physics::Character &character)
{
    TestScene test;
    const Deck deck = AddDeck(test, velocity, 0.f, carries);
    const ECS::Entity rider = AddRider(test, {0.f, 0.f, 0.f}, character);

    Outcome outcome;
    float elapsed = 0.f;
    for (int32_t i = 0; i < kStepsPerSecond; ++i)
    {
        elapsed = Ride(test, deck, 1, elapsed);
        outcome.based = outcome.based || StateOf(test.scene, rider).baseEntity != ECS::NullEntity;
    }
    outcome.feet = FeetOf(test, rider);
    return outcome;
}

} // namespace

TEST_CASE("A body that does not carry, or a rider that does not ride, leaves the character as it was")
{
    const glm::vec3 deckVelocity{2.f, 0.f, 0.f};

    /// Far less than the two metres the deck travels: a character on a body
    /// that does not carry still rides it by taking its velocity.
    constexpr float kLeftBehindAtMost = 0.5f;

    const Outcome plain = RideASecond(deckVelocity, /*carries=*/ false, Physics::Character{});
    CHECK_FALSE(plain.based);
    CHECK(std::abs(plain.feet.x - deckVelocity.x) < kLeftBehindAtMost);

    // On a carrier, a rider that opts out moves exactly as one on a body that
    // does not carry.
    Physics::Character optedOut;
    optedOut.options = Physics::AllCharacterOptions.Without(Physics::CharacterOption::RidesBases);
    const Outcome declined = RideASecond(deckVelocity, /*carries=*/ true, optedOut);
    CHECK_FALSE(declined.based);
    CHECK(declined.feet == plain.feet);
}

TEST_CASE("Walking off a carrier onto other ground lets go of it without a jump in position")
{
    /// A static floor beside the deck, its top level with the deck's.
    constexpr float kFloorHalfWidth = 6.f;
    constexpr float kFloorCentreX = kDeckHalfWidth + kFloorHalfWidth;

    /// Steps enough to walk from near the deck's edge well onto the floor.
    constexpr int32_t kWalkSteps = kStepsPerSecond;

    /// Most a single step may move a walking character, with room for the
    /// floor's seam (m).
    constexpr float kMostPerStepFactor = 1.5f;

    TestScene test;
    const Deck deck = AddDeck(test, glm::vec3(0.f), 0.f, /*carries=*/ true);
    AddBody(test.scene, {kFloorCentreX, -kDeckHalfThickness, 0.f},
            Box({kFloorHalfWidth, kDeckHalfThickness, kDeckHalfWidth}, true));
    const ECS::Entity rider = AddRider(test, {kDeckHalfWidth - 1.f, 0.f, 0.f});
    REQUIRE(StateOf(test.scene, rider).baseEntity == deck.entity);

    const float walkSpeed = test.scene.Get<Physics::Character>(rider)->walkSpeed;
    const glm::vec3 east{1.f, 0.f, 0.f};
    float largestStep = 0.f;
    for (int32_t i = 0; i < kWalkSteps; ++i)
    {
        const glm::vec3 from = FeetOf(test, rider);
        Drive(test.scene, rider, east * walkSpeed, /*jump=*/ false);
        Step(test.world);
        largestStep = glm::max(largestStep, glm::length(FeetOf(test, rider) - from));
    }

    CHECK(FeetOf(test, rider).x > kDeckHalfWidth + 1.f);
    CHECK(StateOf(test.scene, rider).baseEntity == ECS::NullEntity);
    CHECK(largestStep < walkSpeed * kStep * kMostPerStepFactor);
}

TEST_CASE("A rider placed somewhere else by gameplay lets go of its carrier and keeps its world velocity")
{
    const glm::vec3 deckVelocity{3.f, 0.f, 0.f};
    constexpr int32_t kRideSteps = kStepsPerSecond / 2;

    /// High above the deck, so the step after the move finds nothing underfoot.
    const glm::vec3 placed{0.f, 20.f, 0.f};

    TestScene test;
    const Deck deck = AddDeck(test, deckVelocity, 0.f, /*carries=*/ true);
    const ECS::Entity rider = AddRider(test, {0.f, 0.f, 0.f});
    const float elapsed = Ride(test, deck, kRideSteps, 0.f);
    REQUIRE(StateOf(test.scene, rider).baseEntity == deck.entity);

    test.scene.GetMut<ECS::Transform>(rider)->position = placed;
    Ride(test, deck, 1, elapsed);

    const Physics::CharacterState state = StateOf(test.scene, rider);
    CHECK(state.baseEntity == ECS::NullEntity);
    CHECK(std::abs(state.velocity.x - deckVelocity.x) < kCarriedSpeedTolerance);
}
