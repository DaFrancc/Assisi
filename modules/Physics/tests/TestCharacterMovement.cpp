/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCharacterMovement.cpp
/// @brief Which movement a character has: ArcadeMovement's numbers, swapping
///        one movement component for the other, a Character with neither, and
///        CharacterLocomotion's presets and fall limit.

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
using Assisi::PhysicsTests::kStep;

namespace
{

/// Steps a character is given to come to rest on the floor.
constexpr int32_t kSettleSteps = 60;

/// Steps of walking that bring any character here to its walk speed.
constexpr int32_t kRunUpSteps = 60;

/// Steps of letting go the stopping cases watch: a fifth of a second.
constexpr int32_t kStoppingSteps = 12;

/// How near a speed must come to the one worked out by hand (m/s).
constexpr float kSpeedTolerance = 0.01f;

/// A ground acceleration that reaches any walk speed here within one step
/// (m/s²).
constexpr float kInstantAcceleration = 1000.f;

/// A static floor whose top is at height 0, reconciled into @p world at once.
void SpawnFloor(ECS::Scene &scene, Physics::PhysicsWorld &world)
{
    (void)PhysicsTests::AddFloor(scene);
    world.Reconcile();
}

/// An ArcadeMovement character with its feet at the origin.
ECS::Entity SpawnArcade(ECS::Scene &scene, Physics::PhysicsWorld &world, const Physics::CharacterLocomotion &locomotion,
                        const Physics::ArcadeMovement &movement)
{
    const ECS::Entity entity = PhysicsTests::AddCharacter(scene, glm::vec3(0.f), Physics::Character{}, locomotion,
                                                          movement);
    world.Reconcile();
    REQUIRE(world.HasBody(entity));
    return entity;
}

/// Steps @p world once with @p character asked for @p move, a direction of
/// length at most 1.
void Step(ECS::Scene &scene, Physics::PhysicsWorld &world, ECS::Entity character, glm::vec3 move, bool jump)
{
    Physics::CharacterIntent &intent = *scene.GetMut<Physics::CharacterIntent>(character);
    intent.move = move;
    intent.jump = intent.jump || jump;
    world.Update(kStep);
}

void Settle(ECS::Scene &scene, Physics::PhysicsWorld &world, ECS::Entity character)
{
    for (int32_t i = 0; i < kSettleSteps; ++i)
    {
        Step(scene, world, character, glm::vec3(0.f), /*jump=*/ false);
    }
}

/// Speed across the floor, whichever way it is heading.
float HorizontalSpeed(const ECS::Scene &scene, ECS::Entity character)
{
    const glm::vec3 velocity = PhysicsTests::StateOf(scene, character).velocity;
    return glm::length(glm::vec2(velocity.x, velocity.z));
}

/// How fast an ArcadeMovement character with a walk speed of 10 m/s is going
/// one step after starting from rest.
float SpeedAfterOneStep(float groundAcceleration)
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    SpawnFloor(scene, world);

    Physics::CharacterLocomotion locomotion = PhysicsTests::CustomLocomotion();
    locomotion.walkSpeed = 10.f;
    const ECS::Entity character =
        SpawnArcade(scene, world, locomotion, Physics::ArcadeMovement{.groundAcceleration = groundAcceleration});
    Settle(scene, world, character);

    Step(scene, world, character, {1.f, 0.f, 0.f}, /*jump=*/ false);
    return PhysicsTests::StateOf(scene, character).velocity.x;
}

