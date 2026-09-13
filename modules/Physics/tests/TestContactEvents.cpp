/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestContactEvents.cpp
/// @brief Enter, Stay and Exit, and the cases Jolt's own callbacks get wrong.
///
/// Jolt reports a contact as *removed* when a body falls asleep, and forbids
/// reading either body at that moment because one may already be destroyed. So
/// the phases here are not Jolt's callbacks renamed: they come from comparing
/// this step's touching pairs against last step's, after the step, on one thread.
/// The cases that matter are the ones where that difference shows — a resting
/// body, a waking body, and a body destroyed mid-contact.

#include <doctest/doctest.h>

#include <cstdint>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

using namespace Assisi;

namespace
{

constexpr float kStep = 1.f / 60.f;

/// Long enough for a dropped body to land and for Jolt to put it to sleep.
constexpr int32_t kSleepSteps = 300;

ECS::Entity Spawn(ECS::Scene &scene, Physics::PhysicsWorld &world, glm::vec3 at, glm::vec3 halfExtents,
                  bool isStatic)
{
    const ECS::Entity entity = scene.Create();
    ECS::Transform *transform = scene.Add<ECS::Transform>(entity);
    REQUIRE(transform != nullptr);
    transform->position = at;

    Physics::RigidBodyDescriptor descriptor{};
    descriptor.halfExtents = halfExtents;
    descriptor.isStatic    = isStatic;
    REQUIRE(scene.Add<Physics::RigidBodyDescriptor>(entity, descriptor) != nullptr);

    (void)world.AddBodyFromDescriptor(scene, entity, *transform, descriptor);
    return entity;
}

/// Floor with its top at y = 0, plus a box dropped from @p dropFrom.
ECS::Entity BuildDrop(ECS::Scene &scene, Physics::PhysicsWorld &world, float dropFrom)
{
    (void)Spawn(scene, world, {0.f, -1.f, 0.f}, {20.f, 1.f, 20.f}, /*isStatic=*/ true);
    return Spawn(scene, world, {0.f, dropFrom, 0.f}, {0.5f, 0.5f, 0.5f}, /*isStatic=*/ false);
}

int32_t CountPhase(const Physics::PhysicsWorld &world, ECS::Entity entity, Physics::ContactPhase phase)
{
    int32_t seen = 0;
    for (const Physics::ContactEvent &event : world.ContactEvents())
    {
        if (event.entity == entity && event.phase == phase)
            ++seen;
    }
    return seen;
}

/// Steps until @p entity reports an Enter, returning the step it happened on, or
/// -1 if it never did.
int32_t StepUntilEnter(Physics::PhysicsWorld &world, ECS::Entity entity, int32_t maxSteps = 240)
{
    for (int32_t i = 0; i < maxSteps; ++i)
    {
        world.Update(kStep);
        if (CountPhase(world, entity, Physics::ContactPhase::Enter) > 0)
            return i;
    }
    return -1;
}

} // namespace

TEST_CASE("A pair reports Enter exactly once, however many contact points it has")
{
    // A box landing flat on a floor touches at four corners, and Jolt runs several
    // collision substeps per Update. All of that has to collapse into one event,
    // or a consumer counting entries counts corners.
    ECS::Scene scene;
    Physics::PhysicsWorld world;
    const ECS::Entity box = BuildDrop(scene, world, 3.f);

    REQUIRE(StepUntilEnter(world, box) >= 0);
    CHECK(CountPhase(world, box, Physics::ContactPhase::Enter) == 1);
}

TEST_CASE("A resting pair keeps reporting Stay after the body falls asleep")
{
    // The case Jolt's callbacks cannot express. Once the box sleeps, Jolt stops
    // reporting the pair entirely and fires its "contact removed" callback — so
    // anything built on that callback would announce the box had left the floor it
    // is still sitting on.
    ECS::Scene scene;
    Physics::PhysicsWorld world;
    const ECS::Entity box = BuildDrop(scene, world, 3.f);

    REQUIRE(StepUntilEnter(world, box) >= 0);

    int32_t stays = 0;
    int32_t exits = 0;
    for (int32_t i = 0; i < kSleepSteps; ++i)
    {
        world.Update(kStep);
        stays += CountPhase(world, box, Physics::ContactPhase::Stay);
        exits += CountPhase(world, box, Physics::ContactPhase::Exit);
    }

    const Physics::RigidBody *body = scene.Get<Physics::RigidBody>(box);
    REQUIRE(body != nullptr);
    REQUIRE_FALSE(world.IsBodyActive(*body)); // it really did go to sleep

    CHECK(stays == kSleepSteps);
    CHECK(exits == 0);
}

