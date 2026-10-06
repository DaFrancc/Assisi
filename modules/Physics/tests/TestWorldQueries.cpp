/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestWorldQueries.cpp
/// @brief PhysicsWorld's ray cast, shape cast and overlap.
///
/// Nothing here calls Update(). That is deliberate and load-bearing: a query has
/// to see a body the moment it is added, because the editor picks at objects it
/// has just placed and a character controller casts on the frame it spawns. If
/// Jolt's broad phase ever stopped taking new bodies synchronously, every case in
/// this file would fail rather than one somewhere else failing much later.
///
/// The filter cases matter more than the geometry ones. A cast that reports the
/// wrong distance is obvious the first time anyone uses it; a cast that quietly
/// ignores its filter is a character controller walking through the one wall it
/// was asked to respect.

#include <doctest/doctest.h>

#include <algorithm>
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

namespace
{

constexpr Physics::Pose kUpright{glm::quat{1.f, 0.f, 0.f, 0.f}, glm::vec3{0.f}};

Physics::Pose At(glm::vec3 position)
{
    return Physics::Pose{glm::quat{1.f, 0.f, 0.f, 0.f}, position};
}

/// A unit box, reconciled into @p world so a query can find it at once.
ECS::Entity SpawnBox(ECS::Scene &scene, Physics::PhysicsWorld &world, glm::vec3 at, bool isStatic,
                     Physics::CollisionFilter filter = Physics::CollisionFilter{})
{
    const PhysicsTests::BodySpec descriptor =
        PhysicsTests::WithFilter(PhysicsTests::Box({0.5f, 0.5f, 0.5f}, isStatic), filter);
    const ECS::Entity entity = PhysicsTests::AddBody(scene, at, descriptor);
    world.Reconcile();
    return entity;
}

/// A box collider of @p halfExtents, asking as an ordinary World body.
Physics::Collider BoxCollider(glm::vec3 halfExtents)
{
    Physics::Collider collider;
    collider.shape = Physics::ColliderShape::Box;
    collider.halfExtents = halfExtents;
    return collider;
}

const Physics::Collider kUnitBox = BoxCollider({0.5f, 0.5f, 0.5f});

bool Contains(const std::vector<Physics::QueryHit> &hits, ECS::Entity entity)
{
    return std::find_if(hits.begin(), hits.end(),
                        [entity](const Physics::QueryHit &hit) { return hit.entity == entity; }) != hits.end();
}

} // namespace

TEST_CASE("CastRay reports the nearest body along the sweep")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};

    const ECS::Entity near = SpawnBox(scene, world, {5.f, 0.f, 0.f}, /*isStatic=*/ true);
    (void)SpawnBox(scene, world, {10.f, 0.f, 0.f}, /*isStatic=*/ true);

    const auto hit = world.CastRay({0.f, 0.f, 0.f}, {20.f, 0.f, 0.f}, {}, ECS::NullEntity);
    REQUIRE(hit.has_value());

    // The near box's face, not the far box and not the near box's centre.
    CHECK(hit->entity == near);
    CHECK(hit->distance == doctest::Approx(4.5f).epsilon(0.02));
    CHECK(hit->position.x == doctest::Approx(4.5f).epsilon(0.02));

    // Pointing back down the ray, out of the surface it struck.
    CHECK(hit->normal.x < -0.9f);
}

TEST_CASE("CastRay finds nothing when it reaches nothing")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    (void)SpawnBox(scene, world, {5.f, 0.f, 0.f}, /*isStatic=*/ true);

    // Aimed the other way.
    CHECK_FALSE(world.CastRay({0.f, 0.f, 0.f}, {-20.f, 0.f, 0.f}, {}, ECS::NullEntity).has_value());

    // Long enough to reach, but past it.
    CHECK_FALSE(world.CastRay({0.f, 20.f, 0.f}, {20.f, 0.f, 0.f}, {}, ECS::NullEntity).has_value());
}

