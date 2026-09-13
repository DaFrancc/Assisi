/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCollisionChannels.cpp
/// @brief Which bodies a body collides with, and the layer bits that decide it.
///
/// The rule is two-way: a pair interacts only if each side's mask admits the
/// other's channel. A one-sided implementation passes every test where the two
/// masks agree, which is most of them, so the cases here deliberately disagree in
/// each direction separately.
///
/// The motion cases exist because the channel and the mask are not the whole of a
/// body's object layer — the motion type rides there too, and decides which
/// broad-phase tree the body lives in. Nothing reads that field directly, so the
/// only way it can be shown to be right is a body that falls when it should.

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

/// Enough steps for a body dropped a couple of metres to land and settle.
constexpr int32_t kSettleSteps = 240;

std::uint32_t Without(Physics::CollisionChannel channel)
{
    return Physics::AllChannels & ~(1u << static_cast<std::uint32_t>(channel));
}

ECS::Entity SpawnBox(ECS::Scene &scene, Physics::PhysicsWorld &world, glm::vec3 at, glm::vec3 halfExtents,
                     bool isStatic, Physics::CollisionChannel channel, std::uint32_t collides)
{
    const ECS::Entity entity = scene.Create();
    ECS::Transform *transform = scene.Add<ECS::Transform>(entity);
    REQUIRE(transform != nullptr);
    transform->position = at;

    Physics::RigidBodyDescriptor descriptor{};
    descriptor.halfExtents  = halfExtents;
    descriptor.isStatic     = isStatic;
    descriptor.channel      = channel;
    descriptor.collidesWith = collides;
    REQUIRE(scene.Add<Physics::RigidBodyDescriptor>(entity, descriptor) != nullptr);

    (void)world.AddBodyFromDescriptor(scene, entity, *transform, descriptor);
    return entity;
}

/// A wide static floor whose top is at y = 0.
ECS::Entity SpawnFloor(ECS::Scene &scene, Physics::PhysicsWorld &world,
                       Physics::CollisionChannel channel  = Physics::CollisionChannel::World,
                       std::uint32_t             collides = Physics::AllChannels)
{
    return SpawnBox(scene, world, {0.f, -1.f, 0.f}, {20.f, 1.f, 20.f}, /*isStatic=*/ true, channel,
                    collides);
}

float HeightOf(ECS::Scene &scene, const Physics::PhysicsWorld &world, ECS::Entity entity)
{
    const Physics::RigidBody *body = scene.Get<Physics::RigidBody>(entity);
    REQUIRE(body != nullptr);
    return world.GetBodyTransform(*body).first.y;
}

void Step(Physics::PhysicsWorld &world, int32_t steps = kSettleSteps)
{
    for (int32_t i = 0; i < steps; ++i)
        world.Update(kStep);
}

} // namespace

TEST_CASE("Two bodies that admit each other collide")
{
    // The baseline the two refusal cases are measured against: with default masks
    // a box lands on a floor and stops on top of it.
    ECS::Scene scene;
    Physics::PhysicsWorld world;

    (void)SpawnFloor(scene, world);
    const ECS::Entity box = SpawnBox(scene, world, {0.f, 3.f, 0.f}, {0.5f, 0.5f, 0.5f},
                                     /*isStatic=*/ false, Physics::CollisionChannel::World,
                                     Physics::AllChannels);

    Step(world);
    CHECK(HeightOf(scene, world, box) == doctest::Approx(0.5f).epsilon(0.1));
}

TEST_CASE("A body whose mask excludes the floor's channel falls through it")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world;

    (void)SpawnFloor(scene, world);
    const ECS::Entity box = SpawnBox(scene, world, {0.f, 3.f, 0.f}, {0.5f, 0.5f, 0.5f},
                                     /*isStatic=*/ false, Physics::CollisionChannel::Character,
                                     Without(Physics::CollisionChannel::World));

    Step(world);
    CHECK(HeightOf(scene, world, box) < -5.f);
}

TEST_CASE("A floor whose mask excludes the body's channel is fallen through too")
{
    // The same refusal from the other side. A filter that checked only the
    // caster's mask would pass the case above and fail this one, and the two are
    // indistinguishable to anything that only ever sets both masks the same way.
    ECS::Scene scene;
    Physics::PhysicsWorld world;

    (void)SpawnFloor(scene, world, Physics::CollisionChannel::World,
                     Without(Physics::CollisionChannel::Character));
    const ECS::Entity box = SpawnBox(scene, world, {0.f, 3.f, 0.f}, {0.5f, 0.5f, 0.5f},
                                     /*isStatic=*/ false, Physics::CollisionChannel::Character,
                                     Physics::AllChannels);

    Step(world);
    CHECK(HeightOf(scene, world, box) < -5.f);
}

TEST_CASE("A body made dynamic at runtime starts colliding with the static world")
{
    // The motion type lives in the object layer beside the channel, and the layer
    // decides which broad-phase tree the body is tested against. Left stale on a
    // motion change, the body would keep claiming it never moves and would be
    // paired with nothing — falling straight through the floor it was resting on.
    ECS::Scene scene;
    Physics::PhysicsWorld world;

    (void)SpawnFloor(scene, world);
    const ECS::Entity box = SpawnBox(scene, world, {0.f, 3.f, 0.f}, {0.5f, 0.5f, 0.5f},
                                     /*isStatic=*/ true, Physics::CollisionChannel::World,
                                     Physics::AllChannels);

    Step(world, 10);
    REQUIRE(HeightOf(scene, world, box) == doctest::Approx(3.f)); // static: still where it was put

    const Physics::RigidBody *body = scene.Get<Physics::RigidBody>(box);
    REQUIRE(body != nullptr);
    world.SetBodyMotionType(*body, Physics::BodyMotion::Dynamic);

    Step(world);
    CHECK(HeightOf(scene, world, box) == doctest::Approx(0.5f).epsilon(0.1));
}
