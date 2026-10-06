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

#include "PhysicsTestScene.hpp"

using namespace Assisi;
using Assisi::PhysicsTests::TestScene;

namespace
{

/// Enough steps for a body dropped a couple of metres to land and settle.
constexpr int32_t kSettleSteps = 240;

Core::Bitmask<Physics::CollisionChannel> AllChannelsBut(Physics::CollisionChannel channel)
{
    return Physics::AllChannels.Without(channel);
}

ECS::Entity SpawnBox(ECS::Scene &scene, glm::vec3 at, glm::vec3 halfExtents, bool isStatic,
                     Physics::CollisionFilter filter)
{
    const PhysicsTests::BodySpec descriptor =
        PhysicsTests::WithFilter(PhysicsTests::Box(halfExtents, isStatic), filter);
    return PhysicsTests::AddBody(scene, at, descriptor);
}

/// A wide static floor whose top is at y = 0.
ECS::Entity SpawnFloor(ECS::Scene &scene, Physics::CollisionFilter filter = Physics::CollisionFilter{})
{
    return SpawnBox(scene, {0.f, -1.f, 0.f}, {20.f, 1.f, 20.f}, /*isStatic=*/ true, filter);
}

float HeightOf(const Physics::PhysicsWorld &world, ECS::Entity entity)
{
    REQUIRE(world.HasBody(entity));
    return world.GetBodyPose(entity).position.y;
}

void Step(Physics::PhysicsWorld &world, int32_t steps = kSettleSteps)
{
    PhysicsTests::Step(world, steps);
}

} // namespace

TEST_CASE("Two bodies that admit each other collide")
{
    // The baseline the two refusal cases are measured against: with default masks
    // a box lands on a floor and stops on top of it.
    TestScene test;

    (void)SpawnFloor(test.scene);
    const ECS::Entity box = SpawnBox(test.scene, {0.f, 3.f, 0.f}, {0.5f, 0.5f, 0.5f}, /*isStatic=*/ false,
                                     Physics::CollisionFilter{});

    Step(test.world);
    CHECK(HeightOf(test.world, box) == doctest::Approx(0.5f).epsilon(0.1));
}

TEST_CASE("A body whose mask excludes the floor's channel falls through it")
{
    TestScene test;

    (void)SpawnFloor(test.scene);
    const ECS::Entity box =
        SpawnBox(test.scene, {0.f, 3.f, 0.f}, {0.5f, 0.5f, 0.5f}, /*isStatic=*/ false,
                 Physics::CollisionFilter{AllChannelsBut(Physics::CollisionChannel::World), Physics::CollisionChannel::Character});

    Step(test.world);
    CHECK(HeightOf(test.world, box) < -5.f);
}

TEST_CASE("A floor whose mask excludes the body's channel is fallen through too")
{
    // The same refusal from the other side. A filter that checked only the
    // caster's mask would pass the case above and fail this one, and the two are
    // indistinguishable to anything that only ever sets both masks the same way.
    TestScene test;

    (void)SpawnFloor(test.scene,
                     Physics::CollisionFilter{AllChannelsBut(Physics::CollisionChannel::Character),
                                              Physics::CollisionChannel::World});
    const ECS::Entity box =
        SpawnBox(test.scene, {0.f, 3.f, 0.f}, {0.5f, 0.5f, 0.5f}, /*isStatic=*/ false,
                 Physics::CollisionFilter{Physics::AllChannels, Physics::CollisionChannel::Character});

    Step(test.world);
    CHECK(HeightOf(test.world, box) < -5.f);
}

TEST_CASE("Editing a body's filter takes effect on the next step, not the next rebuild")
{
    // An edit that did not reach the live body would apply one play session late.
    TestScene test;

    (void)SpawnFloor(test.scene);
    const ECS::Entity box = SpawnBox(test.scene, {0.f, 3.f, 0.f}, {0.5f, 0.5f, 0.5f}, /*isStatic=*/ false,
                                     Physics::CollisionFilter{});
    test.world.Reconcile();

    Physics::Collider &descriptor = *test.scene.GetMut<Physics::Collider>(box);
    descriptor.collidesWith = AllChannelsBut(Physics::CollisionChannel::World);
    descriptor.channel = Physics::CollisionChannel::Character;
    test.world.Reconcile();

    // Reads back as set, and — the part that matters — the simulation agrees.
    const Physics::CollisionFilter now = test.world.GetBodyCollisionFilter(box);
    CHECK(now.channel == Physics::CollisionChannel::Character);
    CHECK(now.collidesWith == AllChannelsBut(Physics::CollisionChannel::World));

    Step(test.world);
    CHECK(HeightOf(test.world, box) < -5.f);
}

TEST_CASE("A body moved onto the Trigger channel stops blocking immediately")
{
    // The same edit for the case that also flips a body flag rather than only
    // layer bits: sensor-ness is not in the layer, so a filter change that
    // repacked the layer alone would leave a solid body on the trigger channel.
    TestScene test;

    (void)SpawnFloor(test.scene);
    const ECS::Entity box = SpawnBox(test.scene, {0.f, 3.f, 0.f}, {0.5f, 0.5f, 0.5f}, /*isStatic=*/ false,
                                     Physics::CollisionFilter{});
    test.world.Reconcile();

    test.scene.GetMut<Physics::Collider>(box)->channel = Physics::CollisionChannel::Trigger;
    test.scene.GetMut<Physics::RigidBody>(box)->motion = Physics::MotionType::Kinematic;

    Step(test.world);
    CHECK_FALSE(test.world.GetBodyPose(box).position.y < 0.f); // a sensor does not fall
    CHECK(test.world.GetBodyCollisionFilter(box).channel == Physics::CollisionChannel::Trigger);
}

TEST_CASE("A body made dynamic at runtime starts colliding with the static world")
{
    // A static body is rebuilt as a dynamic one rather than converted, and the
    // rebuilt one has to be in the moving broad-phase tree, paired with the floor
    // it now falls onto — or it would drop straight through.
    TestScene test;

    (void)SpawnFloor(test.scene);
    const ECS::Entity box = SpawnBox(test.scene, {0.f, 3.f, 0.f}, {0.5f, 0.5f, 0.5f}, /*isStatic=*/ true,
                                     Physics::CollisionFilter{});

    Step(test.world, 10);
    REQUIRE(HeightOf(test.world, box) == doctest::Approx(3.f)); // static: still where it was put

    REQUIRE(test.scene.Add(box, Physics::RigidBody{}) != nullptr);

    Step(test.world);
    CHECK(HeightOf(test.world, box) == doctest::Approx(0.5f).epsilon(0.1));
}
