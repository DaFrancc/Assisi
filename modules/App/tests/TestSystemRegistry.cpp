/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/ECS/Scene.hpp>

using namespace Assisi::App;

namespace
{
// Drive the render phase: it uses the same TopoSort as the game phases but its
// RenderContext needs no InputContext/window, so it's the cleanest way to test
// dependency ordering in isolation.
RenderContext MakeCtx(Assisi::ECS::Scene &scene)
{
    return RenderContext{scene, 0.0f, glm::mat4(1.0f), glm::mat4(1.0f)};
}

// A game-phase context with no input devices — the headless shape a dedicated
// server (and these tests) run in. Null input is why SystemContext holds
// pointers there: an InputContext cannot exist without a window.
SystemContext MakeGameCtx(World &world, Assisi::Core::EventQueue &events, bool isActiveWorld)
{
    return SystemContext{world,  0.016f, /*input=*/nullptr, /*actions=*/nullptr,
                         events, isActiveWorld};
}

std::size_t IndexOf(const std::vector<std::string> &order, const std::string &name)
{
    return static_cast<std::size_t>(std::find(order.begin(), order.end(), name) - order.begin());
}
} // namespace

TEST_CASE("SystemRegistry: After/Before constraints determine execution order")
{
    SystemRegistry systems;
    Assisi::ECS::Scene scene;
    std::vector<std::string> order;

    auto record = [&order](const char *name) {
        return [&order, name](RenderContext &) { order.emplace_back(name); };
    };

    // Registration order is deliberately "wrong"; constraints must fix it.
    systems.RegisterRender("Draw", record("Draw")).After("Cull");
    systems.RegisterRender("Cull", record("Cull")).After("Culling.Prepare");
    systems.RegisterRender("Culling.Prepare", record("Culling.Prepare"));

    systems.RunRender(MakeCtx(scene));

    REQUIRE(order.size() == 3);
    CHECK(IndexOf(order, "Culling.Prepare") < IndexOf(order, "Cull"));
    CHECK(IndexOf(order, "Cull") < IndexOf(order, "Draw"));
}

TEST_CASE("SystemRegistry: Before is honored symmetrically with After")
{
    SystemRegistry systems;
    Assisi::ECS::Scene scene;
    std::vector<std::string> order;

    auto record = [&order](const char *name) {
        return [&order, name](RenderContext &) { order.emplace_back(name); };
    };

    systems.RegisterRender("Late", record("Late"));
    systems.RegisterRender("Early", record("Early")).Before("Late");

    systems.RunRender(MakeCtx(scene));

    REQUIRE(order.size() == 2);
    CHECK(order[0] == "Early");
    CHECK(order[1] == "Late");
}

TEST_CASE("SystemRegistry: a dependency on an unregistered system still runs everything")
{
    SystemRegistry systems;
    Assisi::ECS::Scene scene;
    std::vector<std::string> order;

    systems.RegisterRender("Solo",
                           [&order](RenderContext &) { order.emplace_back("Solo"); })
        .After("GhostThatWasNeverRegistered");

    systems.RunRender(MakeCtx(scene));

    CHECK(order.size() == 1); // logged an error, but did not drop the system
}

TEST_CASE("SystemRegistry: game phases run headlessly, with the world in the context")
{
    // No window, so no InputContext — a dedicated server and these tests share
    // that shape. Systems reach entities through ctx.world.scene.
    WorldManager             worlds;
    World                   &world = worlds.Create("Test");
    Assisi::Core::EventQueue events;
    SystemRegistry           systems;

    const Assisi::ECS::Entity entity = world.scene.Create();

    World      *seenWorld  = nullptr;
    std::size_t seenAlive  = 0;
    systems.Register(SystemPhase::Update, "Inspect",
                     [&](SystemContext &ctx)
                     {
                         seenWorld = &ctx.world;
                         seenAlive = ctx.world.scene.IsAlive(entity) ? 1u : 0u;
                         CHECK(ctx.input == nullptr);
                     });

    systems.Run(SystemPhase::Update, MakeGameCtx(world, events, /*isActiveWorld=*/true));

    CHECK(seenWorld == &world);
    CHECK(seenAlive == 1u);
}

TEST_CASE("SystemRegistry: ActiveWorldOnly systems run only in the active world")
{
    // One InputContext, N resident worlds: a controller system must not apply the
    // same keypresses in every world (docs/multi-scene-design-notes.md §1).
    WorldManager             worlds;
    World                   &world = worlds.Create("Test");
    Assisi::Core::EventQueue events;
    SystemRegistry           systems;

    int32_t everywhere = 0;
    int32_t activeOnly = 0;

    systems.Register(SystemPhase::Update, "Everywhere", [&](SystemContext &) { ++everywhere; });
    systems.Register(SystemPhase::Update, "ActiveOnly", [&](SystemContext &) { ++activeOnly; })
        .ActiveWorldOnly();

    systems.Run(SystemPhase::Update, MakeGameCtx(world, events, /*isActiveWorld=*/true));
    CHECK(everywhere == 1);
    CHECK(activeOnly == 1);

    systems.Run(SystemPhase::Update, MakeGameCtx(world, events, /*isActiveWorld=*/false));
    CHECK(everywhere == 2); // world-agnostic system still ticks
    CHECK(activeOnly == 1); // ...the input-consuming one does not
}

TEST_CASE("SystemRegistry: skipping an ActiveWorldOnly system preserves the order of the rest")
{
    // The gate is a dispatch-time skip, not a re-sort: the surviving systems must
    // keep the order their After()/Before() constraints define.
    WorldManager             worlds;
    World                   &world = worlds.Create("Test");
    Assisi::Core::EventQueue events;
    SystemRegistry           systems;
    std::vector<std::string> order;

    auto record = [&order](const char *name)
    { return [&order, name](SystemContext &) { order.emplace_back(name); }; };

    systems.Register(SystemPhase::Update, "Last", record("Last")).After("Middle");
    systems.Register(SystemPhase::Update, "Middle", record("Middle")).After("First");
    systems.Register(SystemPhase::Update, "First", record("First"));
    // The gated system sits in the middle of the chain by registration, but
    // declares no constraints — its absence must not disturb the others.
    systems.Register(SystemPhase::Update, "Gated", record("Gated")).ActiveWorldOnly();

    systems.Run(SystemPhase::Update, MakeGameCtx(world, events, /*isActiveWorld=*/false));

    REQUIRE(order.size() == 3);
    CHECK(order[0] == "First");
    CHECK(order[1] == "Middle");
    CHECK(order[2] == "Last");
}

TEST_CASE("SystemRegistry: a dependency cycle falls back to running all systems")
{
    SystemRegistry systems;
    Assisi::ECS::Scene scene;
    std::vector<std::string> order;

    auto record = [&order](const char *name) {
        return [&order, name](RenderContext &) { order.emplace_back(name); };
    };

    // A <-> B mutual dependency: unschedulable, must not silently drop systems.
    systems.RegisterRender("A", record("A")).After("B");
    systems.RegisterRender("B", record("B")).After("A");

    systems.RunRender(MakeCtx(scene));

    CHECK(order.size() == 2); // fallback to registration order — nothing dropped
}
