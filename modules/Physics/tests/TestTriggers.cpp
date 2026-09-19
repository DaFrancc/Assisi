/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestTriggers.cpp
/// @brief Bodies on the Trigger channel: detected by what enters them, blocking
/// nothing.
///
/// Two kinds, chosen by the descriptor's `isStatic`, and the difference between
/// them is exactly one case: a body that was already asleep when the volume
/// arrived. A kinematic trigger never sleeps and finds it; a static one is told
/// about it only because creating or moving the volume wakes whatever it
/// encloses. Both cases are pinned here, the second as behaviour rather than as a
/// defect, because it is the reason the two kinds both exist.
///
/// No test switches contact reporting on, because there is no switch. A volume
/// that had to be enabled somewhere else is a volume that silently does nothing,
/// which is the failure this design is shaped to prevent.

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
                  bool isStatic, Physics::CollisionChannel channel,
                  Core::Bitmask<Physics::CollisionChannel> collides = Physics::AllChannels)
{
    const ECS::Entity entity = scene.Create();
    ECS::Transform *transform = scene.Add<ECS::Transform>(entity);
    REQUIRE(transform != nullptr);
    transform->position = at;

    Physics::RigidBodyDescriptor descriptor{};
    descriptor.halfExtents  = halfExtents;
    descriptor.isStatic     = isStatic;
    descriptor.channel      = channel;
    descriptor.collidesWith = collides;
    REQUIRE(scene.Add<Physics::RigidBodyDescriptor>(entity, descriptor) != nullptr);

    (void)world.AddBodyFromDescriptor(scene, entity, *transform, descriptor);
    return entity;
}

/// Counts the phases reported for @p entity over @p steps steps.
struct PhaseCounts
{
    int32_t enters = 0;
    int32_t stays  = 0;
    int32_t exits  = 0;
};

PhaseCounts CountPhases(Physics::PhysicsWorld &world, ECS::Entity entity, int32_t steps)
{
    PhaseCounts counts;
    for (int32_t i = 0; i < steps; ++i)
    {
        world.Update(kStep);
        for (const Physics::ContactEvent &event : world.ContactEvents())
        {
            if (event.entity != entity)
                continue;
            if (event.phase == Physics::ContactPhase::Enter)
                ++counts.enters;
            else if (event.phase == Physics::ContactPhase::Stay)
                ++counts.stays;
            else
                ++counts.exits;
        }
    }
    return counts;
}

float HeightOf(ECS::Scene &scene, const Physics::PhysicsWorld &world, ECS::Entity entity)
{
    const Physics::RigidBody *body = scene.Get<Physics::RigidBody>(entity);
    REQUIRE(body != nullptr);
    return world.GetBodyTransform(*body).first.y;
}

} // namespace

TEST_CASE("A body passes through a trigger and is reported doing it")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world;

    // Floor top at y = 0, and a trigger volume hanging above it.
    (void)Spawn(scene, world, {0.f, -1.f, 0.f}, {20.f, 1.f, 20.f}, /*isStatic=*/ true,
                Physics::CollisionChannel::World);
    (void)Spawn(scene, world, {0.f, 2.f, 0.f}, {1.f, 1.f, 1.f}, /*isStatic=*/ true,
                Physics::CollisionChannel::Trigger);

    const ECS::Entity box = Spawn(scene, world, {0.f, 6.f, 0.f}, {0.5f, 0.5f, 0.5f},
                                  /*isStatic=*/ false, Physics::CollisionChannel::World);

    bool sawSensorEnter = false;
    bool sawSensorExit  = false;
    for (int32_t i = 0; i < kSleepSteps; ++i)
    {
        world.Update(kStep);
        for (const Physics::ContactEvent &event : world.ContactEvents())
        {
            if (event.entity != box || !event.sensor)
                continue;
            if (event.phase == Physics::ContactPhase::Enter)
                sawSensorEnter = true;
            if (event.phase == Physics::ContactPhase::Exit)
                sawSensorExit = true;
        }
    }

    CHECK(sawSensorEnter);
    CHECK(sawSensorExit);

    // And the volume did not hold it up: it is resting on the floor below, not
    // sitting on top of the trigger.
    CHECK(HeightOf(scene, world, box) == doctest::Approx(0.5f).epsilon(0.1));
}

TEST_CASE("A trigger only reports the channels its mask admits")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world;

    const auto charactersOnly = Core::Bitmask<Physics::CollisionChannel>::Of(Physics::CollisionChannel::Character);
    (void)Spawn(scene, world, {0.f, 2.f, 0.f}, {1.f, 1.f, 1.f}, /*isStatic=*/ true,
                Physics::CollisionChannel::Trigger, charactersOnly);

    const ECS::Entity crate = Spawn(scene, world, {0.f, 6.f, 0.f}, {0.5f, 0.5f, 0.5f},
                                    /*isStatic=*/ false, Physics::CollisionChannel::World);

    const PhaseCounts counts = CountPhases(world, crate, 120);
    CHECK(counts.enters == 0);
    CHECK(counts.stays == 0);
    CHECK(counts.exits == 0);
}