TEST_CASE("CastRay refuses a zero sweep instead of dividing by its length")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    (void)SpawnBox(scene, world, {0.f, 0.f, 0.f}, /*isStatic=*/ true);

    // The origin is inside the box, so this is the case most likely to return a
    // hit at distance 0 by accident rather than the nothing a zero-length query
    // means. Normalizing it would also produce NaNs and poison every fraction.
    CHECK_FALSE(world.CastRay({0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, {}, ECS::NullEntity).has_value());
}

TEST_CASE("A ray starting inside a body hits it, and ignore is how you skip it")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};

    const ECS::Entity self   = SpawnBox(scene, world, {0.f, 0.f, 0.f}, /*isStatic=*/ true);
    const ECS::Entity beyond = SpawnBox(scene, world, {5.f, 0.f, 0.f}, /*isStatic=*/ true);

    // Solid, so a cast from inside reports its own body at no distance at all.
    const auto self_hit = world.CastRay({0.f, 0.f, 0.f}, {20.f, 0.f, 0.f}, {}, ECS::NullEntity);
    REQUIRE(self_hit.has_value());
    CHECK(self_hit->entity == self);
    CHECK(self_hit->distance == doctest::Approx(0.f));

    // Which is why a caster names itself: this is the character-controller case,
    // and without it every self-cast stops at the caster.
    const auto skipped = world.CastRay({0.f, 0.f, 0.f}, {20.f, 0.f, 0.f}, {}, self);
    REQUIRE(skipped.has_value());
    CHECK(skipped->entity == beyond);
    CHECK(skipped->distance == doctest::Approx(4.5f).epsilon(0.02));
}

TEST_CASE("A query and a body must each admit the other's channel")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};

    // Visible to everything, on the World channel.
    (void)SpawnBox(scene, world, {5.f, 0.f, 0.f}, /*isStatic=*/ true);

    const Physics::CollisionFilter seeing{Physics::AllChannels, Physics::CollisionChannel::Visibility};
    REQUIRE(world.CastRay({0.f, 0.f, 0.f}, {20.f, 0.f, 0.f}, seeing, ECS::NullEntity).has_value());

    // The query refusing the body's channel.
    const auto withoutWorld = Physics::AllChannels.Without(Physics::CollisionChannel::World);
    const Physics::CollisionFilter blind{withoutWorld, Physics::CollisionChannel::Visibility};
    CHECK_FALSE(world.CastRay({0.f, 0.f, 0.f}, {20.f, 0.f, 0.f}, blind, ECS::NullEntity).has_value());
}

TEST_CASE("A body refusing the query's channel is not found either")
{
    // The other direction of the same rule, which a one-sided implementation
    // would get wrong while passing every test above: a pane of glass declines to
    // be seen, and a line-of-sight ray passes through it.
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};

    const auto unseen = Physics::AllChannels.Without(Physics::CollisionChannel::Visibility);
    (void)SpawnBox(scene, world, {5.f, 0.f, 0.f}, /*isStatic=*/ true,
                   Physics::CollisionFilter{unseen, Physics::CollisionChannel::World});

    const Physics::CollisionFilter seeing{Physics::AllChannels, Physics::CollisionChannel::Visibility};
    CHECK_FALSE(world.CastRay({0.f, 0.f, 0.f}, {20.f, 0.f, 0.f}, seeing, ECS::NullEntity).has_value());

    // Still solid to everything else, so the mask narrowed one channel rather than
    // quietly removing the body from the world.
    const Physics::CollisionFilter ordinary{Physics::AllChannels, Physics::CollisionChannel::World};
    CHECK(world.CastRay({0.f, 0.f, 0.f}, {20.f, 0.f, 0.f}, ordinary, ECS::NullEntity).has_value());
}

