/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPhysicsWriteback.cpp
/// @brief The writeback at the end of PhysicsWorld::Update: each moving body's
/// pose from the step that just ran, written into its Transform.
///
/// Transform is ACOMP(tracked), so the pose this writes must stamp a change tick:
/// propagation and network delta replication both decide whether to act on
/// `Scene::Changed<Transform>`. These cases pin the stamping down, pin down that
/// bodies the writeback skips are not stamped (an over-stamp would replicate the
/// whole scene's transforms every tick), and pin down that the writeback and a
/// Transform written by anything else never undo each other.

#include <doctest/doctest.h>

#include <cstdint>

#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Math/Matrix.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

#include "PhysicsTestScene.hpp"

using namespace Assisi;
using namespace Assisi::PhysicsTests;

namespace
{

/// A body of @p isStatic at @p position, built and stepped once.
ECS::Entity SpawnSimulatedBody(TestScene &test, glm::vec3 position, bool isStatic)
{
    const ECS::Entity e = AddBody(test.scene, position, Ball(0.5f, isStatic));
    Step(test.world);
    return e;
}

/// Steps long enough for a body dropped onto the floor to come to rest and fall
/// asleep.
constexpr int32_t kSettleSteps = 600;

} // namespace

TEST_CASE("Writeback: a body read from Transform right after a step is where the simulation put it")
{
    TestScene test;
    const ECS::Entity e = SpawnSimulatedBody(test, {0.f, 10.f, 0.f}, false);
    const glm::quat spin = glm::angleAxis(0.5f, glm::normalize(glm::vec3(1.f, 1.f, 0.f)));
    test.scene.GetMut<ECS::Transform>(e)->rotation = spin;
    Step(test.world, 5);

    const ECS::Transform &transform = *test.scene.Get<ECS::Transform>(e);
    const Physics::Pose body = test.world.GetBodyPose(e);
    CHECK(transform.position == body.position);
    CHECK(transform.rotation == body.rotation);
    CHECK(transform.position.y < 10.f);
}

TEST_CASE("Writeback: a step stamps the moved body's change tick")
{
    TestScene test;
    const ECS::Entity e = SpawnSimulatedBody(test, {0.f, 10.f, 0.f}, false);
    const uint64_t since = test.scene.CurrentChangeTick();

    Step(test.world);

    CHECK(test.scene.Changed<ECS::Transform>(e, since));
}

TEST_CASE("Writeback: a static body is skipped and never stamped")
{
    TestScene test;
    const ECS::Entity e = SpawnSimulatedBody(test, {0.f, 10.f, 0.f}, true);
    const uint64_t since = test.scene.CurrentChangeTick();

    Step(test.world);

    CHECK_FALSE(test.scene.Changed<ECS::Transform>(e, since));
    CHECK(test.scene.Get<ECS::Transform>(e)->position.y == 10.f);
}

TEST_CASE("Writeback: an entity without a body is untouched")
{
    TestScene test;
    const ECS::Entity simulated = SpawnSimulatedBody(test, {0.f, 10.f, 0.f}, false);
    const ECS::Entity placement = test.scene.Create();
    REQUIRE(test.scene.Add(placement, ECS::Transform{.position = {5.f, 5.f, 5.f}}) != nullptr);
    const uint64_t since = test.scene.CurrentChangeTick();

    Step(test.world);

    CHECK(test.scene.Changed<ECS::Transform>(simulated, since));
    CHECK_FALSE(test.scene.Changed<ECS::Transform>(placement, since));
}

TEST_CASE("Writeback: one moved body costs exactly one change tick")
{
    TestScene test;
    (void)SpawnSimulatedBody(test, {0.f, 10.f, 0.f}, false);
    const uint64_t since = test.scene.CurrentChangeTick();

    Step(test.world);

    // Position and rotation are both written, but through a single reference:
    // at hundreds of bodies a step, extra ticks would inflate every consumer's
    // bookmark for nothing.
    CHECK(test.scene.CurrentChangeTick() == since + 1);
}

TEST_CASE("Writeback: a Transform written between steps reaches the body rather than being overwritten")
{
    // A gizmo drag on a frame that runs no step: the next step pushes it to the
    // body before simulating, so the pose written back follows from it.
    TestScene test;
    const ECS::Entity e = SpawnSimulatedBody(test, {0.f, 10.f, 0.f}, false);

    const glm::vec3 dragged{4.f, 20.f, 4.f};
    test.scene.GetMut<ECS::Transform>(e)->position = dragged;
    Step(test.world);

    const glm::vec3 after = test.scene.Get<ECS::Transform>(e)->position;
    CHECK(after.x == doctest::Approx(dragged.x));
    CHECK(after.y == doctest::Approx(dragged.y).epsilon(0.01));
}

