/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCompoundBodies.cpp
/// @brief Colliders on child entities of a RigidBody are pieces of its body:
/// one shape, one mass, and hits that name the part that was struck.

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <vector>

#include <Assisi/Core/Bitmask.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/ColliderRole.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

#include "PhysicsTestScene.hpp"

using namespace Assisi;
using PhysicsTests::AddBody;
using PhysicsTests::AddChildCollider;
using PhysicsTests::AddFloor;
using PhysicsTests::Box;
using PhysicsTests::Step;
using PhysicsTests::TestScene;

namespace
{

/// The mass of a one-metre cube of water, which is what a default unit box
/// collider weighs.
constexpr float kUnitBoxMass = 1000.f;

/// Mass is read back through Jolt's inverse, so it rounds.
constexpr double kMassEpsilon = 1e-3;

/// How far above a body a test ray starts, and how far down it reaches.
constexpr float kRayHeight = 10.f;
constexpr float kRayLength = 20.f;

/// A box collider one metre on a side.
Physics::Collider UnitBox()
{
    Physics::Collider collider;
    collider.shape = Physics::ColliderShape::Box;
    collider.halfExtents = glm::vec3(0.5f);
    return collider;
}

/// A one-metre dynamic crate at @p position.
ECS::Entity AddCrate(ECS::Scene &scene, glm::vec3 position)
{
    return AddBody(scene, position, Box(glm::vec3(0.5f), /*isStatic=*/ false));
}

/// What a ray straight down through @p x, @p z hits first.
std::optional<Physics::QueryHit> RayDown(const Physics::PhysicsWorld &world, float x, float z)
{
    return world.CastRay(glm::vec3(x, kRayHeight, z), glm::vec3(0.f, -kRayLength, 0.f), Physics::CollisionFilter{},
                         ECS::NullEntity);
}

} // namespace

TEST_CASE("A hit on a piece names the piece and the body it belongs to")
{
    TestScene test;
    const ECS::Entity crate = AddCrate(test.scene, glm::vec3(0.f));
    const ECS::Entity handle = AddChildCollider(test.scene, crate, glm::vec3(0.f, 2.f, 0.f), UnitBox());
    test.world.Reconcile();

    const std::optional<Physics::QueryHit> onHandle = RayDown(test.world, 0.f, 0.f);
    REQUIRE(onHandle.has_value());
    CHECK(onHandle->entity == crate);
    CHECK(onHandle->piece == handle);
    CHECK(onHandle->position.y == doctest::Approx(2.5f).epsilon(0.01));

    const std::optional<Physics::QueryHit> onCrate =
        test.world.CastRay(glm::vec3(5.f, 0.f, 0.f), glm::vec3(-10.f, 0.f, 0.f), {}, ECS::NullEntity);
    REQUIRE(onCrate.has_value());
    CHECK(onCrate->entity == crate);
    CHECK(onCrate->piece == crate);

    CHECK_FALSE(test.world.HasBody(handle));
    CHECK(test.world.BodyOf(handle) == crate);
}

TEST_CASE("A compound body's mass adds up each piece's volume at its density")
{
    TestScene test;
    const ECS::Entity crate = AddCrate(test.scene, glm::vec3(0.f));
    Physics::Collider lead = UnitBox();
    lead.density = 2.f * Physics::kWaterDensity;
    const ECS::Entity base = AddChildCollider(test.scene, crate, glm::vec3(0.f, -1.f, 0.f), lead);
    test.world.Reconcile();

    CHECK(test.world.Mass(crate) == doctest::Approx(3.f * kUnitBoxMass).epsilon(kMassEpsilon));
    CHECK(test.world.Mass(base) == doctest::Approx(test.world.Mass(crate)));

    SUBCASE("RigidBody.mass overrides the total")
    {
        constexpr float kOverride = 10.f;
        test.scene.GetMut<Physics::RigidBody>(crate)->mass = kOverride;
        test.world.Reconcile();
        CHECK(test.world.Mass(crate) == doctest::Approx(kOverride).epsilon(kMassEpsilon));
    }
}

