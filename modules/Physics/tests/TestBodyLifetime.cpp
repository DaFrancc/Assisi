/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBodyLifetime.cpp
/// @brief A body lives exactly as long as its entity's Collider or Character and Transform,
///        whoever adds or removes them and however the scene learns of it.
///
/// Nothing in these cases asks the world to create or destroy anything. Each
/// changes the scene and checks the world followed, because that is now the only
/// way a body comes or goes: a leak is a body left colliding after its entity
/// went, and a gap is an entity that should be simulated and is not.

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
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

/// More removals than the scene's removal log holds, so a reader whose cursor is
/// older than all of them is told the log is incomplete.
constexpr int32_t kLogOverflow = 5000;

} // namespace

TEST_CASE("An entity with a Collider and a Transform has a body after a reconcile")
{
    TestScene test;
    const glm::vec3 spot{3.f, 0.f, 0.f};
    const ECS::Entity box = AddBody(test.scene, spot, Box({0.5f, 0.5f, 0.5f}, true));

    CHECK_FALSE(test.world.HasBody(box));
    test.world.Reconcile();
    CHECK(test.world.HasBody(box));
    CHECK(SomethingAt(test.world, spot));
}

TEST_CASE("Asking about an entity with no body answers with nothing")
{
    TestScene test;
    const ECS::Entity bare = test.scene.Create();

    CHECK_FALSE(test.world.HasBody(bare));
    CHECK(test.world.GetBodyPose(bare).position == glm::vec3(0.f));
    CHECK(test.world.GetBodyVelocity(bare).first == glm::vec3(0.f));
    CHECK_FALSE(test.world.IsBodyActive(bare));
    test.world.Teleport(bare, Physics::Pose{});
}

TEST_CASE("Destroying an entity destroys its body")
{
    TestScene test;
    const glm::vec3 spot{3.f, 0.f, 0.f};
    const ECS::Entity box = AddBody(test.scene, spot, Box({0.5f, 0.5f, 0.5f}, true));
    test.world.Reconcile();
    REQUIRE(SomethingAt(test.world, spot));

    test.scene.Destroy(box);
    test.scene.FlushDestroyed();
    Step(test.world);

    CHECK_FALSE(test.world.HasBody(box));
    CHECK_FALSE(SomethingAt(test.world, spot));
}

TEST_CASE("Destroying an entity destroys its character")
{
    TestScene test;
    const glm::vec3 feet{-4.f, 0.f, 0.f};
    const ECS::Entity character = AddCharacter(test.scene, feet);
    test.world.Reconcile();
    REQUIRE(test.world.HasBody(character));
    REQUIRE(SomethingAt(test.world, feet + glm::vec3(0.f, 0.5f, 0.f)));

    test.scene.Destroy(character);
    test.scene.FlushDestroyed();
    Step(test.world);

    CHECK_FALSE(test.world.HasBody(character));
    CHECK_FALSE(SomethingAt(test.world, feet + glm::vec3(0.f, 0.5f, 0.f)));
}

TEST_CASE("Removing the Collider destroys the body, and adding it back builds a new one")
{
    TestScene test;
    const glm::vec3 spot{0.f, 0.f, 2.f};
    const ECS::Entity box = AddBody(test.scene, spot, Box({0.5f, 0.5f, 0.5f}, true));
    test.world.Reconcile();

    REQUIRE(test.scene.Remove<Physics::Collider>(box) == ECS::RemoveResult::Removed);
    test.world.Reconcile();
    CHECK_FALSE(test.world.HasBody(box));
    CHECK_FALSE(SomethingAt(test.world, spot));

    REQUIRE(test.scene.Add(box, Box({0.5f, 0.5f, 0.5f}, true).collider) != nullptr);
    test.world.Reconcile();
    CHECK(test.world.HasBody(box));
    CHECK(SomethingAt(test.world, spot));
}

