/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCharacterMove.cpp
/// @brief App::CharacterMoveSystem — the one thing that has to run for a
///        character to move, and the only place intent becomes motion.
///
/// The controller itself is covered in the Physics suite. What is checked here is
/// the translation: that the descriptor's speed is applied, that the crouch scale
/// is applied to the stance actually reached rather than the one asked for, and
/// that a jump request is consumed exactly once.

#include <doctest/doctest.h>

#include <cstdint>

#include <Assisi/App/PhysicsSystems.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>

using namespace Assisi::App;

namespace
{

constexpr float kStep = 1.f / 60.f;

/// One whole fixed tick, in the order the hosts run it: the pre-step systems,
/// the step, then the post-step systems. Both halves matter here — the intent is
/// consumed before the step and the state is published after it, which is the
/// split PostFixedUpdate exists for.
void Tick(World &world, Assisi::Core::EventQueue &events)
{
    SystemContext ctx{world, kStep, /*simTick=*/ 0, nullptr, nullptr, events, true, nullptr};

    CharacterMoveSystem(ctx);

    world.physics.Update(kStep);
    world.physics.CaptureState();

    CharacterStateSystem(ctx);
}

Assisi::ECS::Entity SpawnFloor(World &world)
{
    const Assisi::ECS::Entity entity = world.scene.Create();
    Assisi::ECS::Transform   *transform = world.scene.Add<Assisi::ECS::Transform>(entity);
    transform->position                 = {0.f, -0.5f, 0.f};

    Assisi::Physics::RigidBodyDescriptor descriptor{};
    descriptor.halfExtents = {20.f, 0.5f, 20.f};
    descriptor.isStatic    = true;
    (void)world.scene.Add<Assisi::Physics::RigidBodyDescriptor>(entity, descriptor);

    (void)world.physics.AddBodyFromDescriptor(world.scene, entity, *transform, descriptor);
    return entity;
}

Assisi::ECS::Entity SpawnCharacter(World &world, const Assisi::Physics::CharacterDescriptor &descriptor)
{
    const Assisi::ECS::Entity entity = world.scene.Create();
    Assisi::ECS::Transform   *transform = world.scene.Add<Assisi::ECS::Transform>(entity);
    transform->position                 = {0.f, 0.f, 0.f};
    (void)world.scene.Add<Assisi::Physics::CharacterDescriptor>(entity, descriptor);

    const auto added =
        world.physics.AddCharacterFromDescriptor(world.scene, entity, *transform, descriptor);
    REQUIRE(added.has_value());
    return entity;
}

/// Sets what the character is being asked to do this tick.
void Ask(World &world, Assisi::ECS::Entity entity, glm::vec3 move, bool jump,
         Assisi::Physics::Stance stance = Assisi::Physics::Stance::Standing)
{
    Assisi::Physics::Character *character =
        world.scene.GetMut<Assisi::Physics::Character>(entity);
    REQUIRE(character != nullptr);
    character->move   = move;
    character->jump   = jump;
    character->stance = stance;
}

} // namespace

TEST_CASE("CharacterMoveSystem scales the move direction by the descriptor's walk speed")
{
    WorldManager worlds;
    World &world = worlds.Create("Characters");
    Assisi::Core::EventQueue events;

    (void)SpawnFloor(world);

    Assisi::Physics::CharacterDescriptor descriptor{};
    descriptor.walkSpeed          = 6.f;
    descriptor.groundAcceleration = 1000.f; // at speed within one step
    const Assisi::ECS::Entity entity = SpawnCharacter(world, descriptor);

    // Let it settle onto the floor first: accelerating is a ground behaviour.
    for (int32_t i = 0; i < 60; ++i)
    {
        Ask(world, entity, glm::vec3(0.f), /*jump=*/ false);
        Tick(world, events);
    }

    Ask(world, entity, {1.f, 0.f, 0.f}, /*jump=*/ false);
    Tick(world, events);

    const Assisi::Physics::Character *character =
        world.scene.Get<Assisi::Physics::Character>(entity);
    REQUIRE(character != nullptr);

    // A direction of magnitude 1 times walkSpeed — not the raw direction, which
    // would crawl at 1 m/s whatever the descriptor said.
    CHECK(character->state.velocity.x == doctest::Approx(6.f).epsilon(0.1));
}

TEST_CASE("Crouching applies the descriptor's speed scale")
{
    WorldManager worlds;
    World &world = worlds.Create("Characters");
    Assisi::Core::EventQueue events;

    (void)SpawnFloor(world);

    Assisi::Physics::CharacterDescriptor descriptor{};
    descriptor.walkSpeed          = 6.f;
    descriptor.crouchSpeedScale   = 0.5f;
    descriptor.groundAcceleration = 1000.f;
    const Assisi::ECS::Entity entity = SpawnCharacter(world, descriptor);

    for (int32_t i = 0; i < 60; ++i)
    {
        Ask(world, entity, glm::vec3(0.f), /*jump=*/ false);
        Tick(world, events);
    }

    // Nothing overhead, so the crouch is granted and the scale applies.
    Ask(world, entity, {1.f, 0.f, 0.f}, /*jump=*/ false, Assisi::Physics::Stance::Crouching);
    Tick(world, events);

    const Assisi::Physics::Character *character =
        world.scene.Get<Assisi::Physics::Character>(entity);
    REQUIRE(character != nullptr);
    CHECK(character->state.stance == Assisi::Physics::Stance::Crouching);
    CHECK(character->state.velocity.x == doctest::Approx(3.f).epsilon(0.1));
}

TEST_CASE("A jump request is consumed once, not held")
{
    // Otherwise a single press jumps again on every step that follows it, which
    // reads as the character sticking to the ceiling.
    WorldManager worlds;
    World &world = worlds.Create("Characters");
    Assisi::Core::EventQueue events;

    (void)SpawnFloor(world);

    Assisi::Physics::CharacterDescriptor descriptor{};
    const Assisi::ECS::Entity entity = SpawnCharacter(world, descriptor);

    for (int32_t i = 0; i < 60; ++i)
    {
        Ask(world, entity, glm::vec3(0.f), /*jump=*/ false);
        Tick(world, events);
    }

    Ask(world, entity, glm::vec3(0.f), /*jump=*/ true);
    Tick(world, events);

    const Assisi::Physics::Character *character =
        world.scene.Get<Assisi::Physics::Character>(entity);
    REQUIRE(character != nullptr);
    CHECK_FALSE(character->jump);
    CHECK(character->state.velocity.y > 1.f);
}

TEST_CASE("CharacterMoveSystem refreshes the state a later system would read")
{
    WorldManager worlds;
    World &world = worlds.Create("Characters");
    Assisi::Core::EventQueue events;

    (void)SpawnFloor(world);

    Assisi::Physics::CharacterDescriptor descriptor{};
    const Assisi::ECS::Entity entity = SpawnCharacter(world, descriptor);

    // Straight after creation the component's state is still its default, which
    // says InAir whatever the character is really doing.
    const Assisi::Physics::Character *character =
        world.scene.Get<Assisi::Physics::Character>(entity);
    REQUIRE(character != nullptr);
    REQUIRE(character->state.ground == Assisi::Physics::GroundState::InAir);

    for (int32_t i = 0; i < 60; ++i)
    {
        Ask(world, entity, glm::vec3(0.f), /*jump=*/ false);
        Tick(world, events);
    }

    CHECK(character->state.ground == Assisi::Physics::GroundState::OnGround);
    CHECK(character->state.canJump);
}
