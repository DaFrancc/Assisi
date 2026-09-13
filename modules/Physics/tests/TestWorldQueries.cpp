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

#include <cstdint>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

using namespace Assisi;

namespace
{

constexpr Physics::Pose kUpright{glm::quat{1.f, 0.f, 0.f, 0.f}, glm::vec3{0.f}};

Physics::Pose At(glm::vec3 position)
{
    return Physics::Pose{glm::quat{1.f, 0.f, 0.f, 0.f}, position};
}

/// A unit box with an entity, so a hit has something to name.
ECS::Entity SpawnBox(ECS::Scene &scene, Physics::PhysicsWorld &world, glm::vec3 at, bool isStatic,
                     Physics::CollisionChannel channel  = Physics::CollisionChannel::World,
                     std::uint32_t             collides = Physics::AllChannels)
{
    const ECS::Entity entity = scene.Create();
    ECS::Transform *transform = scene.Add<ECS::Transform>(entity);
    REQUIRE(transform != nullptr);
    transform->position = at;

    Physics::RigidBodyDescriptor descriptor{};
    descriptor.halfExtents  = {0.5f, 0.5f, 0.5f};
    descriptor.isStatic     = isStatic;
    descriptor.channel      = channel;
    descriptor.collidesWith = collides;
    REQUIRE(scene.Add<Physics::RigidBodyDescriptor>(entity, descriptor) != nullptr);

    (void)world.AddBodyFromDescriptor(scene, entity, *transform, descriptor);
    return entity;
}

constexpr Physics::PhysicsWorld::ColliderShapeDesc kUnitBox{.shape       = Physics::ColliderShape::Box,
                                                            .halfExtents = {0.5f, 0.5f, 0.5f}};

} // namespace

TEST_CASE("CastRay reports the nearest body along the sweep")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world;

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
    Physics::PhysicsWorld world;
    (void)SpawnBox(scene, world, {5.f, 0.f, 0.f}, /*isStatic=*/ true);

    // Aimed the other way.
    CHECK_FALSE(world.CastRay({0.f, 0.f, 0.f}, {-20.f, 0.f, 0.f}, {}, ECS::NullEntity).has_value());

    // Long enough to reach, but past it.
    CHECK_FALSE(world.CastRay({0.f, 20.f, 0.f}, {20.f, 0.f, 0.f}, {}, ECS::NullEntity).has_value());
}

TEST_CASE("CastRay refuses a zero sweep instead of dividing by its length")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world;
    (void)SpawnBox(scene, world, {0.f, 0.f, 0.f}, /*isStatic=*/ true);

    // The origin is inside the box, so this is the case most likely to return a
    // hit at distance 0 by accident rather than the nothing a zero-length query
    // means. Normalizing it would also produce NaNs and poison every fraction.
    CHECK_FALSE(world.CastRay({0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, {}, ECS::NullEntity).has_value());
}

TEST_CASE("A ray starting inside a body hits it, and ignore is how you skip it")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world;

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
    Physics::PhysicsWorld world;

    // Visible to everything, on the World channel.
    (void)SpawnBox(scene, world, {5.f, 0.f, 0.f}, /*isStatic=*/ true);

    const Physics::CollisionFilter seeing{Physics::AllChannels, Physics::CollisionChannel::Visibility};
    REQUIRE(world.CastRay({0.f, 0.f, 0.f}, {20.f, 0.f, 0.f}, seeing, ECS::NullEntity).has_value());

    // The query refusing the body's channel.
    const std::uint32_t withoutWorld =
        Physics::AllChannels & ~(1u << static_cast<std::uint32_t>(Physics::CollisionChannel::World));
    const Physics::CollisionFilter blind{withoutWorld, Physics::CollisionChannel::Visibility};
    CHECK_FALSE(world.CastRay({0.f, 0.f, 0.f}, {20.f, 0.f, 0.f}, blind, ECS::NullEntity).has_value());
}