/// The velocity along x of an ArcadeMovement character that jumped running
/// along +x at 5 m/s, then asked for -x for half a second of its flight.
float DriftAfterJump(float airAcceleration)
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    SpawnFloor(scene, world);

    Physics::CharacterLocomotion locomotion = PhysicsTests::CustomLocomotion();
    locomotion.walkSpeed = 5.f;
    locomotion.jumpSpeed = 8.f;
    locomotion.coyoteTime = 0.f;
    locomotion.jumpBufferTime = 0.f;
    const ECS::Entity character = SpawnArcade(
        scene, world, locomotion,
        Physics::ArcadeMovement{.groundAcceleration = kInstantAcceleration, .airAcceleration = airAcceleration});
    Settle(scene, world, character);

    for (int32_t i = 0; i < 30; ++i)
    {
        Step(scene, world, character, {1.f, 0.f, 0.f}, /*jump=*/ false);
    }
    Step(scene, world, character, {1.f, 0.f, 0.f}, /*jump=*/ true);

    for (int32_t i = 0; i < 30; ++i)
    {
        Step(scene, world, character, {-1.f, 0.f, 0.f}, /*jump=*/ false);
    }
    return PhysicsTests::StateOf(scene, character).velocity.x;
}

/// The speed an ArcadeMovement character settles at when its
/// CharacterLocomotion has @p preset and a walkSpeed field of 1 m/s.
float WalkSpeedReached(Physics::LocomotionPreset preset)
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    SpawnFloor(scene, world);

    // A walk speed no preset has, so reaching it proves the field was read.
    const Physics::CharacterLocomotion locomotion{.walkSpeed = 1.f, .preset = preset};
    const ECS::Entity character =
        SpawnArcade(scene, world, locomotion, Physics::ArcadeMovement{.groundAcceleration = kInstantAcceleration});
    Settle(scene, world, character);
    for (int32_t i = 0; i < kRunUpSteps; ++i)
    {
        Step(scene, world, character, {1.f, 0.f, 0.f}, /*jump=*/ false);
    }
    return HorizontalSpeed(scene, character);
}

/// The walk speed of the turning case (m/s), and the slow acceleration that
/// leaves a turn to turnFriction (m/s²).
constexpr float kTurnWalkSpeed = 5.f;
constexpr float kTurnAcceleration = 5.f;

/// Steps the turning case runs up for, long enough to reach kTurnWalkSpeed at
/// kTurnAcceleration, and the quarter of a second it then turns for.
constexpr int32_t kTurnRunUpSteps = 120;
constexpr int32_t kTurnSteps = 15;

/// The velocity of an ArcadeMovement character that ran along +x at walk speed,
/// then asked for +z for kTurnSteps.
glm::vec3 VelocityAfterTurning(float turnFriction)
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    SpawnFloor(scene, world);

    Physics::CharacterLocomotion locomotion = PhysicsTests::CustomLocomotion();
    locomotion.walkSpeed = kTurnWalkSpeed;
    const ECS::Entity character =
        SpawnArcade(scene, world, locomotion,
                    Physics::ArcadeMovement{.groundAcceleration = kTurnAcceleration, .turnFriction = turnFriction});
    Settle(scene, world, character);
    for (int32_t i = 0; i < kTurnRunUpSteps; ++i)
    {
        Step(scene, world, character, {1.f, 0.f, 0.f}, /*jump=*/ false);
    }
    REQUIRE(HorizontalSpeed(scene, character) == doctest::Approx(kTurnWalkSpeed).epsilon(kSpeedTolerance));

    for (int32_t i = 0; i < kTurnSteps; ++i)
    {
        Step(scene, world, character, {0.f, 0.f, 1.f}, /*jump=*/ false);
    }
    const glm::vec3 velocity = PhysicsTests::StateOf(scene, character).velocity;
    return {velocity.x, 0.f, velocity.z};
}

/// The speed of an ArcadeMovement character in the air at 5 m/s, after
/// letting go for kStoppingSteps.
float AirSpeedAfterLettingGo(float airBrakingDeceleration)
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};

    Physics::CharacterLocomotion locomotion = PhysicsTests::CustomLocomotion();
    locomotion.walkSpeed = 5.f;
    locomotion.gravityScale = 0.f; // airborne for as long as the case runs
    const ECS::Entity character = SpawnArcade(
        scene, world, locomotion,
        Physics::ArcadeMovement{.airAcceleration = kInstantAcceleration, .airBrakingDeceleration = airBrakingDeceleration});
    for (int32_t i = 0; i < kRunUpSteps; ++i)
    {
        Step(scene, world, character, {1.f, 0.f, 0.f}, /*jump=*/ false);
    }
    REQUIRE(HorizontalSpeed(scene, character) == doctest::Approx(5.f).epsilon(kSpeedTolerance));

    for (int32_t i = 0; i < kStoppingSteps; ++i)
    {
        Step(scene, world, character, glm::vec3(0.f), /*jump=*/ false);
    }
    return HorizontalSpeed(scene, character);
}

