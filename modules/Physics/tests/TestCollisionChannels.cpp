/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCollisionChannels.cpp
/// @brief Which bodies a body collides with, the layers that decide it, and the
/// names a game gives its own channels.
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

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <Assisi/Core/Reflect/ComponentMeta.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/Core/Reflect/EnumLabels.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/CollisionChannelNames.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

#include "PhysicsTestScene.hpp"

using namespace Assisi;
using Assisi::PhysicsTests::TestScene;

namespace
{

/// Enough steps for a body dropped a couple of metres to land and settle.
constexpr int32_t kSettleSteps = 240;

Core::Bitmask<Physics::CollisionChannel> Without(Physics::CollisionChannel channel)
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
                 Physics::CollisionFilter{Without(Physics::CollisionChannel::World), Physics::CollisionChannel::Character});

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
                     Physics::CollisionFilter{Without(Physics::CollisionChannel::Character),
                                              Physics::CollisionChannel::World});
    const ECS::Entity box =
        SpawnBox(test.scene, {0.f, 3.f, 0.f}, {0.5f, 0.5f, 0.5f}, /*isStatic=*/ false,
                 Physics::CollisionFilter{Physics::AllChannels, Physics::CollisionChannel::Character});

    Step(test.world);
    CHECK(HeightOf(test.world, box) < -5.f);
}

TEST_CASE("Bodies on the last game channel collide when both masks admit it, and not when either refuses")
{
    // The highest slot, so a filter that kept only some of a mask's bits, or only
    // some of a channel's index, cannot pass by accident.
    constexpr Physics::CollisionChannel kGame = Physics::GameChannel(Physics::kGameChannelCount - 1u);
    const Core::Bitmask<Physics::CollisionChannel> onlyGame = Core::Bitmask<Physics::CollisionChannel>::Of(kGame);

    SUBCASE("both admit")
    {
        TestScene test;
        (void)SpawnFloor(test.scene, Physics::CollisionFilter{onlyGame, kGame});
        const ECS::Entity box = SpawnBox(test.scene, {0.f, 3.f, 0.f}, {0.5f, 0.5f, 0.5f}, /*isStatic=*/ false,
                                         Physics::CollisionFilter{onlyGame, kGame});
        Step(test.world);
        CHECK(HeightOf(test.world, box) == doctest::Approx(0.5f).epsilon(0.1));
    }

    SUBCASE("the floor refuses")
    {
        TestScene test;
        (void)SpawnFloor(test.scene, Physics::CollisionFilter{Without(kGame), kGame});
        const ECS::Entity box = SpawnBox(test.scene, {0.f, 3.f, 0.f}, {0.5f, 0.5f, 0.5f}, /*isStatic=*/ false,
                                         Physics::CollisionFilter{onlyGame, kGame});
        Step(test.world);
        CHECK(HeightOf(test.world, box) < -5.f);
    }

    SUBCASE("the box refuses")
    {
        TestScene test;
        (void)SpawnFloor(test.scene, Physics::CollisionFilter{onlyGame, kGame});
        const ECS::Entity box = SpawnBox(test.scene, {0.f, 3.f, 0.f}, {0.5f, 0.5f, 0.5f}, /*isStatic=*/ false,
                                         Physics::CollisionFilter{Without(kGame), kGame});
        Step(test.world);
        CHECK(HeightOf(test.world, box) < -5.f);
    }
}

TEST_CASE("A ray aimed at a game channel passes what is not on it")
{
    // A World box stands in front of a box on a game channel. The ray admits only
    // the game channel, so it has to pass the first and stop at the second.
    constexpr Physics::CollisionChannel kTargetChannel = Physics::GameChannel(Physics::kGameChannelCount - 1u);
    TestScene test;

    const ECS::Entity nearer = SpawnBox(test.scene, {2.f, 0.f, 0.f}, {0.5f, 0.5f, 0.5f}, /*isStatic=*/ true,
                                        Physics::CollisionFilter{});
    const ECS::Entity target =
        SpawnBox(test.scene, {5.f, 0.f, 0.f}, {0.5f, 0.5f, 0.5f}, /*isStatic=*/ true,
                 Physics::CollisionFilter{Physics::AllChannels, kTargetChannel});
    test.world.Reconcile();

    const Physics::CollisionFilter ray{Core::Bitmask<Physics::CollisionChannel>::Of(kTargetChannel),
                                       Physics::GameChannel(3)};
    const std::optional<Physics::QueryHit> hit = test.world.CastRay({0.f, 0.f, 0.f}, {10.f, 0.f, 0.f}, ray,
                                                                    ECS::NullEntity);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == target);
    CHECK(hit->entity != nearer);
}

