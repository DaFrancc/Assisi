/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestNonFinite.cpp
/// @brief A value that is not a number never reaches a body.
///
/// One NaN in a body spreads to everything it touches within a step, and from
/// there into every Transform the writeback writes. So each way gameplay hands
/// physics a number is tried with NaN, and the world has to refuse it and stay
/// as it was.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

#include "PhysicsTestScene.hpp"

using namespace Assisi;
using Assisi::PhysicsTests::TestScene;

namespace
{

const float kNaN = std::numeric_limits<float>::quiet_NaN();
const glm::vec3 kNaNVector{kNaN, kNaN, kNaN};

/// Long enough for a refused value to have spread, had it not been refused.
constexpr int32_t kSteps = 30;

bool Finite(glm::vec3 value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

/// A dynamic box resting on a floor.
ECS::Entity RestingBox(TestScene &test)
{
    (void)PhysicsTests::AddFloor(test.scene);
    const ECS::Entity box =
        PhysicsTests::AddBody(test.scene, {0.f, 0.5f, 0.f}, PhysicsTests::Box({0.5f, 0.5f, 0.5f}, false));
    PhysicsTests::Step(test.world, kSteps);
    return box;
}

} // namespace

TEST_CASE("A Transform that is not a number is put back, and the body stays where it was")
{
    TestScene test;
    const ECS::Entity box = RestingBox(test);
    const glm::vec3 before = test.world.GetBodyPose(box).position;

    test.scene.GetMut<ECS::Transform>(box)->position = kNaNVector;
    PhysicsTests::Step(test.world, kSteps);

    CHECK(Finite(test.world.GetBodyPose(box).position));
    CHECK(test.world.GetBodyPose(box).position.y == doctest::Approx(before.y).epsilon(0.05));
    CHECK(Finite(test.scene.Get<ECS::Transform>(box)->position));
}

TEST_CASE("A velocity that is not a number is put back, and the body keeps the one it had")
{
    TestScene test;
    const ECS::Entity box = RestingBox(test);

    test.scene.GetMut<Physics::BodyState>(box)->linearVelocity = kNaNVector;
    PhysicsTests::Step(test.world, kSteps);

    CHECK(Finite(test.world.GetBodyPose(box).position));
    CHECK(Finite(test.world.GetBodyState(box).linearVelocity));
    CHECK(Finite(test.scene.Get<Physics::BodyState>(box)->linearVelocity));
}

TEST_CASE("A push, a teleport or a correction that is not a number is ignored")
{
    TestScene test;
    const ECS::Entity box = RestingBox(test);

    test.world.AddImpulse(box, kNaNVector);
    test.world.AddForceAt(box, {0.f, 1.f, 0.f}, kNaNVector);
    test.world.Teleport(box, Physics::Pose{glm::quat(1.f, 0.f, 0.f, 0.f), kNaNVector});
    test.world.ApplyCorrection(box, Physics::Pose{}, Physics::BodyState{.linearVelocity = kNaNVector});
    PhysicsTests::Step(test.world, kSteps);

    CHECK(Finite(test.world.GetBodyPose(box).position));
    CHECK(Finite(test.world.GetBodyState(box).linearVelocity));
    CHECK(Finite(test.scene.Get<ECS::Transform>(box)->position));
}

TEST_CASE("A character asked to move by something that is not a number stands still")
{
    TestScene test;
    (void)PhysicsTests::AddFloor(test.scene);
    const ECS::Entity character = PhysicsTests::AddCharacter(test.scene, {0.f, 0.f, 0.f});
    PhysicsTests::Step(test.world, kSteps);

    test.scene.GetMut<Physics::CharacterIntent>(character)->move = kNaNVector;
    PhysicsTests::Step(test.world, kSteps);

    CHECK(Finite(test.scene.Get<ECS::Transform>(character)->position));
    CHECK(Finite(PhysicsTests::StateOf(test.scene, character).velocity));
}