/// How fast a character asking to sprint goes, and whether its state says it
/// sprinted.
struct SprintOutcome
{
    float speed = 0.f;
    bool sprinting = false;
};

/// An Arcade-preset character, asked to sprint in @p stance, after running up.
SprintOutcome Sprinting(bool canSprint, Physics::Stance stance)
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    SpawnFloor(scene, world);

    const Physics::CharacterLocomotion locomotion{.preset = Physics::LocomotionPreset::Arcade, .canSprint = canSprint};
    const ECS::Entity character = SpawnArcade(
        scene, world, locomotion,
        Physics::ArcadeMovement{.groundAcceleration = kInstantAcceleration, .brakingDeceleration = kInstantAcceleration});
    Settle(scene, world, character);
    REQUIRE(PhysicsTests::AskStance(scene, world, character, stance));
    PhysicsTests::Face(scene, character, {1.f, 0.f, 0.f}); // running forward

    scene.GetMut<Physics::CharacterIntent>(character)->sprint = true;
    for (int32_t i = 0; i < kRunUpSteps; ++i)
    {
        Step(scene, world, character, {1.f, 0.f, 0.f}, /*jump=*/ false);
    }
    return SprintOutcome{HorizontalSpeed(scene, character), PhysicsTests::StateOf(scene, character).sprinting};
}

/// Whether an Arcade-preset character facing +x, asked to sprint along @p move,
/// sprints when its sprintMaxAngle is @p maxAngle.
bool SprintsAlong(glm::vec3 move, float maxAngle)
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    SpawnFloor(scene, world);

    const Physics::CharacterLocomotion locomotion{.sprintMaxAngle = maxAngle,
                                                  .preset = Physics::LocomotionPreset::Arcade};
    const ECS::Entity character = SpawnArcade(scene, world, locomotion, Physics::ArcadeMovement{});
    Settle(scene, world, character);
    PhysicsTests::Face(scene, character, {1.f, 0.f, 0.f});

    scene.GetMut<Physics::CharacterIntent>(character)->sprint = true;
    Step(scene, world, character, move, /*jump=*/ false);
    return PhysicsTests::StateOf(scene, character).sprinting;
}

/// Steps in the air the sprint-in-air case flips its sprint for, after taking
/// off.
constexpr int32_t kAirSteps = 5;

/// Whether a character that took off sprinting, or not, sprints after asking
/// for the opposite in the air, with @p sprintInAir as given.
bool SprintingAfterChangingInAir(bool sprintInAir, bool sprintAtTakeOff)
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    SpawnFloor(scene, world);

    const Physics::CharacterLocomotion locomotion{.preset = Physics::LocomotionPreset::Arcade,
                                                  .sprintInAir = sprintInAir};
    const ECS::Entity character = SpawnArcade(scene, world, locomotion, Physics::ArcadeMovement{});
    Settle(scene, world, character);
    PhysicsTests::Face(scene, character, {1.f, 0.f, 0.f});

    scene.GetMut<Physics::CharacterIntent>(character)->sprint = sprintAtTakeOff;
    Step(scene, world, character, {1.f, 0.f, 0.f}, /*jump=*/ true);
    REQUIRE(PhysicsTests::StateOf(scene, character).sprinting == sprintAtTakeOff);

    scene.GetMut<Physics::CharacterIntent>(character)->sprint = !sprintAtTakeOff;
    for (int32_t i = 0; i < kAirSteps; ++i)
    {
        Step(scene, world, character, {1.f, 0.f, 0.f}, /*jump=*/ false);
    }
    REQUIRE(PhysicsTests::StateOf(scene, character).ground != Physics::GroundState::OnGround);
    return PhysicsTests::StateOf(scene, character).sprinting;
}

