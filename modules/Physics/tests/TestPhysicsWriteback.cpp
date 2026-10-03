/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPhysicsWriteback.cpp
/// @brief PhysicsWorld::InterpolateTransforms — the engine's one system that
/// writes ECS::Transform every frame from outside the ECS.
///
/// Transform is ACOMP(tracked), so the pose this writes must stamp a change tick:
/// PropagateTransforms's dirty-skip and network delta replication both decide
/// whether to act on `Scene::Changed<Transform>`. These cases pin the stamping
/// down, pin down that bodies the writeback skips are not stamped (an over-stamp
/// would replicate the whole scene's transforms every tick), and pin down that
/// the writeback and a Transform written by anything else never undo each other.

#include <doctest/doctest.h>

#include <cstdint>

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

/// A body of @p isStatic at @p position, stepped twice so both interpolation
/// snapshots straddle a real displacement.
ECS::Entity SpawnSimulatedBody(TestScene &test, glm::vec3 position, bool isStatic)
{
    const ECS::Entity e = AddBody(test.scene, position, Ball(0.5f, isStatic));
    Step(test.world, 2);
    return e;
}

/// Steps long enough for a body dropped onto the floor to come to rest and fall
/// asleep.
constexpr int32_t kSettleSteps = 600;

} // namespace

TEST_CASE("InterpolateTransforms: the physics writeback stamps the Transform change tick")
{
    TestScene test;
    const ECS::Entity e = SpawnSimulatedBody(test, {0.f, 10.f, 0.f}, false);

    // Bookmark taken after every setup write, exactly as a replication or
    // propagation consumer would record it at the end of its own pass.
    const uint64_t since = test.scene.CurrentChangeTick();
    REQUIRE_FALSE(test.scene.Changed<ECS::Transform>(e, since));

    test.world.InterpolateTransforms(0.5f);

    // The body fell, so the pose really did change...
    const ECS::Transform *t = test.scene.Get<ECS::Transform>(e);
    REQUIRE(t != nullptr);
    REQUIRE(t->position.y < 10.f);

    // ...and the change must be observable.
    CHECK(test.scene.Changed<ECS::Transform>(e, since));
}

TEST_CASE("InterpolateTransforms: a static body is skipped and never stamped")
{
    TestScene test;
    const ECS::Entity e = SpawnSimulatedBody(test, {0.f, 10.f, 0.f}, true);

    const uint64_t since = test.scene.CurrentChangeTick();
    test.world.InterpolateTransforms(0.5f);

    CHECK_FALSE(test.scene.Changed<ECS::Transform>(e, since));
    CHECK(test.scene.CurrentChangeTick() == since);
    CHECK(test.scene.Get<ECS::Transform>(e)->position.y == doctest::Approx(10.f));
}

TEST_CASE("InterpolateTransforms: an entity without a body is untouched")
{
    TestScene test;
    const ECS::Entity simulated = SpawnSimulatedBody(test, {0.f, 10.f, 0.f}, false);
    const ECS::Entity placement = test.scene.Create();
    REQUIRE(test.scene.Add(placement, ECS::Transform{.position = {5.f, 5.f, 5.f}}) != nullptr);

    const uint64_t since = test.scene.CurrentChangeTick();
    test.world.InterpolateTransforms(0.5f);

    CHECK(test.scene.Changed<ECS::Transform>(simulated, since));
    CHECK_FALSE(test.scene.Changed<ECS::Transform>(placement, since));
}

TEST_CASE("InterpolateTransforms: one moved body costs exactly one change tick")
{
    TestScene test;
    (void)SpawnSimulatedBody(test, {0.f, 10.f, 0.f}, false);

    const uint64_t since = test.scene.CurrentChangeTick();
    test.world.InterpolateTransforms(0.5f);

    // Position and rotation are both written, but through a single reference:
    // every mutable access stamps, and at hundreds of bodies a frame the extra
    // ticks would inflate every consumer's bookmark for nothing.
    CHECK(test.scene.CurrentChangeTick() == since + 1);
}