TEST_CASE("A body is destroyed even when the removal log no longer reaches back to it")
{
    TestScene test;
    const glm::vec3 spot{5.f, 0.f, 5.f};
    const ECS::Entity box = AddBody(test.scene, spot, Box({0.5f, 0.5f, 0.5f}, true));
    test.world.Reconcile();

    test.scene.Destroy(box);
    test.scene.FlushDestroyed();

    // Enough unrelated removals to push the destroy out of every log the world
    // reads, so it has to find the dead body by checking what it holds.
    const ECS::Entity churn = test.scene.Create();
    REQUIRE(test.scene.Add(churn, ECS::Transform{}) != nullptr);
    for (int32_t i = 0; i < kLogOverflow; ++i)
    {
        REQUIRE(test.scene.Add(churn, Box({0.5f, 0.5f, 0.5f}, true).collider) != nullptr);
        REQUIRE(test.scene.Remove<Physics::Collider>(churn) == ECS::RemoveResult::Removed);
    }
    std::vector<ECS::Entity> removed;
    REQUIRE_FALSE(test.scene.RemovedSince<Physics::Collider>(0, removed));

    test.world.Reconcile();
    CHECK_FALSE(test.world.HasBody(box));
    CHECK_FALSE(SomethingAt(test.world, spot));
}

TEST_CASE("A body for a destroyed entity does not survive into the next life of its index")
{
    TestScene test;
    const glm::vec3 oldSpot{6.f, 0.f, 0.f};
    const ECS::Entity first = AddBody(test.scene, oldSpot, Box({0.5f, 0.5f, 0.5f}, true));
    test.world.Reconcile();

    test.scene.Destroy(first);
    test.scene.FlushDestroyed();
    const glm::vec3 newSpot{-6.f, 0.f, 0.f};
    const ECS::Entity second = AddBody(test.scene, newSpot, Box({0.5f, 0.5f, 0.5f}, true));
    REQUIRE(second.index == first.index);
    REQUIRE(second.generation != first.generation);

    test.world.Reconcile();
    CHECK_FALSE(test.world.HasBody(first));
    CHECK(test.world.HasBody(second));
    CHECK_FALSE(SomethingAt(test.world, oldSpot));
    CHECK(SomethingAt(test.world, newSpot));
}

TEST_CASE("After a scene is cleared, a reused entity gets a new body rather than the old one")
{
    TestScene test;
    AddFloor(test.scene);
    const ECS::Entity faller = AddBody(test.scene, {0.f, 20.f, 0.f}, Ball(0.5f, false));
    Step(test.world, 10);
    REQUIRE(test.world.GetBodyVelocity(faller).first.y < 0.f);

    // A level load: the scene starts over, and the first entity is {0, 0} again.
    test.scene.Clear();
    const glm::vec3 spot{0.f, 0.f, 8.f};
    const ECS::Entity reused = AddBody(test.scene, spot, Box({0.5f, 0.5f, 0.5f}, true));
    REQUIRE(reused == ECS::Entity{0, 0});

    test.world.Reconcile();
    CHECK(test.world.HasBody(reused));
    CHECK(test.world.GetBodyVelocity(reused).first == glm::vec3(0.f));
    CHECK(SomethingAt(test.world, spot));
    CHECK_FALSE(SomethingAt(test.world, {0.f, 10.f, 0.f}));
}

