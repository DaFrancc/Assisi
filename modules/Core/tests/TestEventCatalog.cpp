/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Core/EventCatalog.hpp>
#include <Assisi/Core/EventQueue.hpp>

#include <cstdint>
#include <string>

using namespace Assisi::Core;

namespace
{

struct ResumeClicked
{
    int32_t marker = 0;
};

struct QuitRequested
{
};

struct Rested
{
    float value = 0.f;
};

struct Flipped
{
    bool value = false;
};

struct Picked
{
    int32_t value = 0;
};

struct Named
{
    std::string value;
};

/// A catalog holding the two above, built by hand. The generated registrations
/// fill the process-wide one; a test that used that would depend on what the
/// link happened to contain.
EventCatalog TwoEvents()
{
    EventCatalog catalog;
    catalog.Register(Assisi::Core::DefineEvent<ResumeClicked>("Game::ResumeClicked"));
    catalog.Register(Assisi::Core::DefineEvent<QuitRequested>("Game::QuitRequested"));
    return catalog;
}

} // namespace

TEST_CASE("EventCatalog: a found entry pushes its own type")
{
    const EventCatalog catalog = TwoEvents();
    EventQueue events;
    EventQueue universal{EventScope::Universal};

    const EventDefinition *const entry = catalog.Find("Game::ResumeClicked");
    REQUIRE(entry != nullptr);
    entry->push(EventRoute{events, universal});

    // The name resolved to one type and not the other, which is the whole claim:
    // a string became a typed push nothing along the way had to know about.
    CHECK(events.Read<ResumeClicked>().size() == 1);
    CHECK(events.Read<QuitRequested>().empty());
}

TEST_CASE("EventCatalog: a name nothing declared is not found")
{
    const EventCatalog catalog = TwoEvents();

    // The case a cook and a load both turn into a refusal. Returning null rather
    // than an empty push matters: a push that did nothing would be a button that
    // silently does nothing.
    CHECK(catalog.Find("Game::Misspelt") == nullptr);
    CHECK(catalog.Find("ResumeClicked") == nullptr); // unqualified is a different name
}

TEST_CASE("EventCatalog: the first registration of a name wins")
{
    EventCatalog catalog;
    catalog.Register({
        .name = "Game::ResumeClicked",
        .push = [](const EventRoute &events) { events.Push(ResumeClicked{.marker = 1}); },
        .pushValue = {},
        .valueKind = EventValueKind::None,
    });
    catalog.Register({
        .name = "Game::ResumeClicked",
        .push = [](const EventRoute &events) { events.Push(ResumeClicked{.marker = 2}); },
        .pushValue = {},
        .valueKind = EventValueKind::None,
    });

    EventQueue events;
    EventQueue universal{EventScope::Universal};
    catalog.Find("Game::ResumeClicked")->push(EventRoute{events, universal});

    // Which one wins would otherwise be link order. It is refused rather than
    // replaced so that a duplicate cannot change what an existing file pushes.
    REQUIRE(events.Read<ResumeClicked>().size() == 1);
    CHECK(events.Read<ResumeClicked>()[0].marker == 1);
}

TEST_CASE("EventCatalog: every entry is listed, for naming what is available")
{
    const EventCatalog catalog = TwoEvents();

    REQUIRE(catalog.All().size() == 2);
    CHECK(catalog.All()[0].name == "Game::ResumeClicked");
    CHECK(catalog.All()[1].name == "Game::QuitRequested");
}

TEST_CASE("EventCatalog: an event with a value field takes what a control hands it")
{
    const EventDefinition rested = DefineEvent<Rested>("Game::Rested");
    CHECK(rested.valueKind == EventValueKind::Float);
    CHECK(DefineEvent<Flipped>("Game::Flipped").valueKind == EventValueKind::Bool);
    CHECK(DefineEvent<Picked>("Game::Picked").valueKind == EventValueKind::Int32);

    EventQueue events;
    EventQueue universal{EventScope::Universal};
    const EventRoute route{events, universal};
    REQUIRE(rested.pushValue);
    rested.pushValue(route, 0.5f);
    REQUIRE(events.Read<Rested>().size() == 1);
    CHECK(events.Read<Rested>()[0].value == doctest::Approx(0.5f));

    // Pushed by name, it is default-constructed as any other is.
    rested.push(route);
    REQUIRE(events.Read<Rested>().size() == 2);
    CHECK(events.Read<Rested>()[1].value == doctest::Approx(0.f));
}

TEST_CASE("EventCatalog: an event no control can fill says so, and one with no value field is plain")
{
    const EventDefinition named = DefineEvent<Named>("Game::Named");
    CHECK(named.valueKind == EventValueKind::Other);
    CHECK_FALSE(named.pushValue);

    const EventDefinition plain = DefineEvent<QuitRequested>("Game::QuitRequested");
    CHECK(plain.valueKind == EventValueKind::None);
    CHECK_FALSE(plain.pushValue);
    CHECK(plain.push);
}