TEST_CASE("A compound body comes to rest on its lowest piece")
{
    TestScene test;
    const ECS::Entity floor = AddFloor(test.scene);
    const ECS::Entity crate = AddCrate(test.scene, glm::vec3(0.f, 4.f, 0.f));
    const ECS::Entity foot = AddChildCollider(test.scene, crate, glm::vec3(0.f, -1.5f, 0.f), UnitBox());

    bool footLanded = false;
    bool crateLanded = false;
    constexpr int32_t kSettleSteps = 240;
    for (int32_t i = 0; i < kSettleSteps; ++i)
    {
        Step(test.world);
        for (const Physics::ContactEvent &event : test.world.ContactEvents())
        {
            if (event.phase != Physics::ContactPhase::Enter || event.entity != crate || event.other != floor)
            {
                continue;
            }
            footLanded = footLanded || event.piece == foot;
            crateLanded = crateLanded || event.piece == crate;
        }
    }

    // The foot's half height above the floor, and the crate 1.5 m above that.
    const ECS::Transform &pose = *test.scene.Get<ECS::Transform>(crate);
    CHECK(pose.position.y == doctest::Approx(2.f).epsilon(0.02));
    CHECK(footLanded);
    CHECK_FALSE(crateLanded);
}

TEST_CASE("A RigidBody whose only colliders are on its children gets a body")
{
    TestScene test;
    (void)AddFloor(test.scene);
    const ECS::Entity owner = test.scene.Create();
    REQUIRE(test.scene.Add(owner, ECS::Transform{.position = {0.f, 3.f, 0.f}}) != nullptr);
    REQUIRE(test.scene.Add(owner, Physics::RigidBody{}) != nullptr);
    const ECS::Entity left = AddChildCollider(test.scene, owner, glm::vec3(-1.f, 0.f, 0.f), UnitBox());
    (void)AddChildCollider(test.scene, owner, glm::vec3(1.f, 0.f, 0.f), UnitBox());
    test.world.Reconcile();

    CHECK(test.world.HasBody(owner));
    CHECK_FALSE(test.world.HasBody(left));
    CHECK(test.world.BodyOf(left) == owner);
    CHECK(test.world.Mass(owner) == doctest::Approx(2.f * kUnitBoxMass).epsilon(kMassEpsilon));
    CHECK(test.world.Mass(left) == doctest::Approx(test.world.Mass(owner)));

    constexpr int32_t kFallSteps = 180;
    Step(test.world, kFallSteps);
    CHECK(test.scene.Get<ECS::Transform>(owner)->position.y == doctest::Approx(0.5f).epsilon(0.02));
}

TEST_CASE("Editing a piece's Transform, or one between it and its body, moves where hits land")
{
    TestScene test;
    const ECS::Entity crate = AddCrate(test.scene, glm::vec3(0.f));
    test.scene.GetMut<Physics::RigidBody>(crate)->gravityScale = 0.f;

    SUBCASE("The piece itself")
    {
        const ECS::Entity handle = AddChildCollider(test.scene, crate, glm::vec3(0.f, 2.f, 0.f), UnitBox());
        test.world.Reconcile();
        test.scene.GetMut<ECS::Transform>(handle)->position = glm::vec3(3.f, 0.f, 0.f);
        test.world.Reconcile();

        const std::optional<Physics::QueryHit> moved = RayDown(test.world, 3.f, 0.f);
        REQUIRE(moved.has_value());
        CHECK(moved->piece == handle);
        const std::optional<Physics::QueryHit> left = RayDown(test.world, 0.f, 0.f);
        REQUIRE(left.has_value());
        CHECK(left->piece == crate);
    }

    SUBCASE("An entity between the piece and its body")
    {
        const ECS::Entity arm = test.scene.Create();
        REQUIRE(test.scene.Add(arm, ECS::Transform{.position = {0.f, 2.f, 0.f}}) != nullptr);
        REQUIRE(test.scene.Add(arm, ECS::Parent{.parent = crate}) != nullptr);
        const ECS::Entity hand = AddChildCollider(test.scene, arm, glm::vec3(0.f), UnitBox());
        test.world.Reconcile();
        test.scene.GetMut<ECS::Transform>(arm)->position = glm::vec3(-3.f, 0.f, 0.f);
        test.world.Reconcile();

        const std::optional<Physics::QueryHit> moved = RayDown(test.world, -3.f, 0.f);
        REQUIRE(moved.has_value());
        CHECK(moved->piece == hand);
        CHECK(moved->entity == crate);
    }

    // The body stays where it was: moving a part recentres the shape's mass,
    // and the body must not jump by that amount.
    const Physics::Pose pose = test.world.GetBodyPose(crate);
    CHECK(glm::length(pose.position) < 1e-4f);
}