TEST_CASE("A body refusing the query's channel is not found either")
{
    // The other direction of the same rule, which a one-sided implementation
    // would get wrong while passing every test above: a pane of glass declines to
    // be seen, and a line-of-sight ray passes through it.
    ECS::Scene scene;
    Physics::PhysicsWorld world;

    const std::uint32_t unseen =
        Physics::AllChannels & ~(1u << static_cast<std::uint32_t>(Physics::CollisionChannel::Visibility));
    (void)SpawnBox(scene, world, {5.f, 0.f, 0.f}, /*isStatic=*/ true, Physics::CollisionChannel::World,
                   unseen);

    const Physics::CollisionFilter seeing{Physics::AllChannels, Physics::CollisionChannel::Visibility};
    CHECK_FALSE(world.CastRay({0.f, 0.f, 0.f}, {20.f, 0.f, 0.f}, seeing, ECS::NullEntity).has_value());

    // Still solid to everything else, so the mask narrowed one channel rather than
    // quietly removing the body from the world.
    const Physics::CollisionFilter ordinary{Physics::AllChannels, Physics::CollisionChannel::World};
    CHECK(world.CastRay({0.f, 0.f, 0.f}, {20.f, 0.f, 0.f}, ordinary, ECS::NullEntity).has_value());
}

TEST_CASE("A hit on a body with no entity reports NullEntity rather than a wrong handle")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world;

    // The raw AddBody knows no entity, so there is nothing truthful to name.
    (void)world.AddBody(At({5.f, 0.f, 0.f}), kUnitBox, Physics::BodyMotion::Static, {});

    const auto hit = world.CastRay({0.f, 0.f, 0.f}, {20.f, 0.f, 0.f}, {}, ECS::NullEntity);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == ECS::NullEntity);
}

TEST_CASE("CastShape stops a swept volume at the surface it meets")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world;

    const ECS::Entity floor = SpawnBox(scene, world, {0.f, 0.f, 0.f}, /*isStatic=*/ true);

    // A unit box dropped from 5 m: the two half-extents mean they meet with 4 m of
    // travel, not 5.
    const auto hit = world.CastShape(kUnitBox, At({0.f, 5.f, 0.f}), {0.f, -10.f, 0.f}, {},
                                     ECS::NullEntity);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == floor);
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
    Physics::PhysicsWorld world;

    (void)SpawnBox(scene, world, {5.f, 0.9f, 0.f}, /*isStatic=*/ true);
    (void)SpawnBox(scene, world, {5.f, -0.9f, 0.f}, /*isStatic=*/ true);

    CHECK_FALSE(world.CastRay({0.f, 0.f, 0.f}, {20.f, 0.f, 0.f}, {}, ECS::NullEntity).has_value());
    CHECK(world.CastShape(kUnitBox, kUpright, {20.f, 0.f, 0.f}, {}, ECS::NullEntity).has_value());
}

TEST_CASE("Overlap lists what a shape encloses, statics included, once each")
{
    // The question a sensor cannot answer: static scenery never generates a
    // contact, so the only way to hear about it is to ask.
    ECS::Scene scene;
    Physics::PhysicsWorld world;

    const ECS::Entity ground = SpawnBox(scene, world, {0.f, 0.f, 0.f}, /*isStatic=*/ true);
    const ECS::Entity crate  = SpawnBox(scene, world, {0.5f, 0.f, 0.f}, /*isStatic=*/ false);
    const ECS::Entity far    = SpawnBox(scene, world, {40.f, 0.f, 0.f}, /*isStatic=*/ true);

    const Physics::PhysicsWorld::ColliderShapeDesc room{.shape       = Physics::ColliderShape::Box,
                                                        .halfExtents = {3.f, 3.f, 3.f}};
    const std::vector<ECS::Entity> inside = world.Overlap(room, kUpright, {}, ECS::NullEntity);

    CHECK(std::find(inside.begin(), inside.end(), ground) != inside.end());
    CHECK(std::find(inside.begin(), inside.end(), crate) != inside.end());
    CHECK(std::find(inside.begin(), inside.end(), far) == inside.end());
    CHECK(inside.size() == 2u);
}

TEST_CASE("Overlap honours the ignored entity")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world;

    const ECS::Entity self  = SpawnBox(scene, world, {0.f, 0.f, 0.f}, /*isStatic=*/ false);
    const ECS::Entity other = SpawnBox(scene, world, {0.5f, 0.f, 0.f}, /*isStatic=*/ false);

    const Physics::PhysicsWorld::ColliderShapeDesc room{.shape       = Physics::ColliderShape::Box,
                                                        .halfExtents = {3.f, 3.f, 3.f}};
    const std::vector<ECS::Entity> inside = world.Overlap(room, kUpright, {}, self);

    CHECK(std::find(inside.begin(), inside.end(), self) == inside.end());
    CHECK(std::find(inside.begin(), inside.end(), other) != inside.end());
}