TEST_CASE("A kinematic trigger finds a body that was already asleep")
{
    // The case the two trigger kinds differ on. The box settles and Jolt stops
    // testing it; the volume is then created around it. A sensor that had gone to
    // sleep itself would never meet it.
    ECS::Scene scene;
    Physics::PhysicsWorld world;

    (void)Spawn(scene, world, {0.f, -1.f, 0.f}, {20.f, 1.f, 20.f}, /*isStatic=*/ true,
                Physics::CollisionChannel::World);
    const ECS::Entity box = Spawn(scene, world, {0.f, 2.f, 0.f}, {0.5f, 0.5f, 0.5f},
                                  /*isStatic=*/ false, Physics::CollisionChannel::World);

    for (int32_t i = 0; i < kSleepSteps; ++i)
        world.Update(kStep);

    const Physics::RigidBody *body = scene.Get<Physics::RigidBody>(box);
    REQUIRE(body != nullptr);
    REQUIRE_FALSE(world.IsBodyActive(*body));

    // isStatic false is the always-awake kind, and the descriptor's default.
    (void)Spawn(scene, world, {0.f, 0.5f, 0.f}, {2.f, 2.f, 2.f}, /*isStatic=*/ false,
                Physics::CollisionChannel::Trigger);

    const PhaseCounts counts = CountPhases(world, box, 10);
    CHECK(counts.enters == 1);
}

TEST_CASE("A static trigger finds a sleeping body because placing it wakes what it encloses")
{
    // The cheap kind reaches the same answer by a different route: it cannot see a
    // sleeping body, so creating it wakes whatever it now contains and the next
    // step tests the pair normally.
    ECS::Scene scene;
    Physics::PhysicsWorld world;

    (void)Spawn(scene, world, {0.f, -1.f, 0.f}, {20.f, 1.f, 20.f}, /*isStatic=*/ true,
                Physics::CollisionChannel::World);
    const ECS::Entity box = Spawn(scene, world, {0.f, 2.f, 0.f}, {0.5f, 0.5f, 0.5f},
                                  /*isStatic=*/ false, Physics::CollisionChannel::World);

    for (int32_t i = 0; i < kSleepSteps; ++i)
        world.Update(kStep);

    const Physics::RigidBody *body = scene.Get<Physics::RigidBody>(box);
    REQUIRE(body != nullptr);
    REQUIRE_FALSE(world.IsBodyActive(*body));

    (void)Spawn(scene, world, {0.f, 0.5f, 0.f}, {2.f, 2.f, 2.f}, /*isStatic=*/ true,
                Physics::CollisionChannel::Trigger);

    const PhaseCounts counts = CountPhases(world, box, 10);
    CHECK(counts.enters == 1);
}

TEST_CASE("Two overlapping triggers report nothing about each other")
{
    // Neither is solid, so neither entered anything. Reachable rather than
    // theoretical: the default trigger is kinematic, and Jolt does pair a
    // kinematic body with a sensor.
    ECS::Scene scene;
    Physics::PhysicsWorld world;

    const ECS::Entity first = Spawn(scene, world, {0.f, 0.f, 0.f}, {2.f, 2.f, 2.f}, /*isStatic=*/ false,
                                    Physics::CollisionChannel::Trigger);
    (void)Spawn(scene, world, {1.f, 0.f, 0.f}, {2.f, 2.f, 2.f}, /*isStatic=*/ false,
                Physics::CollisionChannel::Trigger);

    const PhaseCounts counts = CountPhases(world, first, 60);
    CHECK(counts.enters == 0);
    CHECK(counts.stays == 0);
}

TEST_CASE("A kinematic trigger stays awake indefinitely")
{
    // What it costs, stated as a test: the volume is in the active set forever,
    // which is the price of noticing bodies that are not moving. A trigger that
    // was allowed to sleep would go silent after a few seconds of quiet.
    ECS::Scene scene;
    Physics::PhysicsWorld world;

    const ECS::Entity volume = Spawn(scene, world, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f},
                                     /*isStatic=*/ false, Physics::CollisionChannel::Trigger);

    for (int32_t i = 0; i < kSleepSteps; ++i)
        world.Update(kStep);

    const Physics::RigidBody *body = scene.Get<Physics::RigidBody>(volume);
    REQUIRE(body != nullptr);
    CHECK(world.IsBodyActive(*body));
}
