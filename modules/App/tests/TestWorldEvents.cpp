/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestWorldEvents.cpp
/// @brief A world's events reach only that world's systems, and a universal
///        event reaches every world's, whether a system or a screen pushed it.
///
/// Two worlds under one manager can hold entities with the same id, so an event
/// that crossed from one to the other would act on the wrong entity. Both
/// worlds here are stepped in one frame before anything is flushed, as a host
/// steps them, so an event that leaked would be read.

#include <doctest/doctest.h>

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/App/WorldFrame.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/EventScope.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Mondrian/DrawList.hpp>
#include <Assisi/Mondrian/Input.hpp>
#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/Ui.hpp>
#include <Assisi/Mondrian/Widget.hpp>
#include <Assisi/Testing/ThrowOnContractViolation.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

using namespace Assisi::App;
namespace Core = Assisi::Core;
namespace Mondrian = Assisi::Mondrian;

namespace
{

constexpr float kStep = 1.f / 60.f;

/// What the UI is laid out against; any size will do, nothing here is placed.
constexpr Mondrian::Extent kViewport{1280, 720};

/// About one world, as most events are, so only that world's systems hear it.
struct Ping
{
    int32_t from = 0;
};

/// What one world's systems read in the frames so far.
struct Heard
{
    std::size_t pings = 0;
    std::size_t quits = 0;
};

void Record(SystemContext &ctx, Heard &heard)
{
    heard.pings += ctx.events.Read<Ping>().size();
    heard.quits += ctx.events.Read<QuitRequested>().size();
}

/// Has @p world's systems note everything they read into @p heard, after any
/// system that could have pushed it.
void Listen(World &world, Heard &heard)
{
    world.systems.Register(SystemPhase::Ui, "Listen", [&heard](SystemContext &ctx) { Record(ctx, heard); });
}

/// Two worlds and the UI they are shown in, sharing the program's universal
/// queue as a host's worlds do.
struct TwoWorlds
{
    Core::EventQueue universal{Core::EventScope::Universal};
    /// Before the worlds, so it outlives the screens they own.
    Mondrian::Ui ui{universal};
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &first = worlds.Create("First");
    World &second = worlds.Create("Second");
    Heard firstHeard;
    Heard secondHeard;

    TwoWorlds()
    {
        Listen(first, firstHeard);
        Listen(second, secondHeard);
    }

    SystemContext Context(World &world)
    {
        return {.world = world,
                .dt = kStep,
                .simTick = 0,
                .input = nullptr,
                .actions = nullptr,
                .universalEvents = universal};
    }

    /// One frame: each world's per-frame phases in turn, then the end-of-frame
    /// flush of every queue.
    void RunFrame()
    {
        for (World *const world : {&first, &second})
        {
            UpdateWorld(Context(*world));
            PresentWorld(Context(*world));
        }
        first.events.Flush();
        second.events.Flush();
        universal.Flush();
    }

    /// One UI frame with @p input, as the host runs it before the worlds.
    void UiFrame(const Mondrian::UiInput &input)
    {
        (void)ui.ProcessInput(input);
        ui.Sync(kViewport);
    }
};

Mondrian::UiInput Accept()
{
    Mondrian::UiInput input;
    input.grant = Mondrian::InputGrant::Everything;
    input.actionPressed[static_cast<std::size_t>(Mondrian::UiAction::Accept)] = true;
    input.actionDown[static_cast<std::size_t>(Mondrian::UiAction::Accept)] = true;
    return input;
}

} // namespace

TEST_CASE("An event a system pushes in one world is read by that world's systems and not another's")
{
    TwoWorlds worlds;
    worlds.first.systems.Register(SystemPhase::Update, "Ping",
                                  [](SystemContext &ctx) { ctx.events.Push(Ping{.from = 1}); });

    worlds.RunFrame();

    CHECK(worlds.firstHeard.pings == 1);
    CHECK(worlds.secondHeard.pings == 0);
}