/// How fast a platform carries the groundSpeed case along (m/s), and how long.
constexpr glm::vec3 kPlatformVelocity{2.f, 0.f, 0.f};
constexpr int32_t kRideSteps = 60;

/// How fast a character with nothing under it falls a second after letting go.
float FallSpeedAfterASecond(float maxFallSpeed)
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};

    const Physics::CharacterLocomotion locomotion{.maxFallSpeed = maxFallSpeed};
    const ECS::Entity character = SpawnArcade(scene, world, locomotion, Physics::ArcadeMovement{});
    for (int32_t i = 0; i < kRunUpSteps; ++i)
    {
        Step(scene, world, character, glm::vec3(0.f), /*jump=*/ false);
    }
    return -PhysicsTests::StateOf(scene, character).velocity.y;
}

} // namespace

TEST_CASE("ArcadeMovement's groundAcceleration decides how quickly walking speed is reached")
{
    // The rate is in m/s²: at 5, one step of 1/60 s gains a twelfth of a metre
    // a second, whatever the walk speed.
    CHECK(SpeedAfterOneStep(5.f) == doctest::Approx(5.f * kStep).epsilon(0.05));

    // A rate that would overshoot in one step stops at the requested speed.
    CHECK(SpeedAfterOneStep(kInstantAcceleration) == doctest::Approx(10.f).epsilon(0.05));
}

TEST_CASE("ArcadeMovement's airAcceleration decides whether a jump can be steered")
{
    // No air control: still travelling the way it launched, at full speed.
    CHECK(DriftAfterJump(0.f) == doctest::Approx(5.f).epsilon(kSpeedTolerance));

    // Plenty of air control: reversed to the speed asked for, never past it.
    CHECK(DriftAfterJump(100.f) == doctest::Approx(-5.f).epsilon(kSpeedTolerance));
}

TEST_CASE("Letting go slows an ArcadeMovement character at its brakingDeceleration, whatever its groundAcceleration")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    SpawnFloor(scene, world);

    constexpr float kBraking = 10.f;

    Physics::CharacterLocomotion locomotion = PhysicsTests::CustomLocomotion();
    locomotion.walkSpeed = 5.f;
    const ECS::Entity character = SpawnArcade(
        scene, world, locomotion,
        Physics::ArcadeMovement{.groundAcceleration = kInstantAcceleration, .brakingDeceleration = kBraking});
    Settle(scene, world, character);

    for (int32_t i = 0; i < kRunUpSteps; ++i)
    {
        Step(scene, world, character, {1.f, 0.f, 0.f}, /*jump=*/ false);
    }
    REQUIRE(HorizontalSpeed(scene, character) == doctest::Approx(5.f).epsilon(kSpeedTolerance));

    // A fifth of a second at 10 m/s² takes 2 m/s away, exactly: the slowing is
    // the braking and nothing else, not the instant acceleration.
    for (int32_t i = 0; i < kStoppingSteps; ++i)
    {
        Step(scene, world, character, glm::vec3(0.f), /*jump=*/ false);
    }
    const float expected = 5.f - kBraking * kStep * static_cast<float>(kStoppingSteps);
    CHECK(HorizontalSpeed(scene, character) == doctest::Approx(expected).epsilon(kSpeedTolerance));
}

TEST_CASE("ArcadeMovement's turnFriction swings a character round to a new direction without losing its speed")
{
    // A slow acceleration, so that without turnFriction a turn is slow too.
    const glm::vec3 none = VelocityAfterTurning(0.f);
    CHECK(none.x > 0.5f * kTurnWalkSpeed); // still mostly heading the old way
    CHECK(glm::length(none) < 0.9f * kTurnWalkSpeed); // and slowed by the turn

    const glm::vec3 sharp = VelocityAfterTurning(20.f);
    CHECK(std::abs(sharp.x) < 0.1f * kTurnWalkSpeed);
    CHECK(sharp.z == doctest::Approx(kTurnWalkSpeed).epsilon(0.05)); // round, at full speed
}

