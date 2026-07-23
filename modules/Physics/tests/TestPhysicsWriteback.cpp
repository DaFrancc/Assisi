/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPhysicsWriteback.cpp
/// @brief Change detection on PhysicsWorld::InterpolateTransforms — the engine's
/// one system that writes ECS::Transform every frame from outside the ECS.
///
/// Transform is ACOMP(tracked), so the pose this writes must stamp a change tick:
/// PropagateTransforms's dirty-skip and network delta replication both decide
/// whether to act on `Scene::Changed<Transform>`. A writeback through a plain
/// Query yields a bare `Transform&` that cannot stamp, which reads as "the body
/// moved but nothing changed" — the world matrix goes stale and the wire silently
/// omits the entity. These cases pin the stamping down, and equally pin down that
/// bodies the writeback *skips* are not stamped (an over-stamp would replicate the
/// whole scene's transforms every tick).

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

constexpr float kStep = 1.f / 60.f;

/// A sphere is the cheapest shape with no orientation subtleties.
constexpr Physics::PhysicsWorld::ColliderShapeDesc kBall{.shape  = Physics::ColliderShape::Sphere,
                                                         .radius = 0.5f};

/// Spawns an entity carrying a Transform plus a live Jolt body of @p motion at
/// @p position, and steps the world twice so both interpolation snapshots exist.
ECS::Entity SpawnSimulatedBody(ECS::Scene &scene, Physics::PhysicsWorld &world, glm::vec3 position,
                               Physics::BodyMotion motion)
{
    const ECS::Entity e = scene.Create();
    REQUIRE(scene.Add(e, ECS::Transform{.position = position}) != nullptr);

    const Physics::RigidBody body = world.AddBody(position, glm::quat(1.f, 0.f, 0.f, 0.f), kBall, motion);
    REQUIRE(scene.Add(e, body) != nullptr);

    // Two steps: CaptureState retires the previous snapshot each time, so after two
    // the prev/cur pair actually straddles a real displacement.
    for (int32_t i = 0; i < 2; ++i)
    {
        world.Update(kStep);
        world.CaptureState();
    }
    return e;
}

} // namespace

TEST_CASE("InterpolateTransforms: the physics writeback stamps the Transform change tick")
{
    ECS::Scene            scene;
    Physics::PhysicsWorld world;

    const ECS::Entity e = SpawnSimulatedBody(scene, world, {0.f, 10.f, 0.f}, Physics::BodyMotion::Dynamic);

    // Bookmark taken after every setup write, exactly as a replication or
    // propagation consumer would record it at the end of its own pass.
    const uint64_t since = scene.CurrentChangeTick();
    REQUIRE_FALSE(scene.Changed<ECS::Transform>(e, since));

    world.InterpolateTransforms(scene, 0.5f);

    // The body fell, so the pose really did change...
    const ECS::Transform *t = scene.Get<ECS::Transform>(e);
    REQUIRE(t != nullptr);
    REQUIRE(t->position.y < 10.f);

    // ...and the change must be observable. Written through a plain Query this
    // assertion fails while the value above still passes: the exact silent hole.
    CHECK(scene.Changed<ECS::Transform>(e, since));
}

TEST_CASE("InterpolateTransforms: a static body is skipped and never stamped")
{
    ECS::Scene            scene;
    Physics::PhysicsWorld world;

    const ECS::Entity e = SpawnSimulatedBody(scene, world, {0.f, 10.f, 0.f}, Physics::BodyMotion::Static);

    const uint64_t since = scene.CurrentChangeTick();
    world.InterpolateTransforms(scene, 0.5f);

    // The mutable reference is taken after the motion-type/snapshot skips, so a
    // body the writeback declines to move burns no tick and stays off the wire.
    CHECK_FALSE(scene.Changed<ECS::Transform>(e, since));
    CHECK(scene.CurrentChangeTick() == since);

    // And its authored pose is left exactly as placed.
    const ECS::Transform *t = scene.Get<ECS::Transform>(e);
    REQUIRE(t != nullptr);
    CHECK(t->position.y == doctest::Approx(10.f));
}

TEST_CASE("InterpolateTransforms: an entity without a RigidBody is untouched")
{
    ECS::Scene            scene;
    Physics::PhysicsWorld world;

    // A plain placement entity, alongside a simulated one so the query is non-empty.
    const ECS::Entity simulated = SpawnSimulatedBody(scene, world, {0.f, 10.f, 0.f}, Physics::BodyMotion::Dynamic);
    const ECS::Entity placement = scene.Create();
    REQUIRE(scene.Add(placement, ECS::Transform{.position = {5.f, 5.f, 5.f}}) != nullptr);

    const uint64_t since = scene.CurrentChangeTick();
    world.InterpolateTransforms(scene, 0.5f);

    CHECK(scene.Changed<ECS::Transform>(simulated, since));
    CHECK_FALSE(scene.Changed<ECS::Transform>(placement, since));
}

TEST_CASE("InterpolateTransforms: one moved body costs exactly one change tick")
{
    ECS::Scene            scene;
    Physics::PhysicsWorld world;

    (void)SpawnSimulatedBody(scene, world, {0.f, 10.f, 0.f}, Physics::BodyMotion::Dynamic);

    const uint64_t since = scene.CurrentChangeTick();
    world.InterpolateTransforms(scene, 0.5f);

    // Position and rotation are both written, but through a single reference taken
    // from the Mut proxy — every mutable access stamps, so writing the two fields
    // through the proxy directly would burn two ticks per body per frame. Not a
    // correctness bug (ticks are uint64_t and over-reporting is safe), but at
    // hundreds of bodies × frames it inflates every consumer's bookmark for free.
    CHECK(scene.CurrentChangeTick() == since + 1);
}