TEST_CASE("A piece moved to another body becomes part of that body")
{
    TestScene test;
    const ECS::Entity first = AddCrate(test.scene, glm::vec3(0.f));
    const ECS::Entity second = AddCrate(test.scene, glm::vec3(10.f, 0.f, 0.f));
    const ECS::Entity handle = AddChildCollider(test.scene, first, glm::vec3(0.f, 2.f, 0.f), UnitBox());
    test.world.Reconcile();
    REQUIRE(test.world.Mass(first) == doctest::Approx(2.f * kUnitBoxMass).epsilon(kMassEpsilon));

    test.scene.GetMut<ECS::Parent>(handle)->parent = second;
    test.world.Reconcile();

    const std::optional<Physics::QueryHit> onSecond = RayDown(test.world, 10.f, 0.f);
    REQUIRE(onSecond.has_value());
    CHECK(onSecond->entity == second);
    CHECK(onSecond->piece == handle);

    const std::optional<Physics::QueryHit> onFirst = RayDown(test.world, 0.f, 0.f);
    REQUIRE(onFirst.has_value());
    CHECK(onFirst->piece == first);

    CHECK(test.world.Mass(first) == doctest::Approx(kUnitBoxMass).epsilon(kMassEpsilon));
    CHECK(test.world.Mass(second) == doctest::Approx(2.f * kUnitBoxMass).epsilon(kMassEpsilon));
}

TEST_CASE("Removing a piece's Collider takes it out of its body")
{
    TestScene test;
    const ECS::Entity crate = AddCrate(test.scene, glm::vec3(0.f));
    const ECS::Entity handle = AddChildCollider(test.scene, crate, glm::vec3(3.f, 0.f, 0.f), UnitBox());
    test.world.Reconcile();
    REQUIRE(RayDown(test.world, 3.f, 0.f).has_value());

    (void)test.scene.Remove<Physics::Collider>(handle);
    test.world.Reconcile();

    CHECK_FALSE(RayDown(test.world, 3.f, 0.f).has_value());
    CHECK(test.world.Mass(crate) == doctest::Approx(kUnitBoxMass).epsilon(kMassEpsilon));
}

TEST_CASE("A disabled collider has no body and no piece, and is never hit")
{
    TestScene test;

    SUBCASE("A piece")
    {
        const ECS::Entity crate = AddCrate(test.scene, glm::vec3(0.f));
        const ECS::Entity handle = AddChildCollider(test.scene, crate, glm::vec3(3.f, 0.f, 0.f), UnitBox());
        test.scene.GetMut<Physics::Collider>(handle)->enabled = false;
        test.world.Reconcile();
        CHECK_FALSE(RayDown(test.world, 3.f, 0.f).has_value());
        CHECK(test.world.Mass(crate) == doctest::Approx(kUnitBoxMass).epsilon(kMassEpsilon));

        test.scene.GetMut<Physics::Collider>(handle)->enabled = true;
        test.world.Reconcile();
        CHECK(RayDown(test.world, 3.f, 0.f).has_value());
        CHECK(test.world.Mass(crate) == doctest::Approx(2.f * kUnitBoxMass).epsilon(kMassEpsilon));
    }

    SUBCASE("A static collider")
    {
        const ECS::Entity wall = AddBody(test.scene, glm::vec3(0.f), Box(glm::vec3(0.5f), /*isStatic=*/ true));
        test.scene.GetMut<Physics::Collider>(wall)->enabled = false;
        test.world.Reconcile();
        CHECK_FALSE(test.world.HasBody(wall));
        CHECK_FALSE(RayDown(test.world, 0.f, 0.f).has_value());

        test.scene.GetMut<Physics::Collider>(wall)->enabled = true;
        test.world.Reconcile();
        CHECK(test.world.HasBody(wall));
    }
}

