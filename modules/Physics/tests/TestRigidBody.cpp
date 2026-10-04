/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestRigidBody.cpp
/// @brief Every RigidBody and Collider field reaches the body built from it, and
/// an edit to one reaches the live body without rebuilding it.
///
/// Each case runs the same scene twice, once with the field at a value that
/// should make a difference and once without, so a field that is copied
/// nowhere fails rather than passing on whatever the default happens to do.

#include <doctest/doctest.h>

#include <cmath>
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

/// One second of simulation.
constexpr int32_t kSecond = 60;

/// Long enough for a body dropped onto the floor to come to rest and fall
/// asleep.
constexpr int32_t kSettleSteps = 600;

/// A dynamic half-metre ball at @p position, tuned by @p rigidBody.
ECS::Entity AddBall(TestScene &test, glm::vec3 position, const Physics::RigidBody &rigidBody)
{
    BodySpec spec = Ball(0.5f, false);
    spec.rigidBody = rigidBody;
    return AddBody(test.scene, position, spec);
}

/// A RigidBody at @p gravityScale, its damping at @p damping.
Physics::RigidBody Tuned(float gravityScale, float damping = 0.05f)
{
    Physics::RigidBody rigidBody;
    rigidBody.gravityScale = gravityScale;
    rigidBody.linearDamping = damping;
    rigidBody.angularDamping = damping;
    return rigidBody;
}

/// Sets @p entity's velocity through its BodyState.
void Launch(TestScene &test, ECS::Entity entity, glm::vec3 linear, glm::vec3 angular = glm::vec3(0.f))
{
    Physics::BodyState &state = *test.scene.GetMut<Physics::BodyState>(entity);
    state.linearVelocity = linear;
    state.angularVelocity = angular;
}

/// A small sphere to ask what is at a point.
Physics::PhysicsWorld::ColliderShapeDesc Probe()
{
    Physics::PhysicsWorld::ColliderShapeDesc probe;
    probe.shape = Physics::ColliderShape::Sphere;
    probe.radius = 0.1f;
    return probe;
}

float Height(const TestScene &test, ECS::Entity entity)
{
    return test.world.GetBodyPose(entity).position.y;
}

