/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCharacterMove.cpp
/// @brief A character driven through its CharacterIntent, in a world ticked the
///        way the hosts tick it.
///
/// The controller itself is covered in the Physics suite. What is checked here is
/// the translation: that the Character's walk speed is applied, that the crouch
/// scale is applied to the stance actually reached rather than the one asked for,
/// that a jump request is consumed exactly once, and that the step publishes
/// its CharacterState. Where a game puts its camera is the game's; the
/// first-person template's tests cover its eye height.

#include <doctest/doctest.h>

#include <cstdint>

#include <Assisi/App/World.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/Matrix.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Components.hpp>

using namespace Assisi::App;

namespace
{

constexpr float kStep = 1.f / 60.f;

/// One whole fixed tick's physics step, inside the fixed-step scope the hosts
/// run it in.
void Tick(World &world)
{
    const Assisi::ECS::FixedStepScope step(world.scene);
    world.physics.Update(kStep);
}

Assisi::ECS::Entity SpawnFloor(World &world)
{
    const Assisi::ECS::Entity entity = world.scene.Create();
    Assisi::ECS::Transform *transform = world.scene.Add<Assisi::ECS::Transform>(entity);
    transform->position = {0.f, -0.5f, 0.f};

    Assisi::Physics::Collider collider{};
    collider.halfExtents = {20.f, 0.5f, 20.f};
    (void)world.scene.Add<Assisi::Physics::Collider>(entity, collider);

    world.physics.Reconcile();
    return entity;
}

/// A SourceMovement character at the origin.
Assisi::ECS::Entity SpawnCharacter(
    World &world, const Assisi::Physics::CharacterLocomotion &locomotion = Assisi::Physics::CharacterLocomotion{},
    const Assisi::Physics::SourceMovement &movement = Assisi::Physics::SourceMovement{})
{
    const Assisi::ECS::Entity entity = world.scene.Create();
    Assisi::ECS::Transform *transform = world.scene.Add<Assisi::ECS::Transform>(entity);
    transform->position = {0.f, 0.f, 0.f};
    (void)world.scene.Add<Assisi::Physics::Character>(entity);
    (void)world.scene.Add<Assisi::Physics::CharacterLocomotion>(entity, locomotion);
    (void)world.scene.Add<Assisi::Physics::SourceMovement>(entity, movement);

    world.physics.Reconcile();
    REQUIRE(world.physics.HasBody(entity));
    return entity;
}

/// Sets what the character is being asked to do this tick.
void Ask(World &world, Assisi::ECS::Entity entity, glm::vec3 move, bool jump,
         Assisi::Physics::Stance stance = Assisi::Physics::Stance::Standing)
{
    Assisi::Physics::CharacterIntent *intent = world.scene.GetMut<Assisi::Physics::CharacterIntent>(entity);
    REQUIRE(intent != nullptr);
    intent->move = move;
    intent->jump = jump;
    intent->stance = stance;
}

} // namespace

TEST_CASE("CharacterIntent's move direction is scaled by the CharacterLocomotion's walk speed")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Characters");

    (void)SpawnFloor(world);

    const Assisi::Physics::CharacterLocomotion locomotion{.walkSpeed = 6.f,
                                                          .preset = Assisi::Physics::LocomotionPreset::Custom};
    const Assisi::Physics::SourceMovement movement{.groundAcceleration = 1000.f}; // at speed within one step
    const Assisi::ECS::Entity entity = SpawnCharacter(world, locomotion, movement);

    // Let it settle onto the floor first: accelerating is a ground behaviour.
    for (int32_t i = 0; i < 60; ++i)
    {
        Ask(world, entity, glm::vec3(0.f), /*jump=*/ false);
        Tick(world);
    }

    Ask(world, entity, {1.f, 0.f, 0.f}, /*jump=*/ false);
    Tick(world);

    const Assisi::Physics::CharacterState *character = world.scene.Get<Assisi::Physics::CharacterState>(entity);
    REQUIRE(character != nullptr);

    // A direction of magnitude 1 times walkSpeed — not the raw direction, which
    // would crawl at 1 m/s whatever the descriptor said.
    CHECK(character->velocity.x == doctest::Approx(6.f).epsilon(0.1));
}

