/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBodyState.cpp
/// @brief BodyState both ways: the writeback reports what a body did, and a
/// velocity gameplay writes reaches the body before the next step.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

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

/// Long enough for a body dropped onto the floor to come to rest and fall
/// asleep.
constexpr int32_t kSettleSteps = 600;

const Physics::BodyState &StateOf(const TestScene &test, ECS::Entity entity)
{
    const Physics::BodyState *state = test.scene.Get<Physics::BodyState>(entity);
    REQUIRE(state != nullptr);
    return *state;
}

} // namespace

TEST_CASE("BodyState: a RigidBody brings one")
{
    TestScene test;
    const ECS::Entity ball = AddBody(test.scene, {0.f, 10.f, 0.f}, Ball(0.5f, false));
    CHECK(test.scene.Has<Physics::BodyState>(ball));
}

TEST_CASE("BodyState: a falling body reports its velocity after each step")
{
    TestScene test;
    const ECS::Entity ball = AddBody(test.scene, {0.f, 10.f, 0.f}, Ball(0.5f, false));
    Step(test.world, 30);

    CHECK(StateOf(test, ball).linearVelocity == test.world.GetBodyVelocity(ball).first);
    CHECK(StateOf(test, ball).linearVelocity.y < -1.f);
    CHECK_FALSE(StateOf(test, ball).asleep);
}

TEST_CASE("BodyState: a written velocity is the one the next step simulates")
{
    TestScene test;
    const ECS::Entity ball = AddBody(test.scene, {0.f, 10.f, 0.f}, Ball(0.5f, false));
    Step(test.world);

    test.scene.GetMut<Physics::BodyState>(ball)->linearVelocity = {4.f, 0.f, 0.f};
    Step(test.world);

    CHECK(test.world.GetBodyVelocity(ball).first.x == doctest::Approx(4.f).epsilon(0.02));
    CHECK(test.world.GetBodyPose(ball).position.x > 0.f);
}

TEST_CASE("BodyState: a written velocity wakes a body asleep on the floor")
{
    TestScene test;
    AddFloor(test.scene);
    const ECS::Entity ball = AddBody(test.scene, {0.f, 1.f, 0.f}, Ball(0.5f, false));
    Step(test.world, kSettleSteps);
    REQUIRE(StateOf(test, ball).asleep);
    REQUIRE_FALSE(test.world.IsBodyActive(ball));

    test.scene.GetMut<Physics::BodyState>(ball)->linearVelocity = {0.f, 5.f, 0.f};
    Step(test.world);

    CHECK(test.world.IsBodyActive(ball));
    CHECK(test.world.GetBodyPose(ball).position.y > 0.55f);
    CHECK_FALSE(StateOf(test, ball).asleep);
}

TEST_CASE("BodyState: a body at rest is written once, and its own writes are never pushed back")
{
    TestScene test;
    AddFloor(test.scene);
    const ECS::Entity ball = AddBody(test.scene, {0.f, 1.f, 0.f}, Ball(0.5f, false));
    Step(test.world, kSettleSteps);
    REQUIRE(StateOf(test, ball).asleep);

    const uint64_t since = test.scene.CurrentChangeTick();
    Step(test.world, 10);

    CHECK_FALSE(test.scene.Changed<Physics::BodyState>(ball, since));
    CHECK_FALSE(test.world.IsBodyActive(ball));
}

TEST_CASE("BodyState: a velocity written before the body exists is the one it starts with")
{
    TestScene test;
    BodySpec spec = Ball(0.5f, false);
    spec.rigidBody->gravityScale = 0.f;
    const ECS::Entity shot = AddBody(test.scene, {0.f, 0.f, 0.f}, spec);
    test.scene.GetMut<Physics::BodyState>(shot)->linearVelocity = {10.f, 0.f, 0.f};
    Step(test.world, 6);

    CHECK(test.world.GetBodyPose(shot).position.x == doctest::Approx(1.f).epsilon(0.05));
}
