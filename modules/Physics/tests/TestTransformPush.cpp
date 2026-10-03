/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestTransformPush.cpp
/// @brief Writing an entity's Transform moves its body, and editing its
///        descriptor changes it, without anything telling the world to.
///
/// Each motion type takes a write its own way: a static body is placed, a
/// kinematic one is swept so it pushes what it meets, a dynamic one is placed
/// and keeps going, and Teleport is the one that stops it. A descriptor edit is
/// applied to the live body rather than waiting for a reload.

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>

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

/// Whether a ray straight down onto @p point finds anything.
bool SomethingAt(const Physics::PhysicsWorld &world, glm::vec3 point)
{
    constexpr float kAbove = 5.f;
    constexpr float kReach = 10.f;
    const std::optional<Physics::QueryHit> hit =
        world.CastRay(point + glm::vec3(0.f, kAbove, 0.f), {0.f, -kReach, 0.f}, Physics::CollisionFilter{},
                      ECS::NullEntity);
    return hit.has_value();
}

/// A kinematic body: today only a moving sensor is one.
Physics::RigidBodyDescriptor KinematicSensor()
{
    Physics::RigidBodyDescriptor descriptor = Box({0.5f, 0.5f, 0.5f}, false);
    descriptor.channel = Physics::CollisionChannel::Trigger;
    return descriptor;
}

/// Steps long enough for a body dropped onto the floor to fall asleep.
constexpr int32_t kSettleSteps = 600;

} // namespace

TEST_CASE("Writing a static body's Transform moves its collider")
{
    TestScene test;
    const glm::vec3 from{0.f, 0.f, 0.f};
    const glm::vec3 to{10.f, 0.f, 0.f};
    const ECS::Entity wall = AddBody(test.scene, from, Box({0.5f, 0.5f, 0.5f}, true));
    test.world.Reconcile();

    test.scene.GetMut<ECS::Transform>(wall)->position = to;
    test.world.Reconcile();

    CHECK(SomethingAt(test.world, to));
    CHECK_FALSE(SomethingAt(test.world, from));
}

TEST_CASE("Writing a kinematic body's Transform sweeps it there over the step")
{
    TestScene test;
    const ECS::Entity platform = AddBody(test.scene, {0.f, 0.f, 0.f}, KinematicSensor());
    test.world.Reconcile();

    const glm::vec3 delta{0.5f, 0.f, 0.f};
    test.scene.GetMut<ECS::Transform>(platform)->position += delta;
    test.world.Update(kStep);

    // Placed, it would have no velocity and push nothing on the way.
    CHECK(test.world.GetBodyVelocity(platform).first.x == doctest::Approx(delta.x / kStep));
    CHECK(test.world.GetBodyPose(platform).position.x == doctest::Approx(delta.x));
}

TEST_CASE("Writing a dynamic body's Transform places it and keeps its velocity")
{
    TestScene test;
    const ECS::Entity ball = AddBody(test.scene, {0.f, 50.f, 0.f}, Ball(0.5f, false));
    Step(test.world, 20);
    const float falling = test.world.GetBodyVelocity(ball).first.y;
    REQUIRE(falling < 0.f);

    const glm::vec3 placed{3.f, 40.f, 3.f};
    test.scene.GetMut<ECS::Transform>(ball)->position = placed;
    test.world.Reconcile();

    CHECK(test.world.GetBodyPose(ball).position == placed);
    CHECK(test.world.GetBodyVelocity(ball).first.y == doctest::Approx(falling));
}

TEST_CASE("Teleport places a body and stops it")
{
    TestScene test;
    const ECS::Entity ball = AddBody(test.scene, {0.f, 50.f, 0.f}, Ball(0.5f, false));
    Step(test.world, 20);
    REQUIRE(test.world.GetBodyVelocity(ball).first.y < 0.f);

    const glm::vec3 target{3.f, 40.f, 3.f};
    test.world.Teleport(ball, Physics::Pose{glm::quat(1.f, 0.f, 0.f, 0.f), target});

    CHECK(test.world.GetBodyPose(ball).position == target);
    CHECK(test.world.GetBodyVelocity(ball).first == glm::vec3(0.f));
    CHECK(test.scene.Get<ECS::Transform>(ball)->position == target);
}

TEST_CASE("Writing a sleeping body's Transform moves it and wakes it")
{
    TestScene test;
    AddFloor(test.scene);
    const ECS::Entity ball = AddBody(test.scene, {0.f, 1.f, 0.f}, Ball(0.5f, false));
    Step(test.world, kSettleSteps);
    REQUIRE_FALSE(test.world.IsBodyActive(ball));

    const glm::vec3 lifted{0.f, 5.f, 0.f};
    test.scene.GetMut<ECS::Transform>(ball)->position = lifted;
    Step(test.world);

    CHECK(test.world.IsBodyActive(ball));
    CHECK(test.world.GetBodyPose(ball).position.y < lifted.y);
    CHECK(test.world.GetBodyPose(ball).position.y > lifted.y - 1.f);
}