TEST_CASE("Crouching applies the CharacterLocomotion's speed scale")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Characters");

    (void)SpawnFloor(world);

    const Assisi::Physics::CharacterLocomotion locomotion{
        .walkSpeed = 6.f, .crouchSpeedScale = 0.5f, .preset = Assisi::Physics::LocomotionPreset::Custom};
    const Assisi::Physics::SourceMovement movement{.groundAcceleration = 1000.f};
    const Assisi::ECS::Entity entity = SpawnCharacter(world, locomotion, movement);

    for (int32_t i = 0; i < 60; ++i)
    {
        Ask(world, entity, glm::vec3(0.f), /*jump=*/ false);
        Tick(world);
    }

    // Nothing overhead, so the crouch is granted and the scale applies.
    Ask(world, entity, {1.f, 0.f, 0.f}, /*jump=*/ false, Assisi::Physics::Stance::Crouching);
    Tick(world);

    const Assisi::Physics::CharacterState *character = world.scene.Get<Assisi::Physics::CharacterState>(entity);
    REQUIRE(character != nullptr);
    CHECK(character->stance == Assisi::Physics::Stance::Crouching);
    CHECK(character->velocity.x == doctest::Approx(3.f).epsilon(0.1));
}

TEST_CASE("A jump request is consumed once, not held")
{
    // Otherwise a single press jumps again on every step that follows it, which
    // reads as the character sticking to the ceiling.
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Characters");

    (void)SpawnFloor(world);

    const Assisi::ECS::Entity entity = SpawnCharacter(world);

    for (int32_t i = 0; i < 60; ++i)
    {
        Ask(world, entity, glm::vec3(0.f), /*jump=*/ false);
        Tick(world);
    }

    Ask(world, entity, glm::vec3(0.f), /*jump=*/ true);
    Tick(world);

    CHECK_FALSE(world.scene.Get<Assisi::Physics::CharacterIntent>(entity)->jump);
    CHECK(world.scene.Get<Assisi::Physics::CharacterState>(entity)->velocity.y > 1.f);
}


TEST_CASE("The step takes a character's facing from its Transform")
{
    // Only the Boost policy reads the facing, so that is what shows it arrived:
    // a character over the limit and travelling backwards gains speed on a jump.
    // With the facing left at its default it would be slowed instead.
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Characters");

    (void)SpawnFloor(world);

    const Assisi::Physics::CharacterLocomotion locomotion{.walkSpeed = 4.f,
                                                          .coyoteTime = 0.f,
                                                          .jumpBufferTime = 0.f,
                                                          .preset = Assisi::Physics::LocomotionPreset::Custom};
    const Assisi::Physics::SourceMovement movement{.groundAcceleration = 1000.f,
                                                   .bunnyHop = Assisi::Physics::BunnyHopPolicy::Boost};
    const Assisi::ECS::Entity entity = SpawnCharacter(world, locomotion, movement);

    // Turned half way round: forward is +z.
    world.scene.GetMut<Assisi::ECS::Transform>(entity)->rotation =
        glm::angleAxis(glm::radians(180.f), glm::vec3(0.f, 1.f, 0.f));

    for (int32_t i = 0; i < 60; ++i)
    {
        Ask(world, entity, glm::vec3(0.f), /*jump=*/ false);
        Tick(world);
    }

    // Backwards along -z at 10 m/s, two and a half times the walk speed.
    for (int32_t i = 0; i < 30; ++i)
    {
        Ask(world, entity, {0.f, 0.f, -2.5f}, /*jump=*/ false);
        Tick(world);
    }

    Ask(world, entity, glm::vec3(0.f), /*jump=*/ true);
    Tick(world);

    const Assisi::Physics::CharacterState *character = world.scene.Get<Assisi::Physics::CharacterState>(entity);
    REQUIRE(character != nullptr);
    REQUIRE(character->velocity.y > 1.f);
    CHECK(character->velocity.z < -13.f);
}

TEST_CASE("The step publishes the CharacterState a later system reads")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Characters");

    (void)SpawnFloor(world);

    const Assisi::ECS::Entity entity = SpawnCharacter(world);

    const Assisi::Physics::CharacterState *character = world.scene.Get<Assisi::Physics::CharacterState>(entity);
    REQUIRE(character != nullptr);

    for (int32_t i = 0; i < 60; ++i)
    {
        Ask(world, entity, glm::vec3(0.f), /*jump=*/ false);
        Tick(world);
    }

    CHECK(character->ground == Assisi::Physics::GroundState::OnGround);
    CHECK(character->canJump);
}