TEST_CASE("An entity destroyed and revived at its own handle starts over")
{
    // The editor's Stop, and an undo of a delete: the entity comes back at the
    // same handle, so only the removal says it is a new life.
    TestScene test;
    AddFloor(test.scene);
    const glm::vec3 spawn{0.f, 20.f, 0.f};
    const ECS::Entity faller = AddBody(test.scene, spawn, Ball(0.5f, false));
    const ECS::Entity walker = AddCharacter(test.scene, {5.f, 0.f, 0.f});
    for (int32_t i = 0; i < 30; ++i)
    {
        PhysicsTests::Drive(test.scene, walker, {3.f, 0.f, 0.f}, /*jump=*/ false);
        Step(test.world);
    }
    REQUIRE(test.world.GetBodyVelocity(faller).first.y < 0.f);
    REQUIRE(PhysicsTests::StateOf(test.scene, walker).velocity.x > 0.f);

    for (const ECS::Entity entity : {faller, walker})
    {
        test.scene.Destroy(entity);
    }
    test.scene.FlushDestroyed();
    test.scene.ReviveAt(faller);
    test.scene.ReviveAt(walker);
    REQUIRE(test.scene.Add(faller, ECS::Transform{.position = spawn}) != nullptr);
    REQUIRE(test.scene.Add(faller, Ball(0.5f, false).collider) != nullptr);
    REQUIRE(test.scene.Add(faller, Physics::RigidBody{}) != nullptr);
    REQUIRE(test.scene.Add(walker, ECS::Transform{.position = {5.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(test.scene.Add(walker, Physics::Character{}) != nullptr);

    test.world.Reconcile();
    CHECK(test.world.GetBodyVelocity(faller).first == glm::vec3(0.f));
    CHECK(test.world.GetBodyPose(faller).position == spawn);
    CHECK(PhysicsTests::StateOf(test.scene, walker).velocity == glm::vec3(0.f));
}

TEST_CASE("Rebuild starts every body and character over from the scene")
{
    TestScene test;
    AddFloor(test.scene);
    const glm::vec3 spawn{0.f, 20.f, 0.f};
    const ECS::Entity faller = AddBody(test.scene, spawn, Ball(0.5f, false));
    const ECS::Entity walker = AddCharacter(test.scene, {5.f, 0.f, 0.f});
    for (int32_t i = 0; i < 30; ++i)
    {
        PhysicsTests::Drive(test.scene, walker, {3.f, 0.f, 0.f}, /*jump=*/ false);
        Step(test.world);
    }
    REQUIRE(PhysicsTests::StateOf(test.scene, walker).velocity.x > 0.f);

    // Put back where it started, as a restore would, without the world being
    // told anything else.
    test.scene.GetMut<ECS::Transform>(faller)->position = spawn;
    test.world.Rebuild();

    CHECK(test.world.HasBody(faller));
    CHECK(test.world.HasBody(walker));
    CHECK(test.world.GetBodyVelocity(faller).first == glm::vec3(0.f));
    CHECK(test.world.GetBodyPose(faller).position == spawn);
    CHECK(PhysicsTests::StateOf(test.scene, walker).velocity == glm::vec3(0.f));
}

TEST_CASE("A world holds more than a thousand bodies by default")
{
    constexpr int32_t kBodies = 1100;
    constexpr float kSpacing = 2.f;
    TestScene test;
    std::vector<ECS::Entity> boxes;
    for (int32_t i = 0; i < kBodies; ++i)
    {
        boxes.push_back(AddBody(test.scene, {static_cast<float>(i) * kSpacing, 0.f, 0.f},
                                Box({0.5f, 0.5f, 0.5f}, true)));
    }
    test.world.Reconcile();

    for (const ECS::Entity box : boxes)
    {
        CHECK(test.world.HasBody(box));
    }
}

TEST_CASE("A body past the world's limit is not built, and the rest are")
{
    constexpr uint32_t kLimit = 4;
    TestScene test(kLimit);
    std::vector<ECS::Entity> boxes;
    for (uint32_t i = 0; i <= kLimit; ++i)
    {
        boxes.push_back(AddBody(test.scene, {static_cast<float>(i) * 2.f, 0.f, 0.f}, Box({0.5f, 0.5f, 0.5f}, true)));
    }
    test.world.Reconcile();

    for (uint32_t i = 0; i < kLimit; ++i)
    {
        CHECK(test.world.HasBody(boxes[i]));
    }
    CHECK_FALSE(test.world.HasBody(boxes[kLimit]));
}
