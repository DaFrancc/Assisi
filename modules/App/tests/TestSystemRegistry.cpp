/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Chiara/Chiara.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>

#include "LogCapture.hpp"

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
    return SystemContext{world,  0.016f, /*simTick=*/ 0, /*input=*/ nullptr,
                         /*actions=*/ nullptr, events, isActiveWorld};
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

TEST_CASE("An ordering target this world does not hold is not a complaint")
{
    // A level installs its systems one at a time, so "run after the patrol
    // system" is a reasonable thing to say in a world that has no patrol system.
    // It is no edge and nothing more — in particular it is not an error, because
    // a target no system anywhere declares cannot reach here: reflectgen refuses
    // that across the whole tree before anything compiles.
    SystemRegistry systems;
    Assisi::ECS::Scene scene;
    std::vector<std::string> order;

    systems.RegisterRender("Solo",
                           [&order](RenderContext &) { order.emplace_back("Solo"); })
    .After("DeclaredElsewhereButNotInstalledHere");

    const Assisi::Tests::LogCapture log;
    systems.RunRender(MakeCtx(scene));

    CHECK(order.size() == 1); // runs, unordered
    CHECK_FALSE(log.Mentions("DeclaredElsewhereButNotInstalledHere"));
}

TEST_CASE("An ordering target installed later takes effect from then on")
{
    // The other half of treating an absent target as ordinary: the constraint is
    // not discarded, it is simply unsatisfiable for now. Installing the named
    // system re-sorts the phase with the edge in place, so a system that arrives
    // late still runs in the order the declaration asked for.
    SystemRegistry systems;
    Assisi::ECS::Scene scene;
    std::vector<std::string> order;

    systems.RegisterRender("Late", [&order](RenderContext &) { order.emplace_back("Late"); })
    .After("Early");

    systems.RunRender(MakeCtx(scene));
    REQUIRE(order == std::vector<std::string>{"Late"});

    // Now the target arrives — registered second, so registration order alone
    // would still put it last.
    order.clear();
    systems.RegisterRender("Early", [&order](RenderContext &) { order.emplace_back("Early"); });

    systems.RunRender(MakeCtx(scene));
    CHECK(order == std::vector<std::string>{"Early", "Late"});
}

TEST_CASE("SystemRegistry: game phases run headlessly, with the world in the context")
{
    // No window, so no InputContext — a dedicated server and these tests share
    // that shape. Systems reach entities through ctx.world.scene.
    WorldManager worlds;
    World &world = worlds.Create("Test");
    Assisi::Core::EventQueue events;
    SystemRegistry systems;

    const Assisi::ECS::Entity entity = world.scene.Create();

    World *seenWorld  = nullptr;
    std::size_t seenAlive  = 0;
    systems.Register(SystemPhase::Update, "Inspect",
                     [&](SystemContext &ctx)
    {
        seenWorld = &ctx.world;
        seenAlive = ctx.world.scene.IsAlive(entity) ? 1u : 0u;
        CHECK(ctx.input == nullptr);
        CHECK(ctx.ui == nullptr);
    });

    systems.Run(SystemPhase::Update, MakeGameCtx(world, events, /*isActiveWorld=*/ true));

    CHECK(seenWorld == &world);
    CHECK(seenAlive == 1u);
}

TEST_CASE("SystemRegistry: every phase is a separate list, and PostFixedUpdate is its own")
{
    // PostFixedUpdate exists because ordering cannot cross the physics step:
    // `after`/`before` arrange systems within one phase, and the step runs
    // between FixedUpdate and this one. A registry that folded the two together
    // would run a post-step system before the step and nothing would say so.
    WorldManager worlds;
    World &world = worlds.Create("Test");
    Assisi::Core::EventQueue events;
    SystemRegistry systems;

    std::vector<std::string> order;
    const auto record = [&order](std::string name)
    {
        return [&order, name = std::move(name)](SystemContext &) { order.push_back(name); };
    };

    systems.Register(SystemPhase::FixedUpdate, "BeforeStep", record("BeforeStep"));
    systems.Register(SystemPhase::PostFixedUpdate, "AfterStep", record("AfterStep"));

    // Running one phase runs only that phase's systems, whatever the other holds.
    systems.Run(SystemPhase::FixedUpdate, MakeGameCtx(world, events, /*isActiveWorld=*/ true));
    REQUIRE(order.size() == 1u);
    CHECK(order[0] == "BeforeStep");

    systems.Run(SystemPhase::PostFixedUpdate, MakeGameCtx(world, events, /*isActiveWorld=*/ true));
    REQUIRE(order.size() == 2u);
    CHECK(order[1] == "AfterStep");
}

