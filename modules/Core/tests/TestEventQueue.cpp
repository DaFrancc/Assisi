/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Core/EventQueue.hpp>

#include <vector>

using namespace Assisi::Core;

namespace
{
struct Damage
{
    int amount = 0;
};
struct Healed
{
    int amount = 0;
};
} // namespace

// Constructing a local EventQueue here is the point of de-singletonizing it:
// tests get an isolated queue instead of sharing one process-wide instance.
TEST_CASE("EventQueue: push then read returns events in order")
{
    EventQueue queue;
    queue.Push(Damage{5});
    queue.Push(Damage{3});

    const auto events = queue.Read<Damage>();
    REQUIRE(events.size() == 2);
    CHECK(events[0].amount == 5);
    CHECK(events[1].amount == 3);
}

TEST_CASE("EventQueue: reading an un-pushed type yields an empty span")
{
    EventQueue queue;
    queue.Push(Damage{1});
    CHECK(queue.Read<Healed>().empty());
}

TEST_CASE("EventQueue: the read view is range-iterable in push order")
{
    EventQueue queue;
    queue.Push(Damage{5});
    queue.Push(Damage{3});
    queue.Push(Damage{8});

    std::vector<int> seen;
    for (const Damage &d : queue.Read<Damage>())
        seen.push_back(d.amount);

    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == 5);
    CHECK(seen[1] == 3);
    CHECK(seen[2] == 8);
}

TEST_CASE("EventQueue: event types are independent")
{
    EventQueue queue;
    queue.Push(Damage{7});
    queue.Push(Healed{2});
    queue.Push(Healed{4});

    CHECK(queue.Read<Damage>().size() == 1);
    CHECK(queue.Read<Healed>().size() == 2);
    CHECK(queue.Read<Damage>()[0].amount == 7);
}

TEST_CASE("EventQueue: flush clears every queue")
{
    EventQueue queue;
    queue.Push(Damage{1});
    queue.Push(Healed{1});

    queue.Flush();

    CHECK(queue.Read<Damage>().empty());
    CHECK(queue.Read<Healed>().empty());

    // Still usable after a flush.
    queue.Push(Damage{9});
    REQUIRE(queue.Read<Damage>().size() == 1);
    CHECK(queue.Read<Damage>()[0].amount == 9);
}
