/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestFollowers.cpp
/// @brief Colliders that ride along with a character or a body as bodies of
/// their own: a character's hitboxes, a trigger on a ball, a part set to
/// ColliderAttach::Body.
///
/// A follower is answered for by its owner — a hit names the owner as its
/// entity and the follower as its piece — and never touches its own owner.

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>

#include <Assisi/Core/Bitmask.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

#include "PhysicsTestScene.hpp"

using namespace Assisi;
using PhysicsTests::AddBody;
using PhysicsTests::AddCharacter;
using PhysicsTests::AddChildCollider;
using PhysicsTests::AddFloor;
using PhysicsTests::Ball;
using PhysicsTests::Box;
using PhysicsTests::Drive;
using PhysicsTests::Step;
using PhysicsTests::TestScene;

namespace
{

/// Where a character's head hitbox sits above its feet, and its half size.
constexpr float kHeadHeight = 1.5f;
constexpr float kHeadHalfExtent = 0.2f;

/// How far above a body a test ray starts, and how far down it reaches.
constexpr float kRayHeight = 10.f;
constexpr float kRayLength = 20.f;

using ChannelMask = Core::Bitmask<Physics::CollisionChannel>;

/// A box on the Hitbox channel, the size of a head.
Physics::Collider Head()
{
    Physics::Collider collider;
    collider.shape = Physics::ColliderShape::Box;
    collider.halfExtents = glm::vec3(kHeadHalfExtent);
    collider.channel = Physics::CollisionChannel::Hitbox;
    return collider;
}

/// A shot that finds only @p channel.
Physics::CollisionFilter ShotAt(Physics::CollisionChannel channel)
{
    return Physics::CollisionFilter{ChannelMask::Of(channel), Physics::CollisionChannel::Visibility};
}

/// What a ray straight down through @p x, @p z finds on @p filter.
std::optional<Physics::QueryHit> RayDown(const Physics::PhysicsWorld &world, glm::vec3 above,
                                         Physics::CollisionFilter filter, ECS::Entity ignore)
{
    return world.CastRay(glm::vec3(above.x, kRayHeight, above.z), glm::vec3(0.f, -kRayLength, 0.f), filter, ignore);
}

glm::vec3 PositionOf(const ECS::Scene &scene, ECS::Entity entity)
{
    return scene.Get<ECS::Transform>(entity)->position;
}

} // namespace

TEST_CASE("A character's hitbox follows it without pushing it, and shots find it by channel")
{
    TestScene test;
    (void)AddFloor(test.scene);
    const ECS::Entity hit = AddCharacter(test.scene, glm::vec3(0.f));
    const ECS::Entity bare = AddCharacter(test.scene, glm::vec3(0.f, 0.f, 5.f));
    const ECS::Entity head = AddChildCollider(test.scene, hit, glm::vec3(0.f, kHeadHeight, 0.f), Head());

    constexpr int32_t kWalkSteps = 60;
    const glm::vec3 walk(2.f, 0.f, 0.f);
    for (int32_t i = 0; i < kWalkSteps; ++i)
    {
        Drive(test.scene, hit, walk, false);
        Drive(test.scene, bare, walk, false);
        Step(test.world);
    }

    // The head sits inside the capsule; a sweep that met it would stop dead
    // or shove the character, and the two would no longer walk alike.
    const glm::vec3 walked = PositionOf(test.scene, hit);
    CHECK(walked.x > 1.f);
    CHECK(walked.x == doctest::Approx(PositionOf(test.scene, bare).x).epsilon(1e-3));

    CHECK_FALSE(test.world.HasBody(head));
    CHECK(test.world.BodyOf(head) == hit);

    SUBCASE("A Hitbox-only shot finds the head where the character now is")
    {
        const std::optional<Physics::QueryHit> shot =
            RayDown(test.world, walked, ShotAt(Physics::CollisionChannel::Hitbox), ECS::NullEntity);
        REQUIRE(shot.has_value());
        CHECK(shot->entity == hit);
        CHECK(shot->piece == head);
        CHECK(shot->position.y == doctest::Approx(walked.y + kHeadHeight + kHeadHalfExtent).epsilon(0.01));
    }

    SUBCASE("A Character-only shot finds the capsule")
    {
        const std::optional<Physics::QueryHit> shot =
            RayDown(test.world, walked, ShotAt(Physics::CollisionChannel::Character), ECS::NullEntity);
        REQUIRE(shot.has_value());
        CHECK(shot->entity == hit);
        CHECK(shot->piece == hit);
    }

    SUBCASE("A shot told to ignore the character skips its hitboxes too")
    {
        CHECK_FALSE(RayDown(test.world, walked, ShotAt(Physics::CollisionChannel::Hitbox), hit).has_value());
    }
}