TEST_CASE("SystemRegistry: ActiveWorldOnly systems run only in the active world")
{
    // One InputContext, N resident worlds: a controller system must not apply the
    // same keypresses in every world.
    WorldManager worlds;
    World &world = worlds.Create("Test");
    Assisi::Core::EventQueue events;
    SystemRegistry systems;

    int32_t everywhere = 0;
    int32_t activeOnly = 0;

    systems.Register(SystemPhase::Update, "Everywhere", [&](SystemContext &) { ++everywhere; });
    systems.Register(SystemPhase::Update, "ActiveOnly", [&](SystemContext &) { ++activeOnly; })
    .ActiveWorldOnly();

    systems.Run(SystemPhase::Update, MakeGameCtx(world, events, /*isActiveWorld=*/ true));
    CHECK(everywhere == 1);
    CHECK(activeOnly == 1);

    systems.Run(SystemPhase::Update, MakeGameCtx(world, events, /*isActiveWorld=*/ false));
    CHECK(everywhere == 2); // world-agnostic system still ticks
    CHECK(activeOnly == 1); // ...the input-consuming one does not
}

TEST_CASE("SystemRegistry: skipping an ActiveWorldOnly system preserves the order of the rest")
{
    // The gate is a dispatch-time skip, not a re-sort: the surviving systems must
    // keep the order their After()/Before() constraints define.
    WorldManager worlds;
    World &world = worlds.Create("Test");
    Assisi::Core::EventQueue events;
    SystemRegistry systems;
    std::vector<std::string> order;

    auto record = [&order](const char *name)
                  { return [&order, name](SystemContext &) { order.emplace_back(name); }; };

    systems.Register(SystemPhase::Update, "Last", record("Last")).After("Middle");
    systems.Register(SystemPhase::Update, "Middle", record("Middle")).After("First");
    systems.Register(SystemPhase::Update, "First", record("First"));
    // The gated system sits in the middle of the chain by registration, but
    // declares no constraints — its absence must not disturb the others.
    systems.Register(SystemPhase::Update, "Gated", record("Gated")).ActiveWorldOnly();

    systems.Run(SystemPhase::Update, MakeGameCtx(world, events, /*isActiveWorld=*/ false));

    REQUIRE(order.size() == 3);
    CHECK(order[0] == "First");
    CHECK(order[1] == "Middle");
    CHECK(order[2] == "Last");
}

TEST_CASE("SystemRegistry: RequireAny skips a system until its components exist")
{
    // What lets a level name systems a world may never need: an open-world level
    // names everything, and residency decides what runs. The gate has to open and
    // close with the data, not just once at startup.
    WorldManager worlds;
    World &world = worlds.Create("Test");
    Assisi::Core::EventQueue events;
    SystemRegistry systems;

    int32_t gated   = 0;
    int32_t ungated = 0;
    systems.Register(SystemPhase::Update, "Ungated", [&](SystemContext &) { ++ungated; });
    systems.Register(SystemPhase::Update, "Gated", [&](SystemContext &) { ++gated; })
    .RequireAny<Assisi::ECS::Transform>();

    // Empty scene: the gated system has nothing to work on.
    systems.Run(SystemPhase::Update, MakeGameCtx(world, events, true));
    CHECK(ungated == 1);
    CHECK(gated == 0);

    // One matching entity is enough to open it.
    const Assisi::ECS::Entity entity = world.scene.Create();
    (void)world.scene.Add<Assisi::ECS::Transform>(entity);
    systems.Run(SystemPhase::Update, MakeGameCtx(world, events, true));
    CHECK(ungated == 2);
    CHECK(gated == 1);

    // ...and removing the last one closes it again.
    world.scene.Remove<Assisi::ECS::Transform>(entity);
    systems.Run(SystemPhase::Update, MakeGameCtx(world, events, true));
    CHECK(ungated == 3);
    CHECK(gated == 1);
}

