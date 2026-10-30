/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/TestWorld.hpp>
#include <Assisi/Mondrian/Ui.hpp>

#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/EventScope.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>

using namespace Assisi::Mondrian;
using Assisi::Core::EventQueue;

namespace
{

constexpr Extent kViewport{1280, 720};

constexpr EntityId kBoss{.index = 4, .generation = 1};
constexpr EntityId kNextBoss{.index = 9, .generation = 2};
/// The boss's slot after it died, taken by something else.
constexpr EntityId kBossSlotReused{.index = 4, .generation = 2};

struct Appeared
{
};

struct Switched
{
};

struct Cleared
{
};

struct Defeated
{
};

/// A HUD whose boss context announces every moment, in a world where the
/// test decides who is alive.
struct Hud
{
    EventQueue events;
    TestWorld world{events};
    EventQueue universal{Assisi::Core::EventScope::Universal};
    Ui ui{universal};
    Screen screen{ui, world, ScreenTraits{}, kSortHud, "hud"};

    Hud()
    {
        screen.OnContext("boss", ContextMoment::Set, Appeared{});
        screen.OnContext("boss", ContextMoment::Switch, Switched{});
        screen.OnContext("boss", ContextMoment::Clear, Cleared{});
        screen.OnContext("boss", ContextMoment::Lost, Defeated{});
        screen.Show();
    }

    /// One frame, starting with a fresh queue as the host's would.
    void Frame()
    {
        events.Flush();
        ui.ProcessInput({});
        ui.Sync(kViewport);
    }

    /// Kills @p entity: the world no longer has it.
    void Kill(EntityId entity) { std::erase(world.alive, entity); }

    /// How many events the last frame pushed, of every kind.
    [[nodiscard]] std::size_t Heard() const
    {
        return events.Read<Appeared>().size() + events.Read<Switched>().size() + events.Read<Cleared>().size() +
               events.Read<Defeated>().size();
    }
};

} // namespace

TEST_CASE("Contexts: each moment is announced once, at the input step after it happens")
{
    Hud hud;
    hud.world.alive = {kBoss, kNextBoss};

    SUBCASE("set")
    {
        hud.screen.SetContext("boss", kBoss);
        hud.Frame();
        CHECK(hud.events.Read<Appeared>().size() == 1);
        CHECK(hud.Heard() == 1);

        // Nothing changed since, so nothing more is said.
        hud.Frame();
        CHECK(hud.Heard() == 0);
    }

    SUBCASE("switch")
    {
        hud.screen.SetContext("boss", kBoss);
        hud.Frame();
        hud.screen.SetContext("boss", kNextBoss);
        hud.Frame();
        CHECK(hud.events.Read<Switched>().size() == 1);
        CHECK(hud.Heard() == 1);
    }

    SUBCASE("clear")
    {
        hud.screen.SetContext("boss", kBoss);
        hud.Frame();
        hud.screen.SetContext("boss", EntityId{});
        hud.Frame();
        CHECK(hud.events.Read<Cleared>().size() == 1);
        CHECK(hud.Heard() == 1);
    }

    SUBCASE("lost")
    {
        hud.screen.SetContext("boss", kBoss);
        hud.Frame();
        hud.Kill(kBoss);
        hud.Frame();
        CHECK(hud.events.Read<Defeated>().size() == 1);
        CHECK(hud.Heard() == 1);
        CHECK_FALSE(hud.screen.GetContext("boss"));
    }
}

TEST_CASE("Contexts: an entity whose slot was taken again is lost, not kept")
{
    // The id names one life of a slot; whatever has the slot now is not the boss.
    Hud hud;
    hud.world.alive = {kBoss};
    hud.screen.SetContext("boss", kBoss);
    hud.Frame();

    hud.world.alive = {kBossSlotReused};
    hud.Frame();
    CHECK(hud.events.Read<Defeated>().size() == 1);
}

