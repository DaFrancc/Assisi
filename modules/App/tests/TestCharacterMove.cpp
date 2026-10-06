/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCharacterMove.cpp
/// @brief A character driven through its CharacterIntent, in a world ticked the
///        way the hosts tick it.
///
/// The controller itself is covered in the Physics suite. What is checked here is
/// the translation: that the Character's walk speed is applied, that the crouch
/// scale is applied to the stance actually reached rather than the one asked for,
/// that a jump request is consumed exactly once, that the step publishes its
/// CharacterState, and that the camera is kept at the eye.

#include <doctest/doctest.h>

#include <cstdint>

#include <Assisi/App/PhysicsSystems.hpp>
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

/// One whole fixed tick, in the order the hosts run it: the step, then the
/// post-step systems, which read the CharacterState the step left.
void Tick(World &world, Assisi::Core::EventQueue &events)
{
    SystemContext ctx{world, kStep, /*simTick=*/ 0, nullptr, nullptr, events, true, nullptr};
    const Assisi::ECS::FixedStepScope step(world.scene);

    world.physics.Update(kStep);

    CharacterEyeSystem(ctx);
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

Assisi::ECS::Entity SpawnCharacter(World &world, const Assisi::Physics::Character &descriptor)
{
    const Assisi::ECS::Entity entity = world.scene.Create();
    Assisi::ECS::Transform *transform = world.scene.Add<Assisi::ECS::Transform>(entity);
    transform->position = {0.f, 0.f, 0.f};
    (void)world.scene.Add<Assisi::Physics::Character>(entity, descriptor);

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

TEST_CASE("CharacterIntent's move direction is scaled by the Character's walk speed")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Characters");
    Assisi::Core::EventQueue events;

    (void)SpawnFloor(world);

    Assisi::Physics::Character descriptor{};
    descriptor.walkSpeed = 6.f;
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

    const Assisi::Physics::CharacterState *character = world.scene.Get<Assisi::Physics::CharacterState>(entity);
    REQUIRE(character != nullptr);

    // A direction of magnitude 1 times walkSpeed — not the raw direction, which
    // would crawl at 1 m/s whatever the descriptor said.
    CHECK(character->velocity.x == doctest::Approx(6.f).epsilon(0.1));
}

TEST_CASE("Crouching applies the Character's speed scale")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Characters");
    Assisi::Core::EventQueue events;

    (void)SpawnFloor(world);

    Assisi::Physics::Character descriptor{};
    descriptor.walkSpeed = 6.f;
    descriptor.crouchSpeedScale = 0.5f;
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
    Assisi::Core::EventQueue events;

    (void)SpawnFloor(world);

    Assisi::Physics::Character descriptor{};
    const Assisi::ECS::Entity entity = SpawnCharacter(world, descriptor);

    for (int32_t i = 0; i < 60; ++i)
    {
        Ask(world, entity, glm::vec3(0.f), /*jump=*/ false);
        Tick(world, events);
    }

    Ask(world, entity, glm::vec3(0.f), /*jump=*/ true);
    Tick(world, events);

    CHECK_FALSE(world.scene.Get<Assisi::Physics::CharacterIntent>(entity)->jump);
    CHECK(world.scene.Get<Assisi::Physics::CharacterState>(entity)->velocity.y > 1.f);
}

