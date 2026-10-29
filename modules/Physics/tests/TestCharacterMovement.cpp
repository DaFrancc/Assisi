/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCharacterMovement.cpp
/// @brief Which movement a character has: ArcadeMovement's numbers, swapping
///        one movement component for the other, a Character with neither, and
///        CharacterLocomotion's presets.

#include <doctest/doctest.h>

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

TEST_CASE("Letting go slows an ArcadeMovement character at its ground acceleration, with no friction")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    SpawnFloor(scene, world);

    constexpr float kGroundAcceleration = 10.f;
    constexpr int32_t kStoppingSteps = 12;

    Physics::CharacterLocomotion locomotion = PhysicsTests::CustomLocomotion();
    locomotion.walkSpeed = 5.f;
    const ECS::Entity character =
        SpawnArcade(scene, world, locomotion, Physics::ArcadeMovement{.groundAcceleration = kGroundAcceleration});
    Settle(scene, world, character);

    for (int32_t i = 0; i < kRunUpSteps; ++i)
    {
        Step(scene, world, character, {1.f, 0.f, 0.f}, /*jump=*/ false);
    }
    REQUIRE(HorizontalSpeed(scene, character) == doctest::Approx(5.f).epsilon(kSpeedTolerance));

    // A fifth of a second at 10 m/s² takes 2 m/s away, exactly: the slowing is
    // the acceleration and nothing else.
    for (int32_t i = 0; i < kStoppingSteps; ++i)
    {
        Step(scene, world, character, glm::vec3(0.f), /*jump=*/ false);
    }
    const float expected = 5.f - kGroundAcceleration * kStep * static_cast<float>(kStoppingSteps);
    CHECK(HorizontalSpeed(scene, character) == doctest::Approx(expected).epsilon(kSpeedTolerance));
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

    constexpr float kGroundAcceleration = 5.f;
    constexpr int32_t kStoppingSteps = 12;

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
    REQUIRE(scene.Add(character, Physics::ArcadeMovement{.groundAcceleration = kGroundAcceleration}) != nullptr);

    // Under SourceMovement's friction a fifth of a second would have taken
    // nearly all of it; at 5 m/s² it takes 1 m/s.
    for (int32_t i = 0; i < kStoppingSteps; ++i)
    {
        Step(scene, world, character, glm::vec3(0.f), /*jump=*/ false);
    }
    const float expected = walkSpeed - kGroundAcceleration * kStep * static_cast<float>(kStoppingSteps);
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