TEST_CASE("InterpolateTransforms: a Transform written since the last writeback is left for the reconcile")
{
    // A gizmo drag on a frame that runs no fixed step: the write has not reached
    // the body yet, and the writeback must not put the old pose back over it.
    TestScene test;
    const ECS::Entity e = SpawnSimulatedBody(test, {0.f, 10.f, 0.f}, false);
    test.world.InterpolateTransforms(0.5f);

    const glm::vec3 dragged{4.f, 20.f, 4.f};
    test.scene.GetMut<ECS::Transform>(e)->position = dragged;
    test.world.InterpolateTransforms(0.75f);

    CHECK(test.scene.Get<ECS::Transform>(e)->position == dragged);
}

TEST_CASE("InterpolateTransforms: turning a character between steps does not stop it being drawn moving")
{
    // A look system turns the character every frame. The writeback never writes
    // a character's rotation, so that write holds nothing back: the position has
    // to keep following, or the character renders frozen between steps and
    // jumps at each one.
    TestScene test;
    AddFloor(test.scene);
    const ECS::Entity walker = AddCharacter(test.scene, {0.f, 0.f, 0.f});
    constexpr glm::vec3 kWalk{3.f, 0.f, 0.f};
    for (int32_t i = 0; i < 30; ++i)
    {
        test.world.MoveCharacter(walker, kWalk, /*jump=*/ false);
        Step(test.world);
    }
    test.world.InterpolateTransforms(0.f);
    const glm::vec3 before = test.scene.Get<ECS::Transform>(walker)->position;

    const glm::quat turned = glm::angleAxis(1.f, glm::vec3(0.f, 1.f, 0.f));
    test.scene.GetMut<ECS::Transform>(walker)->rotation = turned;
    test.world.InterpolateTransforms(1.f);

    const ECS::Transform *after = test.scene.Get<ECS::Transform>(walker);
    CHECK(after->position.x > before.x);
    CHECK(after->rotation == turned);
}

TEST_CASE("InterpolateTransforms: a mutable access that changes nothing does not stop the writeback")
{
    TestScene test;
    const ECS::Entity e = SpawnSimulatedBody(test, {0.f, 10.f, 0.f}, false);
    test.world.InterpolateTransforms(0.f);
    const float before = test.scene.Get<ECS::Transform>(e)->position.y;

    (void)test.scene.GetMut<ECS::Transform>(e);
    test.world.InterpolateTransforms(1.f);

    CHECK(test.scene.Get<ECS::Transform>(e)->position.y < before);
}

TEST_CASE("InterpolateTransforms: a scale written between steps still reaches the collider")
{
    TestScene test;
    const ECS::Entity e = SpawnSimulatedBody(test, {0.f, 10.f, 0.f}, false);
    test.world.InterpolateTransforms(0.f);

    const glm::vec3 grown{2.f, 2.f, 2.f};
    test.scene.GetMut<ECS::Transform>(e)->scale = grown;
    test.world.InterpolateTransforms(1.f);
    test.world.Reconcile();

    CHECK(test.world.GetColliderScale(e) == grown);
}

TEST_CASE("InterpolateTransforms: the writeback's own writes are never pushed back to the body")
{
    // The same fall run twice, once with a render-time blend between steps.
    // Were the blended Transform read back as someone else's write, the second
    // body would be dragged a fraction of a step behind every frame.
    TestScene plain;
    TestScene rendered;
    const ECS::Entity a = AddBody(plain.scene, {0.f, 30.f, 0.f}, Ball(0.5f, false));
    const ECS::Entity b = AddBody(rendered.scene, {0.f, 30.f, 0.f}, Ball(0.5f, false));

    constexpr int32_t kSteps = 30;
    for (int32_t i = 0; i < kSteps; ++i)
    {
        Step(plain.world);
        Step(rendered.world);
        rendered.world.InterpolateTransforms(0.5f);
    }

    CHECK(rendered.world.GetBodyPose(b).position.y == doctest::Approx(plain.world.GetBodyPose(a).position.y));
}

TEST_CASE("InterpolateTransforms: a body that falls asleep ends with its Transform exactly at rest")
{
    TestScene test;
    AddFloor(test.scene);
    const ECS::Entity e = AddBody(test.scene, {0.f, 2.f, 0.f}, Ball(0.5f, false));

    for (int32_t i = 0; i < kSettleSteps; ++i)
    {
        Step(test.world);
        test.world.InterpolateTransforms(0.5f);
    }
    REQUIRE_FALSE(test.world.IsBodyActive(e));

    CHECK(test.scene.Get<ECS::Transform>(e)->position == test.world.GetBodyPose(e).position);
}