TEST_CASE("Every body keeps the whole filter it was given, however many distinct filters there are")
{
    // More distinct masks than a packed layer could ever hold, spread over every
    // bit, on both static and moving bodies. Each has to read back exactly.
    constexpr int32_t kBodies = 60;

    // Odd, so multiplying by it is a bijection on 32 bits and every mask differs.
    constexpr std::uint32_t kSpread = 0x9E3779B9u;

    TestScene test;
    std::vector<ECS::Entity> bodies;
    std::vector<Physics::CollisionFilter> filters;
    for (int32_t i = 0; i < kBodies; ++i)
    {
        // Every channel but Trigger, which would make the body a sensor.
        const uint32_t slot = static_cast<uint32_t>(i) % static_cast<uint32_t>(Physics::CollisionChannel::Count);
        const Physics::CollisionChannel channel = slot == static_cast<uint32_t>(Physics::CollisionChannel::Trigger)
                                                      ? Physics::GameChannel(0)
                                                      : static_cast<Physics::CollisionChannel>(slot);
        const uint32_t mask = static_cast<uint32_t>(i + 1) * kSpread;
        const Physics::CollisionFilter filter{Core::Bitmask<Physics::CollisionChannel>{mask}, channel};
        filters.push_back(filter);
        bodies.push_back(SpawnBox(test.scene, {3.f * static_cast<float>(i), 0.f, 0.f}, {0.5f, 0.5f, 0.5f},
                                  /*isStatic=*/ i % 2 == 0, filter));
    }
    test.world.Reconcile();

    for (int32_t i = 0; i < kBodies; ++i)
    {
        const Physics::CollisionFilter read = test.world.GetBodyCollisionFilter(bodies[static_cast<std::size_t>(i)]);
        CHECK(read.channel == filters[static_cast<std::size_t>(i)].channel);
        CHECK(read.collidesWith == filters[static_cast<std::size_t>(i)].collidesWith);
    }
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
    descriptor.collidesWith = Without(Physics::CollisionChannel::World);
    descriptor.channel = Physics::CollisionChannel::Character;
    test.world.Reconcile();

    // Reads back as set, and — the part that matters — the simulation agrees.
    const Physics::CollisionFilter now = test.world.GetBodyCollisionFilter(box);
    CHECK(now.channel == Physics::CollisionChannel::Character);
    CHECK(now.collidesWith == Without(Physics::CollisionChannel::World));

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

namespace
{

constexpr Physics::ChannelName kGoodNames[] = {
    {"Bullet", Physics::GameChannel(0)},
    {"Pickup", Physics::GameChannel(1)},
};
constexpr Physics::ChannelName kBuiltInRenamed[] = {{"Floor", Physics::CollisionChannel::World}};
constexpr Physics::ChannelName kSlotTwice[] = {{"Bullet", Physics::GameChannel(0)}, {"Shot", Physics::GameChannel(0)}};
constexpr Physics::ChannelName kNameTwice[] = {{"Bullet", Physics::GameChannel(0)}, {"Bullet", Physics::GameChannel(1)}};
constexpr Physics::ChannelName kUnnamed[] = {{"", Physics::GameChannel(0)}};

// The way a game registers its table, so the macro itself is compiled and run.
ASSISI_COLLISION_CHANNEL_NAMES(kGoodNames);

static_assert(!Physics::ValidChannelNames(kBuiltInRenamed), "the engine's own channels keep their names");
static_assert(!Physics::ValidChannelNames(kSlotTwice), "one slot, one name");
static_assert(!Physics::ValidChannelNames(kNameTwice), "one name, one slot");
static_assert(!Physics::ValidChannelNames(kUnnamed), "a channel needs a name");

} // namespace

TEST_CASE("A game's channel names reach the labels the editor reads for Collider's channel fields")
{
    // The registry is keyed by the enum's name, so a key that differed from the
    // one reflection writes on the field would leave every game channel
    // unnamed in the editor without anything failing.
    const Core::Reflect::ComponentMeta *collider = Core::Reflect::ComponentRegistry::Instance().Find("Collider");
    REQUIRE(collider != nullptr);
    for (const Core::Reflect::FieldMeta &field : collider->fields)
    {
        if (field.name == "channel" || field.name == "collidesWith")
        {
            CHECK(field.enumType == Physics::kCollisionChannelEnumType);
        }
    }

    constexpr Physics::ChannelName kNames[] = {{"TestProjectile", Physics::GameChannel(Physics::kGameChannelCount - 1u)}};
    const Physics::ChannelNameRegistration registration{kNames};
    (void)registration;

    bool registeredByHand = false;
    bool registeredByMacro = false;
    for (const Core::Reflect::EnumConstant &label : Core::Reflect::EnumLabelsOf(Physics::kCollisionChannelEnumType))
    {
        registeredByHand = registeredByHand ||
                           (label.name == "TestProjectile" &&
                            label.value == static_cast<std::int64_t>(Physics::GameChannel(Physics::kGameChannelCount - 1u)));
        registeredByMacro = registeredByMacro ||
                            (label.name == "Bullet" && label.value == static_cast<std::int64_t>(Physics::GameChannel(0)));
    }
    CHECK(registeredByHand);
    CHECK(registeredByMacro);
}