TEST_CASE("A universal event a system pushes in one world is read by every world's systems")
{
    // QuitRequested is the engine's own AEVENT(universal): that it reaches the
    // second world is the generated registration having said so.
    TwoWorlds worlds;
    worlds.first.systems.Register(SystemPhase::Update, "Quit",
                                  [](SystemContext &ctx) { ctx.events.Push(QuitRequested{}); });

    worlds.RunFrame();

    CHECK(worlds.firstHeard.quits == 1);
    CHECK(worlds.secondHeard.quits == 1);
    CHECK(worlds.universal.Read<QuitRequested>().empty()); // flushed with the frame
}

TEST_CASE("A button's world event reaches only its screen's world, and a universal one every world")
{
    TwoWorlds worlds;
    Mondrian::Screen &screen = AddScreen(
        worlds.first,
        std::make_unique<Mondrian::Screen>(worlds.ui, worlds.first.screenWorld,
                                           Mondrian::ScreenTraits{.input = Mondrian::ScreenInput::ConsumeInput},
                                           Mondrian::kSortMenu, std::string{"menu"}),
        {});
    const Mondrian::ButtonId ping = screen.AddButton(screen.Root(), "Ping");
    const Mondrian::ButtonId quit = screen.AddButton(screen.Root(), "Quit");
    screen.OnActivate(ping.node, Ping{.from = 1});
    screen.OnActivate(quit.node, QuitRequested{});
    screen.Show();
    worlds.UiFrame({});

    worlds.ui.SetFocus(screen, ping.node);
    worlds.UiFrame(Accept());
    worlds.RunFrame();
    CHECK(worlds.firstHeard.pings == 1);
    CHECK(worlds.secondHeard.pings == 0);

    worlds.ui.SetFocus(screen, quit.node);
    worlds.UiFrame(Accept());
    worlds.RunFrame();
    CHECK(worlds.firstHeard.quits == 1);
    CHECK(worlds.secondHeard.quits == 1);
}

#ifndef NDEBUG
TEST_CASE("A world refuses a screen built for another world")
{
    // Its buttons would be heard by the other world's systems while it is shown
    // over this one.
    const Assisi::Testing::ThrowOnContractViolation guard;
    TwoWorlds worlds;
    std::unique_ptr<Mondrian::Screen> screen = std::make_unique<Mondrian::Screen>(
        worlds.ui, worlds.second.screenWorld, Mondrian::ScreenTraits{}, Mondrian::kSortMenu, std::string{"menu"});

    CHECK_THROWS_AS(AddScreen(worlds.first, std::move(screen), {}), Core::ContractViolation);
}
#endif // !NDEBUG

TEST_CASE("A screen's context is lost when its world destroys the entity, and only that world hears it")
{
    TwoWorlds worlds;
    const Assisi::ECS::Entity boss = worlds.first.scene.Create();
    // The same id, alive in the other world: the context is the first world's.
    REQUIRE(worlds.second.scene.Create() == boss);

    Mondrian::Screen &screen =
        AddScreen(worlds.first,
                  std::make_unique<Mondrian::Screen>(worlds.ui, worlds.first.screenWorld, Mondrian::ScreenTraits{},
                                                     Mondrian::kSortHud, std::string{"hud"}),
                  {});
    screen.OnContext("boss", Mondrian::ContextMoment::Lost, Ping{.from = 1});
    screen.SetContext("boss", ScreenEntity(boss));
    screen.Show();
    worlds.UiFrame({});
    worlds.RunFrame();

    // Alive until the flush, for the screen as for any system.
    worlds.first.scene.Destroy(boss);
    worlds.UiFrame({});
    worlds.RunFrame();
    CHECK(worlds.firstHeard.pings == 0);

    worlds.first.scene.FlushDestroyed();
    worlds.UiFrame({});
    worlds.RunFrame();
    CHECK(worlds.firstHeard.pings == 1);
    CHECK(worlds.secondHeard.pings == 0);
    CHECK_FALSE(screen.GetContext("boss"));
}