TEST_CASE("Writeback: a character's position is written and its rotation left to whoever turns it")
{
    TestScene test;
    AddFloor(test.scene);
    const ECS::Entity walker = AddCharacter(test.scene, {0.f, 0.f, 0.f});
    constexpr glm::vec3 kWalk{3.f, 0.f, 0.f};
    const glm::quat turned = glm::angleAxis(1.f, glm::vec3(0.f, 1.f, 0.f));
    test.scene.GetMut<ECS::Transform>(walker)->rotation = turned;

    for (int32_t i = 0; i < 30; ++i)
    {
        test.world.MoveCharacter(walker, kWalk, /*jump=*/ false);
        Step(test.world);
    }

    const ECS::Transform *after = test.scene.Get<ECS::Transform>(walker);
    CHECK(after->position.x > 0.f);
    CHECK(after->rotation == turned);
}

TEST_CASE("Writeback: a scale written between steps reaches the collider")
{
    TestScene test;
    const ECS::Entity e = SpawnSimulatedBody(test, {0.f, 10.f, 0.f}, false);

    const glm::vec3 grown{2.f, 2.f, 2.f};
    test.scene.GetMut<ECS::Transform>(e)->scale = grown;
    Step(test.world);

    CHECK(test.world.GetColliderScale(e) == grown);
}

TEST_CASE("Writeback: the writeback's own writes are never pushed back to the body")
{
    // The same fall run twice, once with the Transform read and re-stamped
    // between steps by a no-op mutable access. Were the writeback's own pose
    // treated as an outside move, the second body's fall would be disturbed.
    TestScene plain;
    TestScene touched;
    const ECS::Entity a = AddBody(plain.scene, {0.f, 30.f, 0.f}, Ball(0.5f, false));
    const ECS::Entity b = AddBody(touched.scene, {0.f, 30.f, 0.f}, Ball(0.5f, false));

    constexpr int32_t kSteps = 30;
    for (int32_t i = 0; i < kSteps; ++i)
    {
        Step(plain.world);
        Step(touched.world);
        (void)touched.scene.GetMut<ECS::Transform>(b);
    }

    CHECK(touched.world.GetBodyPose(b).position.y == plain.world.GetBodyPose(a).position.y);
    CHECK(touched.world.GetBodyVelocity(b).first == plain.world.GetBodyVelocity(a).first);
}

TEST_CASE("Writeback: a body that falls asleep is written once at rest, then left alone")
{
    TestScene test;
    AddFloor(test.scene);
    const ECS::Entity e = AddBody(test.scene, {0.f, 2.f, 0.f}, Ball(0.5f, false));
    Step(test.world, kSettleSteps);
    REQUIRE_FALSE(test.world.IsBodyActive(e));
    CHECK(test.scene.Get<ECS::Transform>(e)->position == test.world.GetBodyPose(e).position);

    const uint64_t since = test.scene.CurrentChangeTick();
    Step(test.world, 10);

    CHECK_FALSE(test.scene.Changed<ECS::Transform>(e, since));
}

TEST_CASE("Writeback: a teleport is drawn at its destination, not slid to")
{
    TestScene test;
    const ECS::Entity e = SpawnSimulatedBody(test, {0.f, 10.f, 0.f}, false);
    uint64_t tick = ECS::PropagateTransforms(test.scene, 0);

    {
        const ECS::FixedStepScope step(test.scene);
        Step(test.world);
        test.world.Teleport(e, Physics::Pose{glm::quat(1.f, 0.f, 0.f, 0.f), {20.f, 5.f, 0.f}});
    }
    ECS::SetBlendAlpha(test.scene, 0.5f);
    tick = ECS::PropagateTransforms(test.scene, tick);

    CHECK(Math::TranslationOf(test.scene.Get<ECS::WorldMatrix>(e)->matrix) == glm::vec3(20.f, 5.f, 0.f));
}

TEST_CASE("Writeback: a falling body is drawn between its last two steps")
{
    TestScene test;
    const ECS::Entity e = SpawnSimulatedBody(test, {0.f, 10.f, 0.f}, false);
    Step(test.world, 10);
    uint64_t tick = ECS::PropagateTransforms(test.scene, 0);
    const float before = test.scene.Get<ECS::Transform>(e)->position.y;

    {
        const ECS::FixedStepScope step(test.scene);
        Step(test.world);
    }
    const float after = test.scene.Get<ECS::Transform>(e)->position.y;
    REQUIRE(after < before);
    ECS::SetBlendAlpha(test.scene, 0.5f);
    tick = ECS::PropagateTransforms(test.scene, tick);

    const glm::mat4 &world = test.scene.Get<ECS::WorldMatrix>(e)->matrix;
    CHECK(Math::TranslationOf(world).y == doctest::Approx(0.5f * (before + after)));
}