TEST_CASE("A follower's own mask decides whether it stops a falling crate")
{
    TestScene test;
    (void)AddFloor(test.scene);
    const ECS::Entity character = AddCharacter(test.scene, glm::vec3(0.f));

    constexpr float kShelfHeight = 3.f;
    Physics::Collider shelf;
    shelf.shape = Physics::ColliderShape::Box;
    shelf.halfExtents = glm::vec3(1.f, 0.1f, 1.f);

    bool stops = true;
    SUBCASE("Colliding with World, it holds the crate")
    {
        stops = true;
    }
    SUBCASE("Without World in its mask, the crate falls through it")
    {
        shelf.collidesWith = Physics::AllChannels.Without(Physics::CollisionChannel::World);
        stops = false;
    }
    (void)AddChildCollider(test.scene, character, glm::vec3(0.f, kShelfHeight, 0.f), shelf);

    const ECS::Entity crate =
        AddBody(test.scene, glm::vec3(0.f, kShelfHeight + 2.f, 0.f), Box(glm::vec3(0.25f), /*isStatic=*/ false));
    constexpr int32_t kFallSteps = 120;
    Step(test.world, kFallSteps);

    const float crateHeight = PositionOf(test.scene, crate).y;
    if (stops)
    {
        CHECK(crateHeight == doctest::Approx(kShelfHeight + 0.1f + 0.25f).epsilon(0.02));
    }
    else
    {
        CHECK(crateHeight < kShelfHeight);
    }
}

TEST_CASE("A trigger riding a ball reports what enters it for the ball, and never the ball itself")
{
    TestScene test;
    (void)AddFloor(test.scene);
    const ECS::Entity ball = AddBody(test.scene, glm::vec3(0.f, 0.5f, 0.f), Ball(0.5f, /*isStatic=*/ false));

    constexpr float kReach = 1.5f;
    Physics::Collider pickup;
    pickup.shape = Physics::ColliderShape::Sphere;
    pickup.radius = kReach;
    pickup.channel = Physics::CollisionChannel::Trigger;
    const ECS::Entity trigger = AddChildCollider(test.scene, ball, glm::vec3(0.f), pickup);

    const ECS::Entity visitor =
        AddBody(test.scene, glm::vec3(1.f, 4.f, 0.f), Box(glm::vec3(0.2f), /*isStatic=*/ false));

    bool visitorEntered = false;
    bool sawItself = false;
    constexpr int32_t kSteps = 120;
    for (int32_t i = 0; i < kSteps; ++i)
    {
        Step(test.world);
        for (const Physics::ContactEvent &event : test.world.ContactEvents())
        {
            if (event.entity == ball && event.other == ball)
            {
                sawItself = true;
            }
            if (event.entity == ball && event.other == visitor && event.phase == Physics::ContactPhase::Enter &&
                event.piece == trigger && event.sensor)
            {
                visitorEntered = true;
            }
        }
    }

    CHECK(visitorEntered);
    CHECK_FALSE(sawItself);

    // The ball itself is solid: the sensor flag belongs to the trigger's own
    // body, not the ball's.
    CHECK(PositionOf(test.scene, ball).y == doctest::Approx(0.5f).epsilon(0.02));
    CHECK_FALSE(test.world.HasBody(trigger));
    CHECK(test.world.BodyOf(trigger) == ball);

    // It goes where the ball goes.
    test.world.Teleport(ball, Physics::Pose{glm::quat(1.f, 0.f, 0.f, 0.f), glm::vec3(20.f, 0.5f, 0.f)});
    Step(test.world);
    const std::optional<Physics::QueryHit> found =
        RayDown(test.world, PositionOf(test.scene, ball), ShotAt(Physics::CollisionChannel::Trigger), ECS::NullEntity);
    REQUIRE(found.has_value());
    CHECK(found->piece == trigger);
    CHECK(found->entity == ball);
}

TEST_CASE("A collider set to ride along is its own body that adds no mass and never pushes its owner")
{
    TestScene test;
    const ECS::Entity crate = AddBody(test.scene, glm::vec3(0.f), Box(glm::vec3(0.5f), /*isStatic=*/ false));
    test.scene.GetMut<Physics::RigidBody>(crate)->gravityScale = 0.f;

    Physics::Collider rider;
    rider.shape = Physics::ColliderShape::Box;
    rider.halfExtents = glm::vec3(0.5f);
    rider.attach = Physics::ColliderAttach::Body;
    // Overlapping its owner: anything but the owner rule would shove the crate.
    const ECS::Entity overlap = AddChildCollider(test.scene, crate, glm::vec3(0.f, 0.5f, 0.f), rider);
    const ECS::Entity above = AddChildCollider(test.scene, crate, glm::vec3(0.f, 3.f, 0.f), rider);

    test.world.Reconcile();
    CHECK_FALSE(test.world.HasBody(overlap));
    CHECK(test.world.BodyOf(overlap) == crate);
    CHECK(test.world.Mass(crate) == doctest::Approx(1000.f).epsilon(1e-3));

    constexpr int32_t kSteps = 30;
    Step(test.world, kSteps);
    CHECK(glm::length(PositionOf(test.scene, crate)) < 1e-3f);

    test.world.Teleport(crate, Physics::Pose{glm::quat(1.f, 0.f, 0.f, 0.f), glm::vec3(5.f, 0.f, 0.f)});
    Step(test.world);
    const std::optional<Physics::QueryHit> found =
        RayDown(test.world, glm::vec3(5.f, 0.f, 0.f), Physics::CollisionFilter{}, ECS::NullEntity);
    REQUIRE(found.has_value());
    CHECK(found->piece == above);
    CHECK(found->entity == crate);
}
