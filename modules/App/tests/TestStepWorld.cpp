/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestStepWorld.cpp
/// @brief One fixed step, live or replayed.

#include <doctest/doctest.h>

#include <array>
#include <cstdint>

#include <Assisi/App/MotionSystems.hpp>
#include <Assisi/App/StepWorld.hpp>
#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Components.hpp>

using namespace Assisi;

namespace
{

constexpr float kStep = 1.f / 60.f;

/// The tick a test steps twice, once live and once replayed.
constexpr std::uint64_t kTick = 42;

/// Steps a character is given to come to rest on the floor.
constexpr std::int32_t kSettleSteps = 30;

/// How far counts as having moved (m).
constexpr float kMoved = 1e-3f;

App::SystemContext ContextFor(App::World &world, Core::EventQueue &events, bool replaying)
{
    App::SystemContext ctx{world, kStep, kTick, nullptr, nullptr, events, true, nullptr};
    ctx.replaying = replaying;
    return ctx;
}

ECS::Entity AddWalker(App::World &world, glm::vec3 feet)
{
    const ECS::Entity entity = world.scene.Create();
    (void)world.scene.Add<ECS::Transform>(entity, ECS::Transform{.position = feet});
    (void)world.scene.Add<Physics::Character>(entity);
    return entity;
}

void AddFloor(App::World &world)
{
    const ECS::Entity floor = world.scene.Create();
    (void)world.scene.Add<ECS::Transform>(floor, ECS::Transform{.position = {0.f, -1.f, 0.f}});
    Physics::Collider collider;
    collider.shape = Physics::ColliderShape::Box;
    collider.halfExtents = {50.f, 1.f, 50.f};
    (void)world.scene.Add<Physics::Collider>(floor, collider);
}

/// Steps a replay runs in the case that counts them.
constexpr std::uint64_t kReplayedSteps = 10;

/// Where a mover travels from, and how far.
constexpr glm::vec3 kMoverOrigin{0.f, 3.f, 10.f};
constexpr float kMoverAmplitude = 2.f;

/// A system of the game reading a character the replay does not step: by
/// handle, and by a query that names the frozen.
struct RemoteWatch
{
    ECS::Entity remote{ECS::NullEntity};
    std::int32_t remoteSeen = 0;
    std::int32_t charactersSeen = 0;

    void Step(App::SystemContext &ctx)
    {
        remoteSeen += ctx.world.scene.Get<Physics::Character>(remote) != nullptr ? 1 : 0;
        for (auto [entity, character] : ctx.world.scene.Query<Physics::Character, ECS::IncludingFrozen>())
        {
            (void)entity;
            (void)character;
            ++charactersSeen;
        }
    }
};

} // namespace

TEST_CASE("A tick stepped live and then replayed runs a liveOnly system once and the rest twice")
{
    App::World world{Physics::NoCollisionAssets()};
    Core::EventQueue events;

    std::int32_t every = 0;
    std::int32_t liveOnly = 0;
    std::int32_t afterStep = 0;
    world.systems.Register(App::SystemPhase::FixedUpdate, "Every", [&every](App::SystemContext &) { ++every; });
    world.systems.Register(App::SystemPhase::FixedUpdate, "LiveOnly", [&liveOnly](App::SystemContext &)
                           { ++liveOnly; })
    .LiveOnly();
    world.systems.Register(App::SystemPhase::PostFixedUpdate, "AfterStep", [&afterStep](App::SystemContext &)
                           { ++afterStep; });

    App::StepWorld(ContextFor(world, events, /*replaying=*/ false));
    App::StepWorld(ContextFor(world, events, /*replaying=*/ true));

    CHECK(every == 2);
    CHECK(afterStep == 2);
    CHECK(liveOnly == 1);
}