TEST_CASE("IgnoreCollision lets two bodies pass through each other until it is undone")
{
    TestScene test;
    const ECS::Entity floor = AddFloor(test.scene);
    const ECS::Entity crate = AddCrate(test.scene, glm::vec3(0.f, 2.f, 0.f));
    constexpr int32_t kFallSteps = 120;

    SUBCASE("Named by the body")
    {
        test.world.IgnoreCollision(crate, floor);
        Step(test.world, kFallSteps);
        CHECK(test.scene.Get<ECS::Transform>(crate)->position.y < -1.f);

        test.world.IgnoreCollision(crate, floor, false);
        test.world.Teleport(crate, Physics::Pose{glm::quat(1.f, 0.f, 0.f, 0.f), glm::vec3(0.f, 2.f, 0.f)});
        Step(test.world, kFallSteps);
        CHECK(test.scene.Get<ECS::Transform>(crate)->position.y == doctest::Approx(0.5f).epsilon(0.02));
    }

    SUBCASE("Named by a piece, which stands for its whole body")
    {
        const ECS::Entity handle = AddChildCollider(test.scene, crate, glm::vec3(0.f, 1.f, 0.f), UnitBox());
        test.world.IgnoreCollision(handle, floor);
        Step(test.world, kFallSteps);
        CHECK(test.scene.Get<ECS::Transform>(crate)->position.y < -1.f);
    }
}

TEST_CASE("A collider's role comes from what is above it")
{
    ECS::Scene scene;
    const ECS::Entity crate = AddCrate(scene, glm::vec3(0.f));
    const ECS::Entity plain = scene.Create();
    REQUIRE(scene.Add(plain, ECS::Transform{}) != nullptr);
    const ECS::Entity character = PhysicsTests::AddCharacter(scene, glm::vec3(5.f, 0.f, 0.f));

    const ECS::Entity piece = AddChildCollider(scene, crate, glm::vec3(0.f, 2.f, 0.f), UnitBox());
    const ECS::Entity arm = scene.Create();
    REQUIRE(scene.Add(arm, ECS::Transform{}) != nullptr);
    REQUIRE(scene.Add(arm, ECS::Parent{.parent = crate}) != nullptr);
    const ECS::Entity deep = AddChildCollider(scene, arm, glm::vec3(0.f), UnitBox());
    const ECS::Entity onPlain = AddChildCollider(scene, plain, glm::vec3(0.f), UnitBox());
    const ECS::Entity hitbox = AddChildCollider(scene, character, glm::vec3(0.f, 1.5f, 0.f), UnitBox());

    Physics::Collider sensor = UnitBox();
    sensor.channel = Physics::CollisionChannel::Trigger;
    const ECS::Entity trigger = AddChildCollider(scene, crate, glm::vec3(0.f), sensor);

    Physics::Collider riding = UnitBox();
    riding.attach = Physics::ColliderAttach::Body;
    const ECS::Entity rider = AddChildCollider(scene, crate, glm::vec3(0.f), riding);

    using Physics::ColliderRole;
    using Physics::ResolveColliderPlacement;
    CHECK(ResolveColliderPlacement(scene, crate).role == ColliderRole::Own);
    CHECK(ResolveColliderPlacement(scene, crate).owner == crate);
    CHECK(ResolveColliderPlacement(scene, piece).role == ColliderRole::Piece);
    CHECK(ResolveColliderPlacement(scene, piece).owner == crate);
    CHECK(ResolveColliderPlacement(scene, deep).role == ColliderRole::Piece);
    CHECK(ResolveColliderPlacement(scene, deep).owner == crate);
    CHECK(ResolveColliderPlacement(scene, onPlain).role == ColliderRole::Static);
    CHECK(ResolveColliderPlacement(scene, onPlain).owner == onPlain);
    CHECK(ResolveColliderPlacement(scene, hitbox).role == ColliderRole::Follower);
    CHECK(ResolveColliderPlacement(scene, hitbox).owner == character);
    CHECK(ResolveColliderPlacement(scene, trigger).role == ColliderRole::Follower);
    CHECK(ResolveColliderPlacement(scene, trigger).owner == crate);
    CHECK(ResolveColliderPlacement(scene, rider).role == ColliderRole::Follower);
    CHECK(ResolveColliderPlacement(scene, rider).owner == crate);
}
