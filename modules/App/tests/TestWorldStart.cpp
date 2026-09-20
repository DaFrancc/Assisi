/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestWorldStart.cpp
/// @brief The two one-shot phases: that they fire once, at the right moment, and
///        that the config decides when the clock starts.
///
/// What makes these worth asserting rather than reading: a phase that fires
/// twice and a phase that fires once leave identical worlds behind. Only a tally
/// separates them, and the bug this phase exists to remove — level-start logic
/// running at some later unrelated moment — is invisible in the scene.

#include <doctest/doctest.h>

#include <Assisi/App/AppConfig.hpp>
#include <Assisi/App/SystemCatalog.hpp>
#include <Assisi/App/TestStartContext.hpp>
#include <Assisi/App/TestSystems.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/Ui.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace Assisi::App;
using Assisi::App::Test::RunCounts;
using Assisi::App::Test::RunOrder;
using Assisi::App::Test::StartContext;

namespace
{

/// A root holding levels written by the case that mounts it.
std::filesystem::path MountTestRoot()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "assisi-world-start";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "levels");
    REQUIRE(Assisi::Core::AssetSystem::SetRoot(root).has_value());
    return root;
}

/// A level naming @p systems and holding one entity, so the load has something
/// to deserialize.
void WriteLevel(const std::filesystem::path &root, const char *name, const std::vector<std::string> &systems)
{
    Assisi::ECS::Scene scene;
    (void)scene.Add<Assisi::ECS::Transform>(scene.Create());
    Assisi::Runtime::LevelHeader header;
    header.systems = systems;
    REQUIRE(Assisi::Runtime::SceneSerializer::SaveToFile(scene, root / "levels" / name, header));
}

/// A manager with no render services (the scene+physics path) but with the event
/// queue a starting world needs, which is what the hosts install.
void InstallServices(WorldManager &worlds, Assisi::Core::EventQueue &events)
{
    worlds.SetServices({.cache = nullptr, .renderer = nullptr, .jobs = nullptr, .events = &events});
}

std::uint32_t Runs(const World &world, const char *system)
{
    return RunCounts::Instance().Count(world, system);
}

} // namespace

TEST_CASE("A loaded level begins once, before it has stepped")
{
    // The whole claim of the phase: Begin runs off the load, not off the first
    // tick. A system keyed to stepping would show zero here.
    const std::filesystem::path root = MountTestRoot();
    WriteLevel(root, "A.alvl", {"Started"});
    RunCounts::Instance().Reset();

    Assisi::Core::EventQueue events;
    WorldManager worlds;
    InstallServices(worlds, events);

    World *const world = worlds.LoadLevel("levels/A.alvl");
    REQUIRE(world != nullptr);
    CHECK(Runs(*world, "Started") == 1u);
    CHECK(world->start == StartProgress::Begun);

    std::filesystem::remove_all(root);
}

TEST_CASE("Begin does not fire a second time, however often the world is settled")
{
    const std::filesystem::path root = MountTestRoot();
    WriteLevel(root, "A.alvl", {"Started"});
    RunCounts::Instance().Reset();

    Assisi::Core::EventQueue events;
    WorldManager worlds;
    InstallServices(worlds, events);

    World *const world = worlds.LoadLevel("levels/A.alvl");
    REQUIRE(world != nullptr);

    SettleWorld(StartContext(*world), /*assetsPending=*/false);
    SettleWorld(StartContext(*world), /*assetsPending=*/false);
    CHECK(Runs(*world, "Started") == 1u);

    std::filesystem::remove_all(root);
}

TEST_CASE("A world settles once its assets stop pending, and not before")
{
    const std::filesystem::path root = MountTestRoot();
    WriteLevel(root, "A.alvl", {"Settled"});
    RunCounts::Instance().Reset();

    Assisi::Core::EventQueue events;
    WorldManager worlds;
    InstallServices(worlds, events);

    World *const world = worlds.LoadLevel("levels/A.alvl");
    REQUIRE(world != nullptr);

    // Still streaming: nothing settles, and the phase has not been spent.
    SettleWorld(StartContext(*world), /*assetsPending=*/true);
    CHECK(Runs(*world, "Settled") == 0u);
    CHECK(world->start == StartProgress::Begun);

    SettleWorld(StartContext(*world), /*assetsPending=*/false);
    CHECK(Runs(*world, "Settled") == 1u);
    CHECK(world->start == StartProgress::Loaded);

    std::filesystem::remove_all(root);
}