TEST_CASE("A body woken while still touching does not report a second Enter")
{
    // The other half of dormancy. Jolt re-adds the contact when the body wakes,
    // which reads as brand new; the pair table remembers it never ended.
    ECS::Scene scene;
    Physics::PhysicsWorld world;
    const ECS::Entity box = BuildDrop(scene, world, 3.f);

    REQUIRE(StepUntilEnter(world, box) >= 0);
    for (int32_t i = 0; i < kSleepSteps; ++i)
        world.Update(kStep);

    const Physics::RigidBody *body = scene.Get<Physics::RigidBody>(box);
    REQUIRE(body != nullptr);
    REQUIRE_FALSE(world.IsBodyActive(*body));

    // A nudge along the floor: it wakes and keeps touching.
    world.SetBodyLinearVelocity(*body, {0.2f, 0.f, 0.f});
    REQUIRE(world.IsBodyActive(*body));

    int32_t enters = 0;
    for (int32_t i = 0; i < 30; ++i)
    {
        world.Update(kStep);
        enters += CountPhase(world, box, Physics::ContactPhase::Enter);
    }
    CHECK(enters == 0);
}

TEST_CASE("Destroying a body reports one Exit for what it was touching")
{
    // The entity behind a body stops being knowable the moment the body goes, so
    // the event has to be built during removal and delivered on the next step. A
    // consumer holding "who is inside me" would otherwise keep a destroyed entity
    // in it forever.
    ECS::Scene scene;
    Physics::PhysicsWorld world;

    const ECS::Entity floor = Spawn(scene, world, {0.f, -1.f, 0.f}, {20.f, 1.f, 20.f}, /*isStatic=*/ true);
    const ECS::Entity box   = Spawn(scene, world, {0.f, 3.f, 0.f}, {0.5f, 0.5f, 0.5f}, /*isStatic=*/ false);

    REQUIRE(StepUntilEnter(world, box) >= 0);

    const Physics::RigidBody *body = scene.Get<Physics::RigidBody>(box);
    REQUIRE(body != nullptr);
    world.RemoveBody(*body);

    world.Update(kStep);

    // Reported from the floor's point of view, which is the side still around to
    // act on it.
    int32_t exits = 0;
    for (const Physics::ContactEvent &event : world.ContactEvents())
    {
        if (event.entity == floor && event.other == box && event.phase == Physics::ContactPhase::Exit)
            ++exits;
    }
    CHECK(exits == 1);

    // And nothing lingers: the pair is gone, not merely reported gone.
    world.Update(kStep);
    CHECK(CountPhase(world, floor, Physics::ContactPhase::Exit) == 0);
    CHECK(CountPhase(world, floor, Physics::ContactPhase::Stay) == 0);
}

TEST_CASE("Events describe one step and are dropped at the next")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world;
    const ECS::Entity box = BuildDrop(scene, world, 3.f);

    REQUIRE(StepUntilEnter(world, box) >= 0);
    const std::size_t landed = world.ContactEvents().size();
    REQUIRE(landed > 0u);

    // The next step replaces them rather than appending: a consumer running once
    // per fixed step must never see the same event twice.
    world.Update(kStep);
    CHECK(CountPhase(world, box, Physics::ContactPhase::Enter) == 0);
}

TEST_CASE("Clear drops every pair without inventing departures")
{
    // A world being emptied is not a world where things left each other, and there
    // is nothing left that could act on the event anyway — the entities are about
    // to mean something else entirely.
    ECS::Scene scene;
    Physics::PhysicsWorld world;
    const ECS::Entity box = BuildDrop(scene, world, 3.f);

    REQUIRE(StepUntilEnter(world, box) >= 0);

    world.Clear();
    CHECK(world.ContactEvents().empty());

    world.Update(kStep);
    CHECK(world.ContactEvents().empty());
}
