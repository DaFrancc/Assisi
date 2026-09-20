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

/// A catalog holding the two above, built by hand. The generated registrations
/// fill the process-wide one; a test that used that would depend on what the
/// link happened to contain.
EventCatalog TwoEvents()
{
    EventCatalog catalog;
    catalog.Register({.name = "Game::ResumeClicked", .push = [](EventQueue &events) { events.Push(ResumeClicked{}); }});
    catalog.Register({.name = "Game::QuitRequested", .push = [](EventQueue &events) { events.Push(QuitRequested{}); }});
    return catalog;
}

} // namespace

TEST_CASE("EventCatalog: a found entry pushes its own type")
{
    const EventCatalog catalog = TwoEvents();
    EventQueue events;

    const EventDefinition *const entry = catalog.Find("Game::ResumeClicked");
    REQUIRE(entry != nullptr);
    entry->push(events);

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
    catalog.Register(
        {.name = "Game::ResumeClicked", .push = [](EventQueue &events) { events.Push(ResumeClicked{.marker = 1}); }});
    catalog.Register(
        {.name = "Game::ResumeClicked", .push = [](EventQueue &events) { events.Push(ResumeClicked{.marker = 2}); }});

    EventQueue events;
    catalog.Find("Game::ResumeClicked")->push(events);

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