TEST_CASE("The simulate-from policy decides when the clock starts, not which phases run")
{
    // Under Loaded the world is Active and begun but frozen, so a loading screen
    // has something to cover. Both phases still run either way, which is the part
    // a developer relies on when putting logic in both.
    const std::filesystem::path root = MountTestRoot();
    WriteLevel(root, "A.alvl", {"Started", "Settled"});
    RunCounts::Instance().Reset();

    Assisi::Core::EventQueue events;
    WorldManager worlds;
    InstallServices(worlds, events);
    worlds.SetSimulateFrom(SimulateFrom::Loaded);

    World *const world = worlds.LoadLevel("levels/A.alvl");
    REQUIRE(world != nullptr);
    CHECK(Runs(*world, "Started") == 1u);
    CHECK(world->state == WorldState::Active);
    CHECK_FALSE(world->simulate); // begun, and deliberately not ticking

    SettleWorld(StartContext(*world), /*assetsPending=*/false);
    CHECK(Runs(*world, "Settled") == 1u);
    CHECK(world->simulate); // released by the settle

    std::filesystem::remove_all(root);
}

TEST_CASE("Under the Begin policy the world is simulating from the first tick")
{
    const std::filesystem::path root = MountTestRoot();
    WriteLevel(root, "A.alvl", {"Started"});
    RunCounts::Instance().Reset();

    Assisi::Core::EventQueue events;
    WorldManager worlds;
    InstallServices(worlds, events);
    worlds.SetSimulateFrom(SimulateFrom::Begin);

    World *const world = worlds.LoadLevel("levels/A.alvl");
    REQUIRE(world != nullptr);
    CHECK(world->simulate);

    std::filesystem::remove_all(root);
}

TEST_CASE("Travel begins the destination without re-running the world it left")
{
    const std::filesystem::path root = MountTestRoot();
    WriteLevel(root, "A.alvl", {"Started"});
    WriteLevel(root, "B.alvl", {"Started"});
    RunCounts::Instance().Reset();

    Assisi::Core::EventQueue events;
    WorldManager worlds;
    InstallServices(worlds, events);

    World *const inA = worlds.LoadLevel("levels/A.alvl");
    REQUIRE(inA != nullptr);
    // Held by value: travelling destroys the outgoing world, so the pointer is
    // dead by the time the assertion runs.
    const std::uint32_t startedInA = Runs(*inA, "Started");

    World *const inB = worlds.LoadLevel("levels/B.alvl");
    REQUIRE(inB != nullptr);
    CHECK(startedInA == 1u);
    CHECK(Runs(*inB, "Started") == 1u); // its own world, its own start

    std::filesystem::remove_all(root);
}

TEST_CASE("A blueprint's systems begin at the drain that installs them")
{
    // The blueprint half. The world is already begun, so the systems arriving
    // with a spawn must get the start their level's own systems already had —
    // and the ones that already ran must not run again.
    const std::filesystem::path root = MountTestRoot();
    WriteLevel(root, "A.alvl", {"Started"});
    RunCounts::Instance().Reset();

    Assisi::Core::EventQueue events;
    WorldManager worlds;
    InstallServices(worlds, events);

    World *const world = worlds.LoadLevel("levels/A.alvl");
    REQUIRE(world != nullptr);
    REQUIRE(Runs(*world, "Started") == 1u);

    QueueSystemInstall(*world, std::vector<std::string>{"StartedLate"}, "car.abp");
    DrainSystemInstalls(StartContext(*world));

    CHECK(Runs(*world, "StartedLate") == 1u); // the newcomer began
    CHECK(Runs(*world, "Started") == 1u);     // and the incumbent did not begin twice

    DrainSystemInstalls(StartContext(*world));
    CHECK(Runs(*world, "StartedLate") == 1u);

    std::filesystem::remove_all(root);
}