TEST_CASE("SystemRegistry: RequireAny runs when ANY of the listed components is present")
{
    WorldManager worlds;
    World &world = worlds.Create("Test");
    Assisi::Core::EventQueue events;
    SystemRegistry systems;

    int32_t ticks = 0;
    systems.Register(SystemPhase::Update, "EitherOr", [&](SystemContext &) { ++ticks; })
    .RequireAny<Assisi::ECS::Transform, Assisi::Physics::RigidBodyDescriptor>();

    systems.Run(SystemPhase::Update, MakeGameCtx(world, events, true));
    CHECK(ticks == 0);

    // Only the SECOND of the two listed components exists — still eligible.
    const Assisi::ECS::Entity entity = world.scene.Create();
    (void)world.scene.Add<Assisi::Physics::RigidBodyDescriptor>(entity);
    systems.Run(SystemPhase::Update, MakeGameCtx(world, events, true));
    CHECK(ticks == 1);
}

TEST_CASE("SystemRegistry: the activation gate is per world, not per registry")
{
    // Two worlds built from one system list share the system *set*; whether each
    // one runs it is decided by that world's own contents.
    WorldManager worlds;
    World &withData = worlds.Create("WithData");
    World &empty    = worlds.Create("Empty");
    Assisi::Core::EventQueue events;
    SystemRegistry systems;

    int32_t ticks = 0;
    systems.Register(SystemPhase::Update, "Gated", [&](SystemContext &) { ++ticks; })
    .RequireAny<Assisi::ECS::Transform>();

    (void)withData.scene.Add<Assisi::ECS::Transform>(withData.scene.Create());

    systems.Run(SystemPhase::Update, MakeGameCtx(empty, events, true));
    CHECK(ticks == 0);
    systems.Run(SystemPhase::Update, MakeGameCtx(withData, events, true));
    CHECK(ticks == 1);
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

#if defined(ASSISI_CHIARA_ENABLED)

// RunPhase is the chokepoint the whole "instrumentation feels automatic" claim
// rests on: one scope there and one per entry means every system ever written is
// profiled with no further work. If that ever silently stops happening, coverage
// quietly rots everywhere at once, so it is worth a test rather than a comment.
TEST_CASE("RunPhase profiles the phase and every system it runs")
{
    Assisi::Chiara::Config config;
    config.mainThreadBufferBytes  = 1u << 20;
    config.otherThreadBufferBytes = 1u << 16;
    Assisi::Chiara::Initialize(config);

    Assisi::ECS::Scene scene;
    SystemRegistry systems;
    systems.RegisterRender("first-render-system", [](RenderContext &) {});
    systems.RegisterRender("second-render-system", [](RenderContext &) {});

    // Mark where this case starts so earlier cases' records are excluded.
    std::uint64_t mark = 0;
    for (const Assisi::Chiara::ThreadSnapshot &snapshot : Assisi::Chiara::SnapshotThreads())
    {
        if (snapshot.isMain)
        {
            mark = snapshot.endIndex;
        }
    }

    systems.RunRender(MakeCtx(scene));

    std::vector<std::string> scopeNames;
    for (const Assisi::Chiara::ThreadSnapshot &snapshot : Assisi::Chiara::SnapshotThreads())
    {
        if (!snapshot.isMain || snapshot.ring == nullptr)
        {
            continue;
        }
        for (std::uint64_t index = std::max(mark, snapshot.beginIndex); index < snapshot.endIndex; ++index)
        {
            const Assisi::Chiara::Event &event = snapshot.ring->At(index);
            if (event.type == Assisi::Chiara::EventType::Scope && event.name != nullptr)
            {
                scopeNames.emplace_back(event.name);
            }
        }
    }

    const auto contains = [&scopeNames](std::string_view wanted)
                          { return std::ranges::find(scopeNames, wanted) != scopeNames.end(); };

    CHECK(contains("Render"));
    CHECK(contains("first-render-system"));
    CHECK(contains("second-render-system"));
}

TEST_CASE("A system's scope name survives the entry vector reallocating")
{
    // Names are interned at registration precisely because Entry lives in a
    // vector: holding name.c_str() would leave every earlier system's scope
    // pointing at freed memory as soon as another one is added.
    Assisi::Chiara::Initialize();

    SystemRegistry systems;
    systems.RegisterRender("survivor", [](RenderContext &) {});
    const char *internedBefore = Assisi::Chiara::InternString("survivor");

    for (int32_t i = 0; i < 64; ++i)
    {
        systems.RegisterRender("filler-" + std::to_string(i), [](RenderContext &) {});
    }

    // Interning is idempotent, so the pointer the first entry captured is still
    // the one that names it — regardless of how far the vector has moved.
    CHECK(Assisi::Chiara::InternString("survivor") == internedBefore);

    Assisi::ECS::Scene scene;
    systems.RunRender(MakeCtx(scene)); // Would read freed memory if this regressed.
}

#endif // ASSISI_CHIARA_ENABLED