TEST_CASE("CastShape stops a swept volume at the surface it meets")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};

    const ECS::Entity floor = SpawnBox(scene, world, {0.f, 0.f, 0.f}, /*isStatic=*/ true);

    // A unit box dropped from 5 m: the two half-extents mean they meet with 4 m of
    // travel, not 5.
    const std::optional<Physics::QueryHit> hit =
        world.CastShape(kUnitBox, At({0.f, 5.f, 0.f}), {0.f, -10.f, 0.f}, ECS::NullEntity);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == floor);
    CHECK(hit->piece == floor);
    CHECK(hit->distance == doctest::Approx(4.f).epsilon(0.02));

    // Out of the floor's top face, back towards where the sweep came from. The
    // sign here is the one thing a penetration axis is easy to get backwards on,
    // and a character controller would walk into the ground if it were.
    CHECK(hit->normal.y > 0.9f);
}

TEST_CASE("A shape cast is wider than a ray, and catches what a ray slips past")
{
    // What makes CastShape worth having. The ray threads the gap between two
    // boxes; the box being swept along the same line cannot.
    //
    // The margins are the point of the case. Each box is a unit cube, so at
    // y = ±0.9 the gap between their faces is 0.8 — wide enough for a ray along
    // y = 0, too narrow for the 1.0-tall box swept down the same line.
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};

    (void)SpawnBox(scene, world, {5.f, 0.9f, 0.f}, /*isStatic=*/ true);
    (void)SpawnBox(scene, world, {5.f, -0.9f, 0.f}, /*isStatic=*/ true);

    CHECK_FALSE(world.CastRay({0.f, 0.f, 0.f}, {20.f, 0.f, 0.f}, {}, ECS::NullEntity).has_value());
    CHECK(world.CastShape(kUnitBox, kUpright, {20.f, 0.f, 0.f}, ECS::NullEntity).has_value());
}

TEST_CASE("A shape query asks with its collider's own channel and mask")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    (void)SpawnBox(scene, world, {5.f, 0.f, 0.f}, /*isStatic=*/ true);

    REQUIRE(world.CastShape(kUnitBox, kUpright, {20.f, 0.f, 0.f}, ECS::NullEntity).has_value());

    // A collider that does not collide with World passes the World box by, as a
    // body with that collider would.
    Physics::Collider blind = kUnitBox;
    blind.collidesWith = Physics::AllChannels.Without(Physics::CollisionChannel::World);
    CHECK_FALSE(world.CastShape(blind, kUpright, {20.f, 0.f, 0.f}, ECS::NullEntity).has_value());
    CHECK_FALSE(world.Overlap(blind, At({5.f, 0.f, 0.f}), ECS::NullEntity).has_value());
}

TEST_CASE("A shape query places its collider at the collider's offset")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};
    const ECS::Entity box = SpawnBox(scene, world, {0.f, 3.f, 0.f}, /*isStatic=*/ true);

    // Held at the origin, the shape itself is three metres up, inside the box.
    Physics::Collider raised = kUnitBox;
    raised.offsetPosition = {0.f, 3.f, 0.f};
    const std::optional<Physics::QueryHit> hit = world.Overlap(raised, kUpright, ECS::NullEntity);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == box);
    CHECK_FALSE(world.Overlap(kUnitBox, kUpright, ECS::NullEntity).has_value());
}

TEST_CASE("CastRayAll reports every body along the sweep, nearest first")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};

    const ECS::Entity far = SpawnBox(scene, world, {10.f, 0.f, 0.f}, /*isStatic=*/ true);
    const ECS::Entity near = SpawnBox(scene, world, {5.f, 0.f, 0.f}, /*isStatic=*/ true);

    std::vector<Physics::QueryHit> hits;
    world.CastRayAll({0.f, 0.f, 0.f}, {20.f, 0.f, 0.f}, {}, ECS::NullEntity, hits);
    REQUIRE(hits.size() == 2u);
    CHECK(hits[0].entity == near);
    CHECK(hits[0].distance == doctest::Approx(4.5f).epsilon(0.02));
    CHECK(hits[0].normal.x < -0.9f);
    CHECK(hits[1].entity == far);
    CHECK(hits[1].distance == doctest::Approx(9.5f).epsilon(0.02));

    // The vector is the caller's, refilled rather than appended to.
    world.CastRayAll({0.f, 0.f, 0.f}, {20.f, 0.f, 0.f}, {}, ECS::NullEntity, hits);
    CHECK(hits.size() == 2u);
    world.CastRayAll({0.f, 0.f, 0.f}, {-20.f, 0.f, 0.f}, {}, ECS::NullEntity, hits);
    CHECK(hits.empty());
}