TEST_CASE("ArcadeMovement's airBrakingDeceleration decides how much momentum letting go in the air keeps")
{
    CHECK(AirSpeedAfterLettingGo(0.f) == doctest::Approx(5.f).epsilon(kSpeedTolerance));

    const float expected = 5.f - 10.f * kStep * static_cast<float>(kStoppingSteps);
    CHECK(AirSpeedAfterLettingGo(10.f) == doctest::Approx(expected).epsilon(kSpeedTolerance));
}

TEST_CASE("CharacterIntent's speedScale multiplies the walk speed a preset gives, and a negative one stops it")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    SpawnFloor(scene, world);

    const ECS::Entity character =
        SpawnArcade(scene, world, Physics::CharacterLocomotion{.preset = Physics::LocomotionPreset::Arcade},
                    Physics::ArcadeMovement{.groundAcceleration = kInstantAcceleration,
                                            .brakingDeceleration = kInstantAcceleration});
    Settle(scene, world, character);

    scene.GetMut<Physics::CharacterIntent>(character)->speedScale = 2.f;
    for (int32_t i = 0; i < kRunUpSteps; ++i)
    {
        Step(scene, world, character, {1.f, 0.f, 0.f}, /*jump=*/ false);
    }
    CHECK(HorizontalSpeed(scene, character) ==
          doctest::Approx(2.f * Physics::kArcadeLocomotion.walkSpeed).epsilon(kSpeedTolerance));

    scene.GetMut<Physics::CharacterIntent>(character)->speedScale = -1.f;
    Step(scene, world, character, {1.f, 0.f, 0.f}, /*jump=*/ false);
    CHECK(HorizontalSpeed(scene, character) < kSpeedTolerance);
}

TEST_CASE("A sprinting SourceMovement character's take-off limits grow with its speedScale")
{
    // Disallow limits a jump to the walk speed. Measured against the walk speed
    // alone, every jump of a sprinting character would throw the sprint away.
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    SpawnFloor(scene, world);

    constexpr float kWalkSpeed = 4.f;
    constexpr float kSprint = 2.f;
    Physics::CharacterLocomotion locomotion = PhysicsTests::CustomLocomotion();
    locomotion.walkSpeed = kWalkSpeed;
    locomotion.coyoteTime = 0.f;
    locomotion.jumpBufferTime = 0.f;
    const ECS::Entity character = PhysicsTests::AddCharacter(
        scene, glm::vec3(0.f), Physics::Character{}, locomotion,
        Physics::SourceMovement{.groundAcceleration = kInstantAcceleration, .bunnyHop = Physics::BunnyHopPolicy::Disallow});
    world.Reconcile();
    Settle(scene, world, character);

    scene.GetMut<Physics::CharacterIntent>(character)->speedScale = kSprint;
    for (int32_t i = 0; i < kRunUpSteps; ++i)
    {
        Step(scene, world, character, {1.f, 0.f, 0.f}, /*jump=*/ false);
    }
    REQUIRE(HorizontalSpeed(scene, character) == doctest::Approx(kSprint * kWalkSpeed).epsilon(kSpeedTolerance));

    Step(scene, world, character, {1.f, 0.f, 0.f}, /*jump=*/ true);
    REQUIRE(PhysicsTests::StateOf(scene, character).velocity.y > 1.f);
    CHECK(HorizontalSpeed(scene, character) == doctest::Approx(kSprint * kWalkSpeed).epsilon(kSpeedTolerance));
}