bool IsActive(const TestScene &test, ECS::Entity entity)
{
    std::vector<ECS::Entity> active;
    test.world.ActiveBodies(active);
    for (const ECS::Entity candidate : active)
    {
        if (candidate == entity)
        {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("RigidBody: gravityScale multiplies the fall")
{
    TestScene test;
    const ECS::Entity still = AddBall(test, {0.f, 100.f, 0.f}, Tuned(0.f));
    const ECS::Entity normal = AddBall(test, {10.f, 100.f, 0.f}, Tuned(1.f));
    const ECS::Entity heavy = AddBall(test, {20.f, 100.f, 0.f}, Tuned(2.f));
    Step(test.world, kSecond);

    CHECK(Height(test, still) == doctest::Approx(100.f));
    const float normalDrop = 100.f - Height(test, normal);
    const float heavyDrop = 100.f - Height(test, heavy);
    REQUIRE(normalDrop > 1.f);
    CHECK(heavyDrop == doctest::Approx(2.f * normalDrop).epsilon(0.05));
}

TEST_CASE("RigidBody: linearDamping slows a moving body")
{
    TestScene test;
    const ECS::Entity damped =
        AddBall(test, {0.f, 0.f, 0.f}, Tuned(0.f, 2.f));
    const ECS::Entity free =
        AddBall(test, {0.f, 0.f, 10.f}, Tuned(0.f, 0.f));
    Launch(test, damped, {5.f, 0.f, 0.f});
    Launch(test, free, {5.f, 0.f, 0.f});
    Step(test.world, kSecond);

    CHECK(test.world.GetBodyVelocity(free).first.x == doctest::Approx(5.f));
    CHECK(test.world.GetBodyVelocity(damped).first.x < 2.5f);
}

TEST_CASE("RigidBody: angularDamping slows a spinning body")
{
    TestScene test;
    const ECS::Entity damped =
        AddBall(test, {0.f, 0.f, 0.f}, Tuned(0.f, 2.f));
    const ECS::Entity free =
        AddBall(test, {0.f, 0.f, 10.f}, Tuned(0.f, 0.f));
    Launch(test, damped, glm::vec3(0.f), {0.f, 5.f, 0.f});
    Launch(test, free, glm::vec3(0.f), {0.f, 5.f, 0.f});
    Step(test.world, kSecond);

    CHECK(test.world.GetBodyVelocity(free).second.y == doctest::Approx(5.f));
    CHECK(test.world.GetBodyVelocity(damped).second.y < 2.5f);
}

TEST_CASE("RigidBody: a locked translation keeps the body from moving along it")
{
    TestScene test;
    Physics::RigidBody locked;
    locked.lockedAxes = Core::Bitmask<Physics::LockedAxis>::Of(Physics::LockedAxis::LinearY);
    const ECS::Entity held = AddBall(test, {0.f, 10.f, 0.f}, locked);
    const ECS::Entity loose = AddBall(test, {10.f, 10.f, 0.f}, Physics::RigidBody{});
    Step(test.world, kSecond);

    CHECK(Height(test, held) == doctest::Approx(10.f));
    CHECK(Height(test, loose) < 9.f);
}

TEST_CASE("RigidBody: locked rotations keep the body from turning")
{
    TestScene test;
    Physics::RigidBody locked = Tuned(0.f);
    locked.lockedAxes = Core::Bitmask<Physics::LockedAxis>::Of(Physics::LockedAxis::AngularX)
                            .With(Physics::LockedAxis::AngularY)
                            .With(Physics::LockedAxis::AngularZ);
    const ECS::Entity held = AddBall(test, {0.f, 0.f, 0.f}, locked);
    const ECS::Entity loose = AddBall(test, {10.f, 0.f, 0.f}, Tuned(0.f));
    Launch(test, held, glm::vec3(0.f), {0.f, 3.f, 0.f});
    Launch(test, loose, glm::vec3(0.f), {0.f, 3.f, 0.f});
    Step(test.world, kSecond);

    const glm::quat identity(1.f, 0.f, 0.f, 0.f);
    CHECK(std::abs(glm::dot(test.world.GetBodyPose(held).rotation, identity)) == doctest::Approx(1.f));
    CHECK(std::abs(glm::dot(test.world.GetBodyPose(loose).rotation, identity)) < 0.99f);
}

TEST_CASE("RigidBody: in a head-on collision the lighter body is the one turned back")
{
    // Equal speeds, perfectly elastic: the heavier ball carries on and the
    // lighter one reverses. With the masses ignored both would simply swap.
    TestScene test;
    BodySpec heavySpec = Ball(0.5f, false);
    heavySpec.collider.restitution = 1.f;
    heavySpec.rigidBody = Tuned(0.f, 0.f);
    heavySpec.rigidBody->mass = 50.f;
    BodySpec lightSpec = heavySpec;
    lightSpec.rigidBody->mass = 1.f;
    const ECS::Entity heavy = AddBody(test.scene, {-2.f, 0.f, 0.f}, heavySpec);
    const ECS::Entity light = AddBody(test.scene, {2.f, 0.f, 0.f}, lightSpec);
    Launch(test, heavy, {3.f, 0.f, 0.f});
    Launch(test, light, {-3.f, 0.f, 0.f});
    Step(test.world, kSecond);

    CHECK(test.world.GetBodyVelocity(heavy).first.x > 0.f);
    CHECK(test.world.GetBodyVelocity(light).first.x > 3.f);
}

TEST_CASE("RigidBody: ccd stops a fast small body at a thin floor it would otherwise pass")
{
    constexpr float kFast = 300.f;
    BodySpec thin = Box({5.f, 0.02f, 5.f}, true);
    BodySpec bullet = Ball(0.05f, false);
    bullet.rigidBody->gravityScale = 0.f;

    TestScene discrete;
    (void)AddBody(discrete.scene, {0.f, 0.f, 0.f}, thin);
    const ECS::Entity passing = AddBody(discrete.scene, {0.f, 2.f, 0.f}, bullet);
    Launch(discrete, passing, {0.f, -kFast, 0.f});
    Step(discrete.world, 10);

    TestScene swept;
    (void)AddBody(swept.scene, {0.f, 0.f, 0.f}, thin);
    bullet.rigidBody->ccd = true;
    const ECS::Entity stopped = AddBody(swept.scene, {0.f, 2.f, 0.f}, bullet);
    Launch(swept, stopped, {0.f, -kFast, 0.f});
    Step(swept.world, 10);

    CHECK(Height(discrete, passing) < -1.f);
    CHECK(Height(swept, stopped) > -0.1f);
}

TEST_CASE("RigidBody: allowSleep decides whether a resting body falls asleep")
{
    TestScene test;
    AddFloor(test.scene);
    Physics::RigidBody awake;
    awake.allowSleep = false;
    const ECS::Entity sleeper = AddBall(test, {0.f, 1.f, 0.f}, Physics::RigidBody{});
    const ECS::Entity insomniac = AddBall(test, {5.f, 1.f, 0.f}, awake);
    Step(test.world, kSettleSteps);

    CHECK_FALSE(IsActive(test, sleeper));
    CHECK(IsActive(test, insomniac));
}

TEST_CASE("Collider: friction holds back a body sliding across a floor")
{
    TestScene test;
    AddFloor(test.scene);
    BodySpec slick = Box({0.5f, 0.5f, 0.5f}, false);
    slick.collider.friction = 0.f;
    BodySpec rough = slick;
    rough.collider.friction = 1.f;
    const ECS::Entity sliding = AddBody(test.scene, {0.f, 0.5f, 0.f}, slick);
    const ECS::Entity held = AddBody(test.scene, {0.f, 0.5f, 10.f}, rough);
    Step(test.world, 10);
    Launch(test, sliding, {3.f, 0.f, 0.f});
    Launch(test, held, {3.f, 0.f, 0.f});
    Step(test.world, kSecond);

    CHECK(test.world.GetBodyVelocity(sliding).first.x > 2.f);
    CHECK(test.world.GetBodyVelocity(held).first.x == doctest::Approx(0.f).epsilon(0.05));
}

TEST_CASE("Collider: restitution decides whether a dropped ball comes back up")
{
    TestScene test;
    AddFloor(test.scene);
    BodySpec dead = Ball(0.5f, false);
    dead.collider.restitution = 0.f;
    BodySpec lively = dead;
    lively.collider.restitution = 1.f;
    const ECS::Entity resting = AddBody(test.scene, {0.f, 3.f, 0.f}, dead);
    const ECS::Entity bouncing = AddBody(test.scene, {5.f, 3.f, 0.f}, lively);

    float restingPeak = 0.f;
    float bouncingPeak = 0.f;
    bool landed = false;
    for (int32_t i = 0; i < 3 * kSecond; ++i)
    {
        Step(test.world);
        landed = landed || Height(test, bouncing) < 0.6f;
        if (landed)
        {
            restingPeak = std::max(restingPeak, Height(test, resting));
            bouncingPeak = std::max(bouncingPeak, Height(test, bouncing));
        }
    }
    REQUIRE(landed);
    CHECK(restingPeak < 0.6f);
    CHECK(bouncingPeak > 2.f);
}

TEST_CASE("RigidBody: a kinematic body ignores gravity and is moved by its Transform")
{
    TestScene test;
    BodySpec spec = Box({0.5f, 0.5f, 0.5f}, false);
    spec.rigidBody->motion = Physics::MotionType::Kinematic;
    const ECS::Entity platform = AddBody(test.scene, {0.f, 5.f, 0.f}, spec);
    Step(test.world, kSecond);
    REQUIRE(Height(test, platform) == doctest::Approx(5.f));

    test.scene.GetMut<ECS::Transform>(platform)->position = {3.f, 5.f, 0.f};
    Step(test.world);

    CHECK(test.world.GetBodyPose(platform).position.x == doctest::Approx(3.f));

    // Swept there over one step, and then stays: the sweep's velocity is for
    // the step it was asked for, not every step after it.
    Step(test.world, 10);
    CHECK(test.world.GetBodyPose(platform).position.x == doctest::Approx(3.f));
    const std::vector<ECS::Entity> found =
        test.world.Overlap(Probe(), Physics::Pose{glm::quat(1.f, 0.f, 0.f, 0.f), {3.f, 5.f, 0.f}},
                           Physics::CollisionFilter{}, ECS::NullEntity);
    CHECK(found.size() == 1u);
}

TEST_CASE("RigidBody: a kinematic body pushes a dynamic one out of its way")
{
    TestScene test;
    BodySpec pusher = Box({0.5f, 0.5f, 0.5f}, false);
    pusher.rigidBody->motion = Physics::MotionType::Kinematic;
    const ECS::Entity wall = AddBody(test.scene, {0.f, 0.f, 0.f}, pusher);
    const ECS::Entity ball = AddBall(test, {1.2f, 0.f, 0.f}, Tuned(0.f));

    for (int32_t i = 1; i <= kSecond; ++i)
    {
        test.scene.GetMut<ECS::Transform>(wall)->position.x = 0.05f * static_cast<float>(i);
        Step(test.world);
    }

    CHECK(test.world.GetBodyPose(ball).position.x > 3.f);
}

TEST_CASE("RigidBody: an edit reaches the live body and keeps its motion")
{
    TestScene test;
    const ECS::Entity ball = AddBall(test, {0.f, 100.f, 0.f}, Physics::RigidBody{});
    Step(test.world, 30);
    const glm::vec3 before = test.world.GetBodyVelocity(ball).first;
    REQUIRE(before.y < -1.f);

    test.scene.GetMut<Physics::RigidBody>(ball)->gravityScale = 0.f;
    test.world.Reconcile();
    CHECK(test.world.GetBodyVelocity(ball).first == before);

    Step(test.world, 30);
    CHECK(test.world.GetBodyVelocity(ball).first.y == doctest::Approx(before.y).epsilon(0.05));
}

TEST_CASE("RigidBody: a mass edit after a shape edit is kept")
{
    // A shape change resets the mass to the new shape's own; the authored mass
    // has to be put back after it.
    TestScene test;
    BodySpec spec = Ball(0.5f, false);
    spec.collider.restitution = 1.f;
    spec.rigidBody = Tuned(0.f, 0.f);
    spec.rigidBody->mass = 50.f;
    const ECS::Entity heavy = AddBody(test.scene, {-2.f, 0.f, 0.f}, spec);
    spec.rigidBody->mass = 1.f;
    const ECS::Entity light = AddBody(test.scene, {2.f, 0.f, 0.f}, spec);
    test.world.Reconcile();
    test.scene.GetMut<Physics::Collider>(heavy)->radius = 0.4f;
    test.world.Reconcile();

    Launch(test, heavy, {3.f, 0.f, 0.f});
    Launch(test, light, {-3.f, 0.f, 0.f});
    Step(test.world, kSecond);

    CHECK(test.world.GetBodyVelocity(heavy).first.x > 0.f);
}

TEST_CASE("RigidBody: switching to kinematic stops the fall, and back lets it fall again")
{
    TestScene test;
    const ECS::Entity ball = AddBall(test, {0.f, 100.f, 0.f}, Physics::RigidBody{});
    Step(test.world, 10);

    test.scene.GetMut<Physics::RigidBody>(ball)->motion = Physics::MotionType::Kinematic;
    Step(test.world);
    const float held = Height(test, ball);
    Step(test.world, kSecond);
    CHECK(Height(test, ball) == doctest::Approx(held));

    test.scene.GetMut<Physics::RigidBody>(ball)->motion = Physics::MotionType::Dynamic;
    Step(test.world, kSecond);
    CHECK(Height(test, ball) < held - 1.f);
}

TEST_CASE("Collider: the offset moves the shape away from the entity's origin")
{
    // A ball whose shape sits a metre below its origin rests with its origin a
    // metre higher than one without.
    TestScene test;
    AddFloor(test.scene);
    BodySpec lowered = Ball(0.5f, false);
    lowered.collider.offsetPosition = {0.f, -1.f, 0.f};
    const ECS::Entity plain = AddBody(test.scene, {0.f, 3.f, 0.f}, Ball(0.5f, false));
    const ECS::Entity offset = AddBody(test.scene, {5.f, 3.f, 0.f}, lowered);
    Step(test.world, kSettleSteps);

    CHECK(Height(test, plain) == doctest::Approx(0.5f).epsilon(0.05));
    CHECK(Height(test, offset) - Height(test, plain) == doctest::Approx(1.f).epsilon(0.05));
}

TEST_CASE("Collider: the offset is scaled with the entity")
{
    TestScene test;
    AddFloor(test.scene);
    BodySpec lowered = Box({0.5f, 0.5f, 0.5f}, false);
    lowered.collider.offsetPosition = {0.f, -1.f, 0.f};
    const ECS::Entity box = AddBody(test.scene, {0.f, 5.f, 0.f}, lowered);
    test.scene.GetMut<ECS::Transform>(box)->scale = glm::vec3(2.f);
    Step(test.world, kSettleSteps);

    // Half extents of 1 after scaling, two metres below the origin: the origin
    // rests at 1 + 2 above the floor.
    CHECK(Height(test, box) == doctest::Approx(3.f).epsilon(0.05));
}

TEST_CASE("Collider: on its own it is static geometry with no BodyState")
{
    TestScene test;
    const ECS::Entity wall = AddBody(test.scene, {0.f, 10.f, 0.f}, Box({0.5f, 0.5f, 0.5f}, true));
    Step(test.world, kSecond);

    CHECK(Height(test, wall) == doctest::Approx(10.f));
    CHECK_FALSE(test.scene.Has<Physics::BodyState>(wall));
    CHECK_FALSE(IsActive(test, wall));
}

TEST_CASE("Collider: a sphere stretched along one axis is built at one scale")
{
    TestScene test;
    const ECS::Entity ball = AddBody(test.scene, {0.f, 10.f, 0.f}, Ball(0.5f, true));
    test.scene.GetMut<ECS::Transform>(ball)->scale = {1.f, 4.f, 1.f};
    test.world.Reconcile();

    const glm::vec3 scale = test.world.GetColliderScale(ball);
    CHECK(scale.x == doctest::Approx(scale.y));
    CHECK(scale.y == doctest::Approx(scale.z));
    CHECK(scale.x == doctest::Approx(2.f));
}
