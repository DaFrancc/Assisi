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
#include <optional>

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

/// A deck whose top face is at height 0, carrying riders as @p carrier says,
/// or not at all without one.
Deck AddDeck(TestScene &test, glm::vec3 velocity, float yawRate, const std::optional<Physics::Carrier> &carrier)
{
    BodySpec spec = Box({kDeckHalfWidth, kDeckHalfThickness, kDeckHalfWidth}, false);
    spec.rigidBody->motion = Physics::MotionType::Kinematic;
    Deck deck;
    deck.start = glm::vec3(0.f, -kDeckHalfThickness, 0.f);
    deck.velocity = velocity;
    deck.yawRate = yawRate;
    deck.entity = AddBody(test.scene, deck.start, spec);
    if (carrier.has_value())
    {
        REQUIRE(test.scene.Add(deck.entity, *carrier) != nullptr);
    }
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
    const Deck deck = AddDeck(test, deckVelocity, kYawRate, Physics::Carrier{});
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
    const Deck deck = AddDeck(test, deckVelocity, kYawRate, Physics::Carrier{});
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
    const Deck deck = AddDeck(test, glm::vec3(0.f), kYawRate, Physics::Carrier{});
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
    const Deck deck = AddDeck(test, deckVelocity, 0.f, Physics::Carrier{});
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

Outcome RideASecond(glm::vec3 velocity, const std::optional<Physics::Carrier> &carrier,
                    const Physics::Character &character)
{
    TestScene test;
    const Deck deck = AddDeck(test, velocity, 0.f, carrier);
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

    const Outcome plain = RideASecond(deckVelocity, std::nullopt, Physics::Character{});
    CHECK_FALSE(plain.based);
    CHECK(std::abs(plain.feet.x - deckVelocity.x) < kLeftBehindAtMost);

    // On a carrier, a rider that opts out moves exactly as one on a body that
    // does not carry.
    Physics::Character optedOut;
    optedOut.options = Physics::AllCharacterOptions.Without(Physics::CharacterOption::RidesBases);
    const Outcome declined = RideASecond(deckVelocity, Physics::Carrier{}, optedOut);
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
    const Deck deck = AddDeck(test, glm::vec3(0.f), 0.f, Physics::Carrier{});
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
    const Deck deck = AddDeck(test, deckVelocity, 0.f, Physics::Carrier{});
    const ECS::Entity rider = AddRider(test, {0.f, 0.f, 0.f});
    const float elapsed = Ride(test, deck, kRideSteps, 0.f);
    REQUIRE(StateOf(test.scene, rider).baseEntity == deck.entity);

    test.scene.GetMut<ECS::Transform>(rider)->position = placed;
    Ride(test, deck, 1, elapsed);

    const Physics::CharacterState state = StateOf(test.scene, rider);
    CHECK(state.baseEntity == ECS::NullEntity);
    CHECK(std::abs(state.velocity.x - deckVelocity.x) < kCarriedSpeedTolerance);
}

namespace
{

/// Most steps a dropped or hopping rider is given to come down.
constexpr int32_t kMaxFallSteps = 2 * kStepsPerSecond;

/// Rides @p deck until @p rider is based on it, at most kMaxFallSteps.
/// @return the seconds elapsed after them.
float RideUntilBased(TestScene &test, const Deck &deck, ECS::Entity rider, float elapsed)
{
    for (int32_t i = 0; i < kMaxFallSteps && StateOf(test.scene, rider).baseEntity != deck.entity; ++i)
    {
        elapsed = Ride(test, deck, 1, elapsed);
    }
    return elapsed;
}

/// How far a rider dropped from rest onto a deck travelling at @p velocity
/// slides across it in the second after it boards.
float SlideAfterBoarding(glm::vec3 velocity, float grip)
{
    /// Height above the deck the rider is dropped from (m).
    constexpr float kDropHeight = 1.f;

    Physics::Carrier carrier;
    carrier.grip = grip;

    TestScene test;
    const Deck deck = AddDeck(test, velocity, 0.f, carrier);
    const ECS::Entity rider = AddCharacter(test.scene, {0.f, kDropHeight, 0.f});
    const float elapsed = RideUntilBased(test, deck, rider, 0.f);
    REQUIRE(StateOf(test.scene, rider).baseEntity == deck.entity);

    const glm::vec3 boarded = OnDeck(test, deck, FeetOf(test, rider));
    Ride(test, deck, kStepsPerSecond, elapsed);
    const glm::vec3 slid = OnDeck(test, deck, FeetOf(test, rider)) - boarded;
    return glm::length(glm::vec2(slid.x, slid.z));
}

/// Where in the deck's frame a rider standing @p local on a deck turning at
/// @p yawRate lands after a hop, relative to where it took off.
glm::vec3 HopOnTurningDeck(float yawRate, float graceTime, glm::vec3 local)
{
    /// Steps ridden before the hop, so the rider is moving with the deck.
    constexpr int32_t kRideBeforeHop = kStepsPerSecond / 2;

    /// Steps the hop is given to leave the deck before landing is looked for.
    constexpr int32_t kLeaveSteps = 5;

    Physics::Carrier carrier;
    carrier.graceTime = graceTime;

    TestScene test;
    const Deck deck = AddDeck(test, glm::vec3(0.f), yawRate, carrier);
    const ECS::Entity rider = AddRider(test, local);
    float elapsed = Ride(test, deck, kRideBeforeHop, 0.f);
    const glm::vec3 tookOff = OnDeck(test, deck, FeetOf(test, rider));

    Drive(test.scene, rider, glm::vec3(0.f), /*jump=*/ true);
    elapsed = Ride(test, deck, kLeaveSteps, elapsed);
    REQUIRE(StateOf(test.scene, rider).ground == Physics::GroundState::InAir);
    for (int32_t i = 0; i < kMaxFallSteps && StateOf(test.scene, rider).ground != Physics::GroundState::OnGround; ++i)
    {
        elapsed = Ride(test, deck, 1, elapsed);
    }
    REQUIRE(StateOf(test.scene, rider).ground == Physics::GroundState::OnGround);
    return OnDeck(test, deck, FeetOf(test, rider)) - tookOff;
}

} // namespace

TEST_CASE("A carrier with full grip takes a boarding rider up to its speed; with none the rider slides")
{
    const glm::vec3 deckVelocity{6.f, 0.f, 0.f};

    /// Far less than the metre and more a rider landing at 6 m/s slides
    /// before friction stops it.
    constexpr float kGrippedSlide = 0.05f;
    constexpr float kUngrippedSlide = 0.5f;

    CHECK(SlideAfterBoarding(deckVelocity, 1.f) < kGrippedSlide);
    CHECK(SlideAfterBoarding(deckVelocity, 0.f) > kUngrippedSlide);
}

TEST_CASE("Walking from a dock onto a passing carrier keeps the walk, relative to the deck")
{
    const glm::vec3 deckVelocity{0.f, 0.f, 3.f};

    /// A static dock beside the deck's +X edge, its top level with the deck's.
    constexpr float kDockHalfWidth = 3.f;
    constexpr float kDockCentreX = kDeckHalfWidth + kDockHalfWidth;

    /// Where the rider starts on the dock, a metre from the deck.
    constexpr float kStartX = kDeckHalfWidth + 1.f;

    /// Steps after boarding before the velocity is judged, so it is not the
    /// boarding step itself.
    constexpr int32_t kStepsAfterBoarding = 5;

    /// How close the deck-relative velocity must be to the walk (m/s).
    constexpr float kWalkTolerance = 0.3f;

    TestScene test;
    const Deck deck = AddDeck(test, deckVelocity, 0.f, Physics::Carrier{});
    AddBody(test.scene, {kDockCentreX, -kDeckHalfThickness, 0.f},
            Box({kDockHalfWidth, kDeckHalfThickness, kDeckHalfWidth}, true));
    const ECS::Entity rider = AddRider(test, {kStartX, 0.f, 0.f});
    REQUIRE(StateOf(test.scene, rider).baseEntity == ECS::NullEntity);

    const float walkSpeed = test.scene.Get<Physics::Character>(rider)->walkSpeed;
    const glm::vec3 west{-walkSpeed, 0.f, 0.f};
    float elapsed = 0.f;
    for (int32_t i = 0; i < kMaxFallSteps && StateOf(test.scene, rider).baseEntity != deck.entity; ++i)
    {
        Drive(test.scene, rider, west, /*jump=*/ false);
        elapsed = Ride(test, deck, 1, elapsed);
    }
    REQUIRE(StateOf(test.scene, rider).baseEntity == deck.entity);
    for (int32_t i = 0; i < kStepsAfterBoarding; ++i)
    {
        Drive(test.scene, rider, west, /*jump=*/ false);
        elapsed = Ride(test, deck, 1, elapsed);
    }

    const glm::vec3 relative = StateOf(test.scene, rider).velocity - deckVelocity;
    CHECK(std::abs(relative.x - west.x) < kWalkTolerance);
    CHECK(std::abs(relative.z) < kWalkTolerance);
}

TEST_CASE("A carrier's grace time keeps a hopping rider in its frame, so the hop lands where it left")
{
    const float kYawRate = glm::radians(90.f);
    const glm::vec3 local{2.f, 0.f, 0.f};

    /// Longer than the hop's half a second in the air.
    constexpr float kLongGrace = 1.f;

    /// How far from its take-off point a carried hop may land (m), and how far
    /// a hop flown straight while the deck turns lands at least.
    constexpr float kCarriedHop = 0.1f;
    constexpr float kUncarriedHop = 0.3f;

    CHECK(glm::length(HopOnTurningDeck(kYawRate, kLongGrace, local)) < kCarriedHop);
    CHECK(glm::length(HopOnTurningDeck(kYawRate, 0.f, local)) > kUncarriedHop);
}

TEST_CASE("A hop longer than the grace time lets go in the air, keeping its world velocity")
{
    const glm::vec3 deckVelocity{3.f, 0.f, 0.f};
    constexpr float kGraceTime = 0.1f;
    constexpr int32_t kGraceSteps = static_cast<int32_t>(kGraceTime * kStepsPerSecond);

    /// Steps into the hop that are well inside the grace time, and steps past
    /// its end by which the rider must have been let go.
    constexpr int32_t kInsideGrace = kGraceSteps / 2;
    constexpr int32_t kPastGrace = kGraceSteps + 2;

    /// The most a step changes a falling rider's velocity: gravity's share,
    /// with room to spare (m/s).
    constexpr float kVelocityStep = 0.5f;

    Physics::Carrier carrier;
    carrier.graceTime = kGraceTime;

    TestScene test;
    const Deck deck = AddDeck(test, deckVelocity, 0.f, carrier);
    const ECS::Entity rider = AddRider(test, {0.f, 0.f, 0.f});
    float elapsed = Ride(test, deck, kStepsPerSecond / 2, 0.f);
    REQUIRE(StateOf(test.scene, rider).baseEntity == deck.entity);

    Drive(test.scene, rider, glm::vec3(0.f), /*jump=*/ true);
    glm::vec3 previous = StateOf(test.scene, rider).velocity;
    bool released = false;
    for (int32_t step = 1; step <= kPastGrace; ++step)
    {
        elapsed = Ride(test, deck, 1, elapsed);
        const Physics::CharacterState state = StateOf(test.scene, rider);
        if (step == kInsideGrace)
        {
            CHECK(state.ground != Physics::GroundState::OnGround);
            CHECK(state.baseEntity == deck.entity);
        }
        if (!released && state.baseEntity == ECS::NullEntity)
        {
            released = true;
            CHECK(step > kInsideGrace);
            CHECK(state.ground == Physics::GroundState::InAir);
            CHECK(glm::length(state.velocity - previous) < kVelocityStep);
        }
        previous = state.velocity;
    }
    CHECK(released);
}