TEST_CASE("A camera parented to a character is placed at its eye height, blended between steps")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Characters");
    Assisi::Core::EventQueue events;

    (void)SpawnFloor(world);

    const Assisi::Physics::Character descriptor{};
    const Assisi::ECS::Entity entity = SpawnCharacter(world, descriptor);

    // Authored somewhere else on purpose: the height is the character's to set.
    const Assisi::ECS::Entity eye = world.scene.Create();
    Assisi::ECS::Transform *eyeTransform = world.scene.Add<Assisi::ECS::Transform>(eye);
    eyeTransform->position = {0.f, 5.f, 0.f};
    (void)world.scene.Add<Assisi::Runtime::Camera>(eye);
    (void)world.scene.Add<Assisi::ECS::Parent>(eye, Assisi::ECS::Parent{entity});

    for (int32_t i = 0; i < 60; ++i)
    {
        Ask(world, entity, glm::vec3(0.f), /*jump=*/ false);
        Tick(world, events);
    }
    CHECK(world.scene.Get<Assisi::ECS::Transform>(eye)->position.y == doctest::Approx(descriptor.eyeHeight));
    uint64_t tick = Assisi::ECS::PropagateTransforms(world.scene, 0);

    // One step into a crouch, a frame drawn half way between the two steps
    // sees the eye half way between them: it moves every frame, not every step.
    Ask(world, entity, glm::vec3(0.f), /*jump=*/ false, Assisi::Physics::Stance::Crouching);
    Tick(world, events);
    const float afterOneStep = world.scene.Get<Assisi::Physics::CharacterState>(entity)->eyeHeight;
    REQUIRE(afterOneStep < descriptor.eyeHeight);
    CHECK(world.scene.Get<Assisi::ECS::Transform>(eye)->position.y == doctest::Approx(afterOneStep));

    Assisi::ECS::SetBlendAlpha(world.scene, 0.5f);
    tick = Assisi::ECS::PropagateTransforms(world.scene, tick);
    const float feet = world.scene.Get<Assisi::ECS::Transform>(entity)->position.y;
    const float drawnEye = Assisi::Math::TranslationOf(world.scene.Get<Assisi::ECS::WorldMatrix>(eye)->matrix).y;
    CHECK(drawnEye - feet == doctest::Approx(0.5f * (descriptor.eyeHeight + afterOneStep)));

    for (int32_t i = 0; i < 60; ++i)
    {
        Ask(world, entity, glm::vec3(0.f), /*jump=*/ false, Assisi::Physics::Stance::Crouching);
        Tick(world, events);
    }
    CHECK(world.scene.Get<Assisi::ECS::Transform>(eye)->position.y == doctest::Approx(descriptor.crouchEyeHeight));
}

TEST_CASE("The step takes a character's facing from its Transform")
{
    // Only the Boost policy reads the facing, so that is what shows it arrived:
    // a character over the limit and travelling backwards gains speed on a jump.
    // With the facing left at its default it would be slowed instead.
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Characters");
    Assisi::Core::EventQueue events;

    (void)SpawnFloor(world);

    Assisi::Physics::Character descriptor{};
    descriptor.walkSpeed = 4.f;
    descriptor.groundAcceleration = 1000.f;
    descriptor.bunnyHop = Assisi::Physics::BunnyHopPolicy::Boost;
    descriptor.coyoteTime = 0.f;
    descriptor.jumpBufferTime = 0.f;
    const Assisi::ECS::Entity entity = SpawnCharacter(world, descriptor);

    // Turned half way round: forward is +z.
    world.scene.GetMut<Assisi::ECS::Transform>(entity)->rotation =
        glm::angleAxis(glm::radians(180.f), glm::vec3(0.f, 1.f, 0.f));

    for (int32_t i = 0; i < 60; ++i)
    {
        Ask(world, entity, glm::vec3(0.f), /*jump=*/ false);
        Tick(world, events);
    }

    // Backwards along -z at 10 m/s, two and a half times the walk speed.
    for (int32_t i = 0; i < 30; ++i)
    {
        Ask(world, entity, {0.f, 0.f, -2.5f}, /*jump=*/ false);
        Tick(world, events);
    }

    Ask(world, entity, glm::vec3(0.f), /*jump=*/ true);
    Tick(world, events);

    const Assisi::Physics::CharacterState *character = world.scene.Get<Assisi::Physics::CharacterState>(entity);
    REQUIRE(character != nullptr);
    REQUIRE(character->velocity.y > 1.f);
    CHECK(character->velocity.z < -13.f);
}

TEST_CASE("The step publishes the CharacterState a later system reads")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Characters");
    Assisi::Core::EventQueue events;

    (void)SpawnFloor(world);

    Assisi::Physics::Character descriptor{};
    const Assisi::ECS::Entity entity = SpawnCharacter(world, descriptor);

    const Assisi::Physics::CharacterState *character = world.scene.Get<Assisi::Physics::CharacterState>(entity);
    REQUIRE(character != nullptr);

    for (int32_t i = 0; i < 60; ++i)
    {
        Ask(world, entity, glm::vec3(0.f), /*jump=*/ false);
        Tick(world, events);
    }

    CHECK(character->ground == Assisi::Physics::GroundState::OnGround);
    CHECK(character->canJump);
}