TEST_CASE("Asking to sprint multiplies the walk speed by sprintSpeedScale, unless crouched or not allowed")
{
    const float walkSpeed = Physics::kArcadeLocomotion.walkSpeed;
    const float sprintScale = Physics::CharacterLocomotion{}.sprintSpeedScale;

    const SprintOutcome running = Sprinting(/*canSprint=*/ true, Physics::Stance::Standing);
    CHECK(running.speed == doctest::Approx(walkSpeed * sprintScale).epsilon(kSpeedTolerance));
    CHECK(running.sprinting);

    const SprintOutcome forbidden = Sprinting(/*canSprint=*/ false, Physics::Stance::Standing);
    CHECK(forbidden.speed == doctest::Approx(walkSpeed).epsilon(kSpeedTolerance));
    CHECK_FALSE(forbidden.sprinting);

    const SprintOutcome crouched = Sprinting(/*canSprint=*/ true, Physics::Stance::Crouching);
    CHECK(crouched.speed ==
          doctest::Approx(walkSpeed * Physics::kArcadeLocomotion.crouchSpeedScale).epsilon(kSpeedTolerance));
    CHECK_FALSE(crouched.sprinting);
}

TEST_CASE("sprintMaxAngle decides which directions from where a character faces it sprints in")
{
    const glm::vec3 forward{1.f, 0.f, 0.f};
    const glm::vec3 forwardDiagonal = glm::normalize(glm::vec3(1.f, 0.f, 1.f));
    const glm::vec3 sideways{0.f, 0.f, 1.f};
    const glm::vec3 backwards{-1.f, 0.f, 0.f};

    // The default takes in the forward diagonals and no more.
    const float defaultAngle = Physics::CharacterLocomotion{}.sprintMaxAngle;
    CHECK(SprintsAlong(forward, defaultAngle));
    CHECK(SprintsAlong(forwardDiagonal, defaultAngle));
    CHECK_FALSE(SprintsAlong(sideways, defaultAngle));
    CHECK_FALSE(SprintsAlong(backwards, defaultAngle));

    // 180 is any direction.
    CHECK(SprintsAlong(backwards, 180.f));

    // Standing still is not sprinting, whatever is held.
    CHECK_FALSE(SprintsAlong(glm::vec3(0.f), 180.f));
}

TEST_CASE("Without sprintInAir a jump keeps the sprint it took off with, and with it a sprint changes in the air")
{
    // Let go of in the air: kept without sprintInAir, dropped with it.
    CHECK(SprintingAfterChangingInAir(/*sprintInAir=*/ false, /*sprintAtTakeOff=*/ true));
    CHECK_FALSE(SprintingAfterChangingInAir(/*sprintInAir=*/ true, /*sprintAtTakeOff=*/ true));

    // Asked for in the air: refused without sprintInAir, taken with it.
    CHECK_FALSE(SprintingAfterChangingInAir(/*sprintInAir=*/ false, /*sprintAtTakeOff=*/ false));
    CHECK(SprintingAfterChangingInAir(/*sprintInAir=*/ true, /*sprintAtTakeOff=*/ false));
}

TEST_CASE("groundSpeed is the speed across what a character stands on, not its speed in the world")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};

    PhysicsTests::BodySpec spec = PhysicsTests::Box({10.f, 0.25f, 10.f}, /*isStatic=*/ false);
    spec.rigidBody->motion = Physics::MotionType::Kinematic;
    const glm::vec3 platformStart{0.f, -0.25f, 0.f};
    const ECS::Entity platform = PhysicsTests::AddBody(scene, platformStart, spec);
    const ECS::Entity character = SpawnArcade(scene, world, Physics::CharacterLocomotion{}, Physics::ArcadeMovement{});
    Settle(scene, world, character);

    for (int32_t i = 0; i < kRideSteps; ++i)
    {
        scene.GetMut<ECS::Transform>(platform)->position =
            platformStart + kPlatformVelocity * (kStep * static_cast<float>(i + 1));
        Step(scene, world, character, glm::vec3(0.f), /*jump=*/ false);
    }
    const Physics::CharacterState riding = PhysicsTests::StateOf(scene, character);
    REQUIRE(riding.velocity.x == doctest::Approx(kPlatformVelocity.x).epsilon(0.05));
    CHECK(riding.groundSpeed < 0.05f); // standing still on it
}