TEST_CASE("Turning a moving body leaves where it is alone")
{
    // The Transform trails the body by the render blend. A write that only turns
    // it still holds that trailing position, which must not pull the body back.
    TestScene test;
    const ECS::Entity ball = AddBody(test.scene, {0.f, 50.f, 0.f}, Ball(0.5f, false));
    Step(test.world, 20);
    test.world.InterpolateTransforms(0.25f);
    const glm::vec3 before = test.world.GetBodyPose(ball).position;
    REQUIRE(test.scene.Get<ECS::Transform>(ball)->position != before);

    test.scene.GetMut<ECS::Transform>(ball)->rotation = glm::angleAxis(1.f, glm::vec3(0.f, 1.f, 0.f));
    test.world.Reconcile();

    CHECK(test.world.GetBodyPose(ball).position == before);
}

TEST_CASE("Scaling an entity scales its collider")
{
    TestScene test;
    const ECS::Entity box = AddBody(test.scene, {0.f, 0.f, 0.f}, Box({0.5f, 0.5f, 0.5f}, true));
    test.world.Reconcile();
    const glm::vec3 beyond{0.8f, 0.f, 0.f};
    REQUIRE_FALSE(SomethingAt(test.world, beyond));

    test.scene.GetMut<ECS::Transform>(box)->scale = glm::vec3(2.f);
    test.world.Reconcile();

    CHECK(SomethingAt(test.world, beyond));
}

TEST_CASE("A collider reports the scale its shape was built at")
{
    TestScene test;
    const glm::vec3 stretch{2.f, 3.f, 4.f};
    const ECS::Entity box = AddBody(test.scene, {0.f, 0.f, 0.f}, Box({0.5f, 0.5f, 0.5f}, true));
    const ECS::Entity ball = AddBody(test.scene, {5.f, 0.f, 0.f}, Ball(0.5f, true));
    test.scene.GetMut<ECS::Transform>(box)->scale = stretch;
    test.scene.GetMut<ECS::Transform>(ball)->scale = stretch;
    test.world.Reconcile();

    // A box takes any scale; a sphere has only one radius, so it is built at one
    // scale on every axis, and the overlay has to draw that rather than the
    // Transform's.
    CHECK(test.world.GetColliderScale(box) == stretch);
    const glm::vec3 round = test.world.GetColliderScale(ball);
    CHECK(round.x == round.y);
    CHECK(round.y == round.z);
    CHECK(round.x > 1.f);

    CHECK(test.world.GetColliderScale(ECS::NullEntity) == glm::vec3(1.f));
}

TEST_CASE("Making a static body dynamic lets it fall")
{
    TestScene test;
    const ECS::Entity box = AddBody(test.scene, {0.f, 10.f, 0.f}, Box({0.5f, 0.5f, 0.5f}, true));
    Step(test.world, 10);
    REQUIRE(test.world.GetBodyPose(box).position.y == doctest::Approx(10.f));

    test.scene.GetMut<Physics::RigidBodyDescriptor>(box)->isStatic = false;
    Step(test.world, 10);

    CHECK(test.world.GetBodyPose(box).position.y < 10.f);
}

TEST_CASE("Editing a collider's size resizes the live body")
{
    TestScene test;
    const ECS::Entity box = AddBody(test.scene, {0.f, 0.f, 0.f}, Box({0.5f, 0.5f, 0.5f}, true));
    test.world.Reconcile();
    const glm::vec3 beyond{1.5f, 0.f, 0.f};
    REQUIRE_FALSE(SomethingAt(test.world, beyond));

    test.scene.GetMut<Physics::RigidBodyDescriptor>(box)->halfExtents = {2.f, 0.5f, 2.f};
    test.world.Reconcile();

    CHECK(SomethingAt(test.world, beyond));
}

TEST_CASE("Turning on CCD reaches the live body")
{
    TestScene test;
    const ECS::Entity ball = AddBody(test.scene, {0.f, 10.f, 0.f}, Ball(0.5f, false));
    test.world.Reconcile();
    REQUIRE_FALSE(test.world.IsBodyCCDEnabled(ball));

    test.scene.GetMut<Physics::RigidBodyDescriptor>(ball)->enableCCD = true;
    test.world.Reconcile();

    CHECK(test.world.IsBodyCCDEnabled(ball));
}

TEST_CASE("Editing a walking character keeps its momentum")
{
    constexpr int32_t kWalkSteps = 30;
    constexpr float kWish = 3.f;
    TestScene test;
    AddFloor(test.scene);
    const ECS::Entity walker = AddCharacter(test.scene, {0.f, 0.f, 0.f});

    for (int32_t i = 0; i < kWalkSteps; ++i)
    {
        test.world.MoveCharacter(walker, {kWish, 0.f, 0.f}, false);
        Step(test.world);
    }
    const glm::vec3 walking = test.world.GetCharacterState(walker).velocity;
    REQUIRE(walking.x > 1.f);

    SUBCASE("a tuning edit")
    {
        test.scene.GetMut<Physics::CharacterDescriptor>(walker)->walkSpeed = 6.f;
    }
    SUBCASE("a capsule edit, which rebuilds it")
    {
        test.scene.GetMut<Physics::CharacterDescriptor>(walker)->radius = 0.35f;
    }
    test.world.Reconcile();

    CHECK(test.world.HasBody(walker));
    CHECK(test.world.GetCharacterState(walker).velocity.x == doctest::Approx(walking.x));
}
