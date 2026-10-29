/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestStepWorld.cpp
/// @brief One fixed step, live or replayed.

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <string_view>

#include <Assisi/App/MotionSystems.hpp>
#include <Assisi/App/StepWorld.hpp>
#include <Assisi/App/SystemCatalog.hpp>
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

App::SystemContext ContextFor(App::World &world, Core::EventQueue &universal, bool replaying)
{
    App::SystemContext ctx{world, kStep, kTick, nullptr, nullptr, universal, true, nullptr};
    ctx.replaying = replaying;
    return ctx;
}

ECS::Entity AddWalker(App::World &world, glm::vec3 feet)
{
    const ECS::Entity entity = world.scene.Create();
    (void)world.scene.Add<ECS::Transform>(entity, ECS::Transform{.position = feet});
    (void)world.scene.Add<Physics::SourceMovement>(entity);
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

/// Where a walker starts above the floor, so its landing is a contact Enter.
constexpr glm::vec3 kDropFrom{0.f, 1.f, 0.f};

/// Steps a walker is given to land, more than its fall from kDropFrom takes.
constexpr std::int32_t kLandSteps = 120;

/// Where a walker is lifted to, well clear of the floor.
constexpr glm::vec3 kLiftedTo{0.f, 20.f, 0.f};

/// Whether the most recent physics step reported @p phase for @p entity.
bool Reported(const App::World &world, ECS::Entity entity, Physics::ContactPhase phase)
{
    for (const Physics::ContactEvent &event : world.physics.ContactEvents())
    {
        if (event.entity == entity && event.phase == phase)
        {
            return true;
        }
    }
    return false;
}

/// Steps @p world live until @p walker reports landing on something, at most
/// kLandSteps times. Returns whether it did.
bool StepUntilLanded(App::World &world, Core::EventQueue &universal, ECS::Entity walker)
{
    for (std::int32_t i = 0; i < kLandSteps; ++i)
    {
        App::StepWorld(ContextFor(world, universal, /*replaying=*/ false));
        if (Reported(world, walker, Physics::ContactPhase::Enter))
        {
            return true;
        }
    }
    return false;
}

/// A replayed system reading a character the replay does not step: by handle,
/// and by a query that names the frozen.
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

TEST_CASE("A tick stepped live and then replayed runs a game's systems once and a Replayed one twice")
{
    App::World world{Physics::NoCollisionAssets()};
    Core::EventQueue universal{Core::EventScope::Universal};

    std::int32_t before = 0;
    std::int32_t after = 0;
    std::int32_t replayed = 0;
    world.systems.Register(App::SystemPhase::FixedUpdate, "Before", [&before](App::SystemContext &) { ++before; });
    world.systems.Register(App::SystemPhase::PostFixedUpdate, "After", [&after](App::SystemContext &) { ++after; });
    world.systems.Register(App::SystemPhase::FixedUpdate, "Replayed", [&replayed](App::SystemContext &)
                           { ++replayed; })
    .Replayed();

    App::StepWorld(ContextFor(world, universal, /*replaying=*/ false));
    App::StepWorld(ContextFor(world, universal, /*replaying=*/ true));

    CHECK(before == 1);
    CHECK(after == 1);
    CHECK(replayed == 2);
}

TEST_CASE("The engine's command and eye systems step again in a replay, and the rest of its systems do not")
{
    const App::SystemCatalog &catalog = App::SystemCatalog::Instance();
    for (const std::string_view name : {"CharacterCommand", "CharacterEye"})
    {
        const App::SystemDefinition *definition = catalog.Find(name);
        REQUIRE(definition != nullptr);
        CHECK(definition->replayed);
    }
    for (const std::string_view name : {"LocalPlayer", "Oscillate", "TimeOfDay"})
    {
        const App::SystemDefinition *definition = catalog.Find(name);
        REQUIRE(definition != nullptr);
        CHECK_FALSE(definition->replayed);
    }
}

TEST_CASE("A replayed step moves only the characters it is given")
{
    App::World world{Physics::NoCollisionAssets()};
    Core::EventQueue universal{Core::EventScope::Universal};

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
        App::StepWorld(ContextFor(world, universal, /*replaying=*/ false));
    }

    for (const ECS::Entity walker : {replayed, frozen})
    {
        world.scene.GetMut<Physics::CharacterIntent>(walker)->move = {1.f, 0.f, 0.f};
    }
    const glm::vec3 replayedBefore = world.scene.Get<ECS::Transform>(replayed)->position;
    const glm::vec3 frozenBefore = world.scene.Get<ECS::Transform>(frozen)->position;

    const std::array<ECS::Entity, 1> only{replayed};
    App::StepWorld(ContextFor(world, universal, /*replaying=*/ true), only);

    CHECK(glm::length(world.scene.Get<ECS::Transform>(replayed)->position - replayedBefore) > kMoved);
    CHECK(world.scene.Get<ECS::Transform>(frozen)->position == frozenBefore);
}

TEST_CASE("A replay steps only what it replays, and a system still reads the rest where it stands")
{
    App::World world{Physics::NoCollisionAssets()};
    Core::EventQueue universal{Core::EventScope::Universal};
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
                           [&watch](App::SystemContext &ctx) { watch.Step(ctx); })
    .Replayed();

    for (std::int32_t i = 0; i < kSettleSteps; ++i)
    {
        App::StepWorld(ContextFor(world, universal, /*replaying=*/ false));
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
        App::SystemContext ctx = ContextFor(world, universal, /*replaying=*/ true);
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

TEST_CASE("A replayed step reports no contacts, not the live step's again")
{
    App::World world{Physics::NoCollisionAssets()};
    Core::EventQueue universal{Core::EventScope::Universal};
    AddFloor(world);
    const ECS::Entity walker = AddWalker(world, kDropFrom);
    REQUIRE(StepUntilLanded(world, universal, walker));

    const std::array<ECS::Entity, 1> replayed{walker};
    App::StepWorld(ContextFor(world, universal, /*replaying=*/ true), replayed);

    CHECK(world.physics.ContactEvents().empty());
}

TEST_CASE("What a replayed step's sweep touched is not taken for the next live step's")
{
    App::World world{Physics::NoCollisionAssets()};
    Core::EventQueue universal{Core::EventScope::Universal};
    AddFloor(world);
    const ECS::Entity walker = AddWalker(world, kDropFrom);
    REQUIRE(StepUntilLanded(world, universal, walker));

    // Standing on the floor, so the replayed sweep touches it.
    const std::array<ECS::Entity, 1> replayed{walker};
    App::StepWorld(ContextFor(world, universal, /*replaying=*/ true), replayed);

    // Clear of the floor for the live step: nothing it sweeps touches the floor,
    // so the pair ends there.
    world.physics.Teleport(walker, Physics::Pose{glm::quat(1.f, 0.f, 0.f, 0.f), kLiftedTo});
    App::StepWorld(ContextFor(world, universal, /*replaying=*/ false));

    CHECK(Reported(world, walker, Physics::ContactPhase::Exit));
}