TEST_CASE("Contexts: a lost context counts as empty, so the next entity is announced as the first was")
{
    Hud hud;
    hud.world.alive = {kBoss, kNextBoss};
    hud.screen.SetContext("boss", kBoss);
    hud.Frame();
    hud.Kill(kBoss);
    hud.Frame();

    hud.screen.SetContext("boss", kNextBoss);
    hud.Frame();
    CHECK(hud.events.Read<Appeared>().size() == 1);
    CHECK(hud.events.Read<Switched>().empty());
}

TEST_CASE("Contexts: what changes and changes back between two input steps announces nothing")
{
    Hud hud;
    hud.world.alive = {kBoss, kNextBoss};

    SUBCASE("set and cleared")
    {
        hud.screen.SetContext("boss", kBoss);
        hud.screen.SetContext("boss", EntityId{});
        hud.Frame();
        CHECK(hud.Heard() == 0);
    }

    SUBCASE("switched away and back")
    {
        hud.screen.SetContext("boss", kBoss);
        hud.Frame();
        hud.screen.SetContext("boss", kNextBoss);
        hud.screen.SetContext("boss", kBoss);
        hud.Frame();
        CHECK(hud.Heard() == 0);
    }

    SUBCASE("given an entity already dead")
    {
        hud.Kill(kBoss);
        hud.screen.SetContext("boss", kBoss);
        hud.Frame();
        CHECK(hud.Heard() == 0);
        CHECK_FALSE(hud.screen.GetContext("boss"));
    }
}

TEST_CASE("Contexts: a context replaced by one already dead is lost")
{
    // Nobody cleared it: its entity went the moment it was given.
    Hud hud;
    hud.world.alive = {kBoss};
    hud.screen.SetContext("boss", kBoss);
    hud.Frame();
    hud.screen.SetContext("boss", kNextBoss);
    hud.Frame();
    CHECK(hud.events.Read<Defeated>().size() == 1);
    CHECK(hud.Heard() == 1);
}

TEST_CASE("Contexts: a screen's contexts answer whether it is shown or not")
{
    // A boss bar waits hidden for a boss, and hears it arrive.
    Hud hud;
    hud.world.alive = {kBoss};
    hud.screen.Hide();
    hud.screen.SetContext("boss", kBoss);
    hud.Frame();
    CHECK(hud.events.Read<Appeared>().size() == 1);
}

TEST_CASE("Contexts: the unnamed context and a named one are separate")
{
    Hud hud;
    hud.world.alive = {kBoss};
    hud.screen.SetContext(kBoss);
    hud.Frame();
    // The boss context is still empty and has said nothing.
    CHECK(hud.Heard() == 0);
    CHECK(hud.screen.GetContext() == kBoss);
    CHECK_FALSE(hud.screen.GetContext("boss"));
}

TEST_CASE("Contexts: an act runs on the screen, so a screen can hide itself when its entity goes")
{
    Hud hud;
    hud.world.alive = {kBoss};
    hud.screen.OnContext("boss", ContextMoment::Lost, [](Screen &self) { self.Hide(); });
    hud.screen.SetContext("boss", kBoss);
    hud.Frame();
    REQUIRE(hud.screen.IsShown());

    hud.Kill(kBoss);
    hud.Frame();
    CHECK_FALSE(hud.screen.IsShown());
}

TEST_CASE("Contexts: a reaction that sets a context is answered at the next input step")
{
    // Answered from a list taken before any reaction runs, so a reaction that
    // makes a context does not grow the list being walked.
    Hud hud;
    hud.world.alive = {kBoss, kNextBoss};
    hud.screen.OnContext("boss", ContextMoment::Set, [](Screen &self) { self.SetContext("minion", kNextBoss); });
    hud.screen.OnContext("minion", ContextMoment::Set, Appeared{});
    hud.screen.SetContext("boss", kBoss);
    hud.Frame();
    CHECK(hud.events.Read<Appeared>().empty());

    hud.Frame();
    CHECK(hud.events.Read<Appeared>().size() == 1);
}