TEST_CASE("A world gives each screen an id that finds it, and never gives that id again")
{
    TwoWorlds worlds;
    Mondrian::Screen &first = AddScreen(
        worlds.first,
        std::make_unique<Mondrian::Screen>(worlds.ui, worlds.first.screenWorld, Mondrian::ScreenTraits{},
                                           Mondrian::kSortHud, std::string{"hud"}),
        {});
    const ScreenId firstId = IdOf(worlds.first, first);
    REQUIRE(firstId);
    CHECK(FindScreen(worlds.first, firstId) == &first);
    CHECK_FALSE(IdOf(worlds.second, first)); // not that world's

    RemoveScreen(worlds.first, first);
    CHECK(FindScreen(worlds.first, firstId) == nullptr);

    // The same file again is another screen: what kept the first id finds
    // nothing rather than finding this one.
    Mondrian::Screen &second = AddScreen(
        worlds.first,
        std::make_unique<Mondrian::Screen>(worlds.ui, worlds.first.screenWorld, Mondrian::ScreenTraits{},
                                           Mondrian::kSortHud, std::string{"hud"}),
        {});
    CHECK(IdOf(worlds.first, second) != firstId);
    CHECK(FindScreen(worlds.first, firstId) == nullptr);
}

TEST_CASE("A screen's binding reads its context's component through its world's scene")
{
    TwoWorlds worlds;
    const Assisi::ECS::Entity crate = worlds.first.scene.Create();
    (void)worlds.first.scene.Add(crate, Assisi::ECS::Transform{.position = {1.f, 2.f, 3.f}});

    Mondrian::Screen &screen =
        AddScreen(worlds.first,
                  std::make_unique<Mondrian::Screen>(worlds.ui, worlds.first.screenWorld, Mondrian::ScreenTraits{},
                                                     Mondrian::kSortHud, std::string{"hud"}),
                  {});
    const Mondrian::NodeId text = screen.AddText(screen.Root(), Mondrian::Style{}, "");
    REQUIRE(screen.BindText(text, "{Transform.position:.0f}").has_value());
    screen.SetContext(ScreenEntity(crate));
    screen.Show();

    worlds.UiFrame({});
    CHECK(screen.Tree().Get(text)->text == "1, 2, 3");

    worlds.first.scene.GetMut<Assisi::ECS::Transform>(crate)->position.x = 7.f;
    worlds.UiFrame({});
    CHECK(screen.Tree().Get(text)->text == "7, 2, 3");
}

TEST_CASE("A world destroyed under a shown screen bound to its player takes the screen with it")
{
    // Travel's last step: the old level goes while its HUD is up and reading its
    // player. Under the asan preset, the UI touching either afterwards fails.
    TwoWorlds worlds;
    const Assisi::ECS::Entity player = worlds.first.scene.Create();
    Mondrian::Screen &hud = AddScreen(
        worlds.first,
        std::make_unique<Mondrian::Screen>(worlds.ui, worlds.first.screenWorld,
                                           Mondrian::ScreenTraits{.input = Mondrian::ScreenInput::ConsumeInput},
                                           Mondrian::kSortHud, std::string{"hud"}),
        {});
    hud.OnContext({}, Mondrian::ContextMoment::Lost, Ping{.from = 1});
    hud.SetContext(ScreenEntity(player));
    hud.Show();
    worlds.UiFrame({});
    REQUIRE(worlds.ui.TakesInput());

    worlds.worlds.SetActive(worlds.second);
    const std::string travelledFrom = worlds.first.name;
    REQUIRE(worlds.worlds.Destroy(travelledFrom));

    worlds.UiFrame({});
    worlds.UiFrame({});
    CHECK_FALSE(worlds.ui.TakesInput());
    CHECK(worlds.ui.GetDrawList().Instances().empty());
}