TEST_CASE("A screen brings the systems it declared, and takes its claim when it goes")
{
    // The screen half of what a blueprint does: a level that shows a pause menu
    // does not also have to know the menu needs the system that opens it. The
    // level below names nothing at all.
    const std::filesystem::path root = MountTestRoot();
    WriteLevel(root, "A.alvl", {});
    RunCounts::Instance().Reset();

    Assisi::Core::EventQueue events;
    WorldManager worlds;
    InstallServices(worlds, events);

    World *const world = worlds.LoadLevel("levels/A.alvl");
    REQUIRE(world != nullptr);
    REQUIRE_FALSE(world->systems.Has("StartedLate"));

    Assisi::Mondrian::Ui ui{events};
    const std::array<std::string, 1> needs{"StartedLate"};
    Assisi::Mondrian::Screen &screen =
        AddScreen(*world,
                  std::make_unique<Assisi::Mondrian::Screen>(ui, Assisi::Mondrian::ScreenTraits{},
                                                             Assisi::Mondrian::kSortMenu, std::string{"pause"}),
                  needs);

    // Queued, not installed on the spot: registering mid-walk would mutate what
    // is being iterated, so it lands at the frame's safe point.
    CHECK_FALSE(world->systems.Has("StartedLate"));
    DrainSystemInstalls(StartContext(*world));
    CHECK(world->systems.Has("StartedLate"));

    // It is counted as required, so a re-apply that names nothing keeps it.
    CHECK(RequiredSystemCounts(*world).contains("StartedLate"));
    REQUIRE(worlds.ApplySystems(*world, {}, "levels/A.alvl"));
    CHECK(world->systems.Has("StartedLate"));

    // The level never claimed it: that belongs to the screen, and writing it
    // into the file would have the level keep claiming it after the screen went.
    CHECK(world->systemNames.empty());

    RemoveScreen(*world, screen);
    CHECK_FALSE(RequiredSystemCounts(*world).contains("StartedLate"));

    std::filesystem::remove_all(root);
}

TEST_CASE("A world's screens are destroyed with it")
{
    // A pause menu is for the world it paused. Nothing has to remember to take
    // it away, which is what a screen outliving its world would need.
    Assisi::Core::EventQueue events;
    Assisi::Mondrian::Ui ui{events};
    {
        WorldManager worlds;
        World &world = worlds.Create("Doomed");
        AddScreen(world,
                  std::make_unique<Assisi::Mondrian::Screen>(ui, Assisi::Mondrian::ScreenTraits{},
                                                             Assisi::Mondrian::kSortMenu, std::string{"hud"}),
                  {});
        REQUIRE(FindScreen(world, "hud") != nullptr);
        REQUIRE_FALSE(ui.GetDrawList().IsFinalized());
    }

    // The worlds are gone; the UI is not, and has nothing left to draw.
    ui.ProcessInput({});
    ui.Sync({1280, 720});
    CHECK(ui.GetDrawList().Instances().empty());
}

TEST_CASE("A world's one-shot systems can build its screens again after its marks are cleared")
{
    // What the editor does between play sessions: a world it keeps across runs
    // has its start progress and once-marks reset so the Loaded phase runs
    // afresh. The screens that phase built have to be cleared with it, or the
    // next run builds a second set behind the first — and the first is still
    // showing whatever it was showing when the session stopped.
    const std::filesystem::path root = MountTestRoot();
    WriteLevel(root, "A.alvl", {});
    RunCounts::Instance().Reset();

    Assisi::Core::EventQueue events;
    WorldManager worlds;
    InstallServices(worlds, events);

    World *const world = worlds.LoadLevel("levels/A.alvl");
    REQUIRE(world != nullptr);

    Assisi::Mondrian::Ui ui{events};
    const auto build = [&ui, world]
    {
        AddScreen(*world,
                  std::make_unique<Assisi::Mondrian::Screen>(ui, Assisi::Mondrian::ScreenTraits{},
                                                             Assisi::Mondrian::kSortMenu, std::string{"pause"}),
                  {});
    };

    build();
    REQUIRE(world->screenStack.size() == 1);

    // Stopping a session: the marks are cleared so the phase may run again, and
    // the screens it made go with the session.
    world->systems.ClearOnceMarks();
    world->start = StartProgress::NotBegun;
    world->screenStack.clear();
    CHECK(Assisi::App::FindScreen(*world, "pause") == nullptr);

    // Running again builds one, not a second.
    build();
    CHECK(world->screenStack.size() == 1);
    CHECK(Assisi::App::FindScreen(*world, "pause") != nullptr);

    std::filesystem::remove_all(root);
}

