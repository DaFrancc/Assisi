/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestFarFromOrigin.cpp
/// @brief A world 100 km from the origin simulates as the same world at the
/// origin does.
///
/// Each case runs one setup twice, once at the origin and once 100 km out, and
/// compares where things end up measured from where the setup was placed. With
/// float positions the far run would round every position to about 8 mm, so the
/// motion along the far axes would come out centimetres off.

#include <doctest/doctest.h>

#include "PhysicsTestScene.hpp"

#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

#include <cstdint>

using namespace Assisi;
using namespace Assisi::PhysicsTests;

namespace
{

/// Where the far run is placed: 100 km out along both horizontal axes, where a
/// float can hold a position only to the nearest 7.8 mm.
constexpr glm::dvec3 kFar{100000.0, 0.0, 100000.0};

/// How closely the far run must match the near one. Far below the float
/// spacing out there, and well above what two runs of the same steps round to.
constexpr double kSameTolerance = 1e-4;

/// A push that rolls a resting ball of kBallMass a few metres across the floor.
constexpr glm::vec3 kPush{6.f, 0.f, -4.f};

/// Light, so kPush moves it metres rather than millimetres.
constexpr float kBallMass = 1.f;

/// Long enough for the pushed ball to roll to a near stop.
constexpr int32_t kRollSteps = 120;

/// A brisk walk along both horizontal axes at once.
constexpr glm::vec3 kWalk{2.f, 0.f, -3.f};

/// One second of walking.
constexpr int32_t kWalkSteps = 60;

/// Steps to let a dropped ball settle on the floor before it is pushed.
constexpr int32_t kSettleSteps = 60;

/// Half the floor's width: room for every case to stay on it.
constexpr float kFloorHalfWidth = 50.f;
constexpr float kFloorHalfThickness = 0.5f;

void AddFloorAt(ECS::Scene &scene, const glm::dvec3 &origin)
{
    (void)AddBody(scene, origin + glm::dvec3(0.0, -kFloorHalfThickness, 0.0),
                  Box({kFloorHalfWidth, kFloorHalfThickness, kFloorHalfWidth}, true));
}

/// Where a ball resting at @p origin ends up after a push, measured from @p origin.
glm::dvec3 PushedBall(const glm::dvec3 &origin)
{
    TestScene test;
    AddFloorAt(test.scene, origin);
    BodySpec spec = Ball(0.5f, false);
    spec.rigidBody->mass = kBallMass;
    const ECS::Entity ball = AddBody(test.scene, origin + glm::dvec3(0.0, 0.5, 0.0), spec);
    Step(test.world, kSettleSteps);
    test.world.AddImpulse(ball, kPush);
    Step(test.world, kRollSteps);
    return test.scene.Get<ECS::Transform>(ball)->position - origin;
}

/// Where a character standing at @p origin is after walking, measured from @p origin.
glm::dvec3 WalkedCharacter(const glm::dvec3 &origin)
{
    TestScene test;
    AddFloorAt(test.scene, origin);
    const ECS::Entity character = AddCharacter(test.scene, origin);
    for (int32_t i = 0; i < kWalkSteps; ++i)
    {
        Drive(test.scene, character, kWalk, /*jump=*/ false);
        test.world.Update(kStep);
    }
    return test.scene.Get<ECS::Transform>(character)->position - origin;
}

} // namespace

TEST_CASE("A body pushed 100 km from the origin rolls as far as one at the origin")
{
    const glm::dvec3 near = PushedBall(glm::dvec3(0.0));
    const glm::dvec3 far = PushedBall(kFar);

    // It did roll, along both of the far axes.
    REQUIRE(near.x > 1.0);
    REQUIRE(near.z < -1.0);
    CHECK(glm::length(far - near) < kSameTolerance);
}

TEST_CASE("A character walking 100 km from the origin covers the same ground as one at the origin")
{
    const glm::dvec3 near = WalkedCharacter(glm::dvec3(0.0));
    const glm::dvec3 far = WalkedCharacter(kFar);

    REQUIRE(near.x > 1.0);
    REQUIRE(near.z < -1.0);
    CHECK(glm::length(far - near) < kSameTolerance);
}

TEST_CASE("A body under a parent 100 km from the origin is placed exactly where its offset puts it")
{
    TestScene test;
    // A parent a fraction of a float step off a whole number of metres: a float
    // would round it away.
    const glm::dvec3 parentAt = kFar + glm::dvec3(0.0012, 2.0, 0.0034);
    const ECS::Entity parent = test.scene.Create();
    REQUIRE(test.scene.Add(parent, ECS::Transform{.position = parentAt}) != nullptr);

    const glm::dvec3 offset{0.5, 0.0, 0.0};
    const ECS::Entity child = test.scene.Create();
    REQUIRE(test.scene.Add(child, ECS::Transform{.position = offset}) != nullptr);
    REQUIRE(test.scene.Add(child, ECS::Parent{.parent = parent}) != nullptr);
    REQUIRE(test.scene.Add(child, Ball(0.5f, true).collider) != nullptr);
    test.world.Reconcile();

    CHECK(glm::length(test.world.GetBodyPose(child).position - (parentAt + offset)) < kSameTolerance);
}