TEST_CASE("CastShapeAll reports every body the swept collider meets, once each, nearest first")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};

    const ECS::Entity near = SpawnBox(scene, world, {5.f, 0.f, 0.f}, /*isStatic=*/ true);
    const ECS::Entity far = SpawnBox(scene, world, {10.f, 0.f, 0.f}, /*isStatic=*/ true);

    std::vector<Physics::QueryHit> hits;
    world.CastShapeAll(kUnitBox, kUpright, {20.f, 0.f, 0.f}, ECS::NullEntity, hits);
    REQUIRE(hits.size() == 2u);
    CHECK(hits[0].entity == near);
    CHECK(hits[0].distance == doctest::Approx(4.f).epsilon(0.02));
    CHECK(hits[1].entity == far);
    CHECK(hits[1].distance == doctest::Approx(9.f).epsilon(0.02));
}

TEST_CASE("OverlapAll lists what a collider encloses, statics included, once each")
{
    // The question a sensor cannot answer: static scenery never generates a
    // contact, so the only way to hear about it is to ask.
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};

    const ECS::Entity ground = SpawnBox(scene, world, {0.f, 0.f, 0.f}, /*isStatic=*/ true);
    const ECS::Entity crate = SpawnBox(scene, world, {0.5f, 0.f, 0.f}, /*isStatic=*/ false);
    const ECS::Entity edge = SpawnBox(scene, world, {3.f, 0.f, 0.f}, /*isStatic=*/ true);
    const ECS::Entity far = SpawnBox(scene, world, {40.f, 0.f, 0.f}, /*isStatic=*/ true);

    std::vector<Physics::QueryHit> inside;
    world.OverlapAll(BoxCollider({3.f, 3.f, 3.f}), kUpright, ECS::NullEntity, inside);

    CHECK(Contains(inside, ground));
    CHECK(Contains(inside, crate));
    CHECK(Contains(inside, edge));
    CHECK_FALSE(Contains(inside, far));
    REQUIRE(inside.size() == 3u);

    // Deepest first: the box only half inside comes last.
    CHECK(inside[2].entity == edge);
    CHECK(inside[0].distance >= inside[1].distance);
    CHECK(inside[1].distance >= inside[2].distance);
}

TEST_CASE("Overlap reports the body it overlaps most deeply")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};

    (void)SpawnBox(scene, world, {0.9f, 0.f, 0.f}, /*isStatic=*/ true);
    const ECS::Entity deep = SpawnBox(scene, world, {-0.2f, 0.f, 0.f}, /*isStatic=*/ true);

    const std::optional<Physics::QueryHit> hit = world.Overlap(kUnitBox, kUpright, ECS::NullEntity);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == deep);
    CHECK(hit->distance > 0.5f);
}

TEST_CASE("Overlap honours the ignored entity")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene, Assisi::Physics::NoCollisionAssets()};

    const ECS::Entity self = SpawnBox(scene, world, {0.f, 0.f, 0.f}, /*isStatic=*/ false);
    const ECS::Entity other = SpawnBox(scene, world, {0.5f, 0.f, 0.f}, /*isStatic=*/ false);

    std::vector<Physics::QueryHit> inside;
    world.OverlapAll(BoxCollider({3.f, 3.f, 3.f}), kUpright, self, inside);

    CHECK_FALSE(Contains(inside, self));
    CHECK(Contains(inside, other));
}