TEST_CASE("A replayed step moves only the characters it is given")
{
    App::World world{Physics::NoCollisionAssets()};
    Core::EventQueue events;

    const ECS::Entity floor = world.scene.Create();
    (void)world.scene.Add<ECS::Transform>(floor, ECS::Transform{.position = {0.f, -1.f, 0.f}});
    Physics::Collider collider;
    collider.shape = Physics::ColliderShape::Box;
    collider.halfExtents = {50.f, 1.f, 50.f};
    (void)world.scene.Add<Physics::Collider>(floor, collider);

    const ECS::Entity replayed = AddWalker(world, {0.f, 0.f, 0.f});
    const ECS::Entity frozen = AddWalker(world, {5.f, 0.f, 0.f});
    for (std::int32_t i = 0; i < kSettleSteps; ++i)
    {
        App::StepWorld(ContextFor(world, events, /*replaying=*/ false));
    }

    for (const ECS::Entity walker : {replayed, frozen})
    {
        world.scene.GetMut<Physics::CharacterIntent>(walker)->move = {1.f, 0.f, 0.f};
    }
    const glm::vec3 replayedBefore = world.scene.Get<ECS::Transform>(replayed)->position;
    const glm::vec3 frozenBefore = world.scene.Get<ECS::Transform>(frozen)->position;

    const std::array<ECS::Entity, 1> only{replayed};
    App::StepWorld(ContextFor(world, events, /*replaying=*/ true), only);

    CHECK(glm::length(world.scene.Get<ECS::Transform>(replayed)->position - replayedBefore) > kMoved);
    CHECK(world.scene.Get<ECS::Transform>(frozen)->position == frozenBefore);
}

TEST_CASE("A replay steps only what it replays, and a system still reads the rest where it stands")
{
    App::World world{Physics::NoCollisionAssets()};
    Core::EventQueue events;
    AddFloor(world);
    const ECS::Entity predicted = AddWalker(world, {0.f, 0.f, 0.f});
    const ECS::Entity remote = AddWalker(world, {5.f, 0.f, 0.f});
    const ECS::Entity mover = world.scene.Create();
    (void)world.scene.Add<ECS::Transform>(mover, ECS::Transform{.position = kMoverOrigin});
    (void)world.scene.Add<Runtime::Oscillator>(
        mover, Runtime::Oscillator{.origin = kMoverOrigin, .axis = {1.f, 0.f, 0.f}, .amplitude = kMoverAmplitude});
    world.systems.Register(App::SystemPhase::FixedUpdate, "Oscillate",
                           [](App::SystemContext &ctx) { App::OscillateSystem(ctx); });

    RemoteWatch watch{.remote = remote};
    world.systems.Register(App::SystemPhase::FixedUpdate, "Watch",
                           [&watch](App::SystemContext &ctx) { watch.Step(ctx); });

    for (std::int32_t i = 0; i < kSettleSteps; ++i)
    {
        App::StepWorld(ContextFor(world, events, /*replaying=*/ false));
    }
    for (const ECS::Entity walker : {predicted, remote})
    {
        world.scene.GetMut<Physics::CharacterIntent>(walker)->move = {0.f, 0.f, 1.f};
    }
    const glm::vec3 predictedBefore = world.scene.Get<ECS::Transform>(predicted)->position;
    const glm::vec3 remoteBefore = world.scene.Get<ECS::Transform>(remote)->position;
    const glm::vec3 moverBefore = world.scene.Get<ECS::Transform>(mover)->position;
    watch = RemoteWatch{.remote = remote};

    const std::array<ECS::Entity, 1> replayed{predicted};
    world.scene.BeginReplay(replayed);
    for (std::uint64_t step = 1; step <= kReplayedSteps; ++step)
    {
        App::SystemContext ctx = ContextFor(world, events, /*replaying=*/ true);
        ctx.simTick = kTick + step;
        App::StepWorld(ctx, replayed);
    }
    world.scene.EndReplay();

    CHECK(glm::length(world.scene.Get<ECS::Transform>(predicted)->position - predictedBefore) > kMoved);
    CHECK(world.scene.Get<ECS::Transform>(remote)->position == remoteBefore);
    CHECK(world.scene.Get<ECS::Transform>(mover)->position == moverBefore);
    CHECK(watch.remoteSeen == static_cast<std::int32_t>(kReplayedSteps));
    CHECK(watch.charactersSeen == static_cast<std::int32_t>(2u * kReplayedSteps));
}