TEST_CASE("A world that never begins runs no one-shot systems")
{
    // The editor's authored world, in miniature: resident and Active, but never
    // started. Level-start logic must not touch a scene somebody is composing.
    RunCounts::Instance().Reset();

    WorldManager worlds;
    World &world = worlds.Create("Authored");
    world.state = WorldState::Active;
    REQUIRE(worlds.ApplySystems(world, std::vector<std::string>{"Started"}, "levels/Authored.alvl"));

    CHECK(world.start == StartProgress::NotBegun);
    CHECK(Runs(world, "Started") == 0u);

    // Settling one that never began does nothing either — the guard is the
    // progress, not the asset flag.
    SettleWorld(StartContext(world), /*assetsPending=*/false);
    CHECK(Runs(world, "Started") == 0u);
    CHECK(world.start == StartProgress::NotBegun);
}

TEST_CASE("Re-applying a world's systems lets it begin again")
{
    // What makes the editor's Stop then Play run Begin a second time: Stop
    // re-applies the pre-play list, which drops the entries and their run marks.
    RunCounts::Instance().Reset();

    Assisi::Core::EventQueue events;
    WorldManager worlds;
    World &world = worlds.Create("Replayed");
    world.state = WorldState::Active;
    REQUIRE(worlds.ApplySystems(world, std::vector<std::string>{"Started"}, "levels/A.alvl"));

    BeginWorld(StartContext(world), SimulateFrom::Begin);
    CHECK(Runs(world, "Started") == 1u);

    BeginWorld(StartContext(world), SimulateFrom::Begin);
    CHECK(Runs(world, "Started") == 1u); // still spent

    REQUIRE(worlds.ApplySystems(world, std::vector<std::string>{"Started"}, "levels/A.alvl"));
    CHECK(world.start == StartProgress::NotBegun);

    BeginWorld(StartContext(world), SimulateFrom::Begin);
    CHECK(Runs(world, "Started") == 2u);
}

TEST_CASE("Clearing the run marks alone lets a world begin again")
{
    // The other half of Stop, for the path where there was nothing to restore
    // and ApplySystems never ran. Without this the world would keep its entries,
    // keep their marks, and never begin again however often Play was pressed.
    RunCounts::Instance().Reset();

    WorldManager worlds;
    World &world = worlds.Create("Remarked");
    world.state = WorldState::Active;
    REQUIRE(worlds.ApplySystems(world, std::vector<std::string>{"Started"}, "levels/A.alvl"));

    BeginWorld(StartContext(world), SimulateFrom::Begin);
    CHECK(Runs(world, "Started") == 1u);

    world.systems.ClearOnceMarks();
    world.start = StartProgress::NotBegun;

    BeginWorld(StartContext(world), SimulateFrom::Begin);
    CHECK(Runs(world, "Started") == 2u);
}

TEST_CASE("One-shot systems honour their ordering")
{
    RunCounts::Instance().Reset();
    RunOrder::Instance().Reset();

    WorldManager worlds;
    World &world = worlds.Create("Ordered");
    world.state = WorldState::Active;
    REQUIRE(worlds.ApplySystems(world, std::vector<std::string>{"StartedLate", "Started"}, "levels/Ordered.alvl"));

    BeginWorld(StartContext(world), SimulateFrom::Begin);

    // Named late-first in the file, so registration order would give the wrong
    // answer and only the sort gives the right one.
    const std::vector<std::string> &order = RunOrder::Instance().Names();
    REQUIRE(order.size() == 2u);
    CHECK(order[0] == "Started");
    CHECK(order[1] == "StartedLate");
}