TEST_CASE("A character standing still reads no velocity, though each step's gravity presses it into the ground")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    SpawnFloor(scene, world);
    const ECS::Entity character = SpawnArcade(scene, world, Physics::CharacterLocomotion{}, Physics::ArcadeMovement{});
    Settle(scene, world, character);

    const Physics::CharacterState standing = PhysicsTests::StateOf(scene, character);
    REQUIRE(standing.ground == Physics::GroundState::OnGround);
    CHECK(standing.velocity.x == doctest::Approx(0.f));
    CHECK(standing.velocity.y == doctest::Approx(0.f));
    CHECK(standing.velocity.z == doctest::Approx(0.f));
}

TEST_CASE("jumps and landings count every jump and every landing, and a rewind puts them back")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    SpawnFloor(scene, world);

    // Dropped from a metre up: a fall that ends in a landing, with no jump.
    const ECS::Entity character =
        SpawnArcade(scene, world, Physics::CharacterLocomotion{}, Physics::ArcadeMovement{});
    scene.GetMut<ECS::Transform>(character)->position = {0.f, 1.f, 0.f};
    world.Reconcile();
    Settle(scene, world, character);
    CHECK(PhysicsTests::StateOf(scene, character).jumps == 0u);
    CHECK(PhysicsTests::StateOf(scene, character).landings == 1u);

    const Physics::CharacterFrame beforeJump = world.CaptureCharacter(character);
    Step(scene, world, character, glm::vec3(0.f), /*jump=*/ true);
    CHECK(PhysicsTests::StateOf(scene, character).jumps == 1u);
    CHECK(PhysicsTests::StateOf(scene, character).landings == 1u); // still in the air
    Settle(scene, world, character);
    CHECK(PhysicsTests::StateOf(scene, character).landings == 2u);

    // Back to before the jump, the counts with it: stepping the jump again
    // does not count it twice.
    world.RewindCharacter(character, beforeJump);
    Step(scene, world, character, glm::vec3(0.f), /*jump=*/ true);
    CHECK(PhysicsTests::StateOf(scene, character).jumps == 1u);
}

TEST_CASE("maxFallSpeed holds a falling character to it, and zero sets no limit")
{
    CHECK(FallSpeedAfterASecond(5.f) == doctest::Approx(5.f).epsilon(kSpeedTolerance));
    CHECK(FallSpeedAfterASecond(0.f) > 9.f); // the world's gravity for a second
}

TEST_CASE("Holding one direction in the air brings an ArcadeMovement character to its walk speed, and no further")
{
    // A SourceMovement character doing the same stays at its air cap, a sixth
    // of the walk speed.
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};

    Physics::CharacterLocomotion locomotion = PhysicsTests::CustomLocomotion();
    locomotion.gravityScale = 0.f; // airborne for as long as the case runs
    const ECS::Entity character = SpawnArcade(scene, world, locomotion, Physics::ArcadeMovement{});

    float fastest = 0.f;
    for (int32_t i = 0; i < kRunUpSteps; ++i)
    {
        Step(scene, world, character, {1.f, 0.f, 0.f}, /*jump=*/ false);
        fastest = glm::max(fastest, HorizontalSpeed(scene, character));
    }

    CHECK(HorizontalSpeed(scene, character) == doctest::Approx(locomotion.walkSpeed).epsilon(kSpeedTolerance));
    CHECK(fastest <= locomotion.walkSpeed + kSpeedTolerance);
}

TEST_CASE("Swapping SourceMovement for ArcadeMovement while the character runs switches how it moves")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    SpawnFloor(scene, world);

    constexpr float kBraking = 5.f;

    const ECS::Entity character = PhysicsTests::AddCharacter(scene, glm::vec3(0.f));
    world.Reconcile();
    Settle(scene, world, character);
    for (int32_t i = 0; i < kRunUpSteps; ++i)
    {
        Step(scene, world, character, {1.f, 0.f, 0.f}, /*jump=*/ false);
    }
    const float walkSpeed = Physics::kSourceLocomotion.walkSpeed;
    REQUIRE(HorizontalSpeed(scene, character) == doctest::Approx(walkSpeed).epsilon(kSpeedTolerance));

    REQUIRE(scene.Remove<Physics::SourceMovement>(character) == ECS::RemoveResult::Removed);
    REQUIRE(scene.Add(character, Physics::ArcadeMovement{.brakingDeceleration = kBraking}) != nullptr);

    // Under SourceMovement's friction a fifth of a second would have taken
    // nearly all of it; braking at 5 m/s² takes 1 m/s.
    for (int32_t i = 0; i < kStoppingSteps; ++i)
    {
        Step(scene, world, character, glm::vec3(0.f), /*jump=*/ false);
    }
    const float expected = walkSpeed - kBraking * kStep * static_cast<float>(kStoppingSteps);
    CHECK(HorizontalSpeed(scene, character) == doctest::Approx(expected).epsilon(kSpeedTolerance));
}

TEST_CASE("A Character with no movement component neither walks nor jumps, and stops where it stands")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    SpawnFloor(scene, world);

    const ECS::Entity character = PhysicsTests::AddCharacter(scene, glm::vec3(0.f));
    world.Reconcile();
    Settle(scene, world, character);
    for (int32_t i = 0; i < kRunUpSteps; ++i)
    {
        Step(scene, world, character, {1.f, 0.f, 0.f}, /*jump=*/ false);
    }
    REQUIRE(HorizontalSpeed(scene, character) > 1.f);
    REQUIRE(PhysicsTests::StateOf(scene, character).canJump);

    // Its movement taken away mid-stride: the next step holds it still on the
    // floor, and no request moves it again.
    REQUIRE(scene.Remove<Physics::SourceMovement>(character) == ECS::RemoveResult::Removed);
    Step(scene, world, character, {1.f, 0.f, 0.f}, /*jump=*/ false);
    CHECK(HorizontalSpeed(scene, character) < kSpeedTolerance);
    CHECK_FALSE(PhysicsTests::StateOf(scene, character).canJump);

    const float x = scene.Get<ECS::Transform>(character)->position.x;
    for (int32_t i = 0; i < kSettleSteps; ++i)
    {
        Step(scene, world, character, {1.f, 0.f, 0.f}, /*jump=*/ true);
    }
    CHECK(scene.Get<ECS::Transform>(character)->position.x == doctest::Approx(x));
    CHECK(PhysicsTests::StateOf(scene, character).velocity.y < kSpeedTolerance); // not rising: no jump fired
    CHECK(PhysicsTests::StateOf(scene, character).ground == Physics::GroundState::OnGround);
}

TEST_CASE("Editing a live character's CharacterLocomotion retunes it")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    SpawnFloor(scene, world);

    Physics::CharacterLocomotion locomotion = PhysicsTests::CustomLocomotion();
    locomotion.walkSpeed = 4.f;
    const ECS::Entity character =
        SpawnArcade(scene, world, locomotion, Physics::ArcadeMovement{.groundAcceleration = kInstantAcceleration});
    Settle(scene, world, character);
    for (int32_t i = 0; i < kRunUpSteps; ++i)
    {
        Step(scene, world, character, {1.f, 0.f, 0.f}, /*jump=*/ false);
    }
    REQUIRE(HorizontalSpeed(scene, character) == doctest::Approx(4.f).epsilon(kSpeedTolerance));

    scene.GetMut<Physics::CharacterLocomotion>(character)->walkSpeed = 6.f;
    Step(scene, world, character, {1.f, 0.f, 0.f}, /*jump=*/ false);
    CHECK(HorizontalSpeed(scene, character) == doctest::Approx(6.f).epsilon(kSpeedTolerance));
}

TEST_CASE("A locomotion preset answers for the fields it covers, and Custom reads them")
{
    CHECK(WalkSpeedReached(Physics::LocomotionPreset::Arcade) ==
          doctest::Approx(Physics::kArcadeLocomotion.walkSpeed).epsilon(kSpeedTolerance));
    CHECK(WalkSpeedReached(Physics::LocomotionPreset::Source) ==
          doctest::Approx(Physics::kSourceLocomotion.walkSpeed).epsilon(kSpeedTolerance));
    CHECK(WalkSpeedReached(Physics::LocomotionPreset::Custom) == doctest::Approx(1.f).epsilon(kSpeedTolerance));
}
