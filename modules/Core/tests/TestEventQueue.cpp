/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Testing/ThrowOnContractViolation.hpp>

#include <cstdint>
#include <typeinfo>
#include <vector>

using namespace Assisi::Core;

namespace
{
struct Damage
{
    int32_t amount = 0;
};
struct Healed
{
    int32_t amount = 0;
};

} // namespace

// Each case builds its own EventQueue: it is not a global, so tests never share
// state through it.
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

    std::vector<int32_t> seen;
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

TEST_CASE("EventQueue: a push inside a DropScope is discarded, and one after it is kept")
{
    EventQueue queue;
    queue.Push(Damage{1});
    {
        const EventQueue::DropScope dropping(queue);
        queue.Push(Damage{2});
        queue.Push(Healed{3});
    }
    queue.Push(Damage{4});

    const EventSpan<Damage> damage = queue.Read<Damage>();
    REQUIRE(damage.size() == 2);
    CHECK(damage[0].amount == 1);
    CHECK(damage[1].amount == 4);
    CHECK(queue.Read<Healed>().empty());
}

// The read-view invalidation guard. With a throwing contract handler installed,
// a fired ASSISI_ASSERT surfaces as a catchable ContractViolation. Debug-only —
// the check compiles out in release.
#ifndef NDEBUG
TEST_CASE("EventQueue guard: pushing the same type while reading is caught")
{
    Assisi::Testing::ThrowOnContractViolation guard;
    EventQueue queue;
    for (int32_t i = 0; i < 4; ++i)
        queue.Push(Damage{i});

    CHECK_THROWS_AS(([&]
    {
        int32_t safety = 0;
        for (const Damage &damage : queue.Read<Damage>())
        {
            (void)damage;
            queue.Push(Damage{99});                  // same type: may realloc the vector being read
            if (++safety > 1000)                     // stop a missed detection from looping forever
                break;
        }
    }()),
                    ContractViolation);
}

TEST_CASE("EventQueue guard: indexing past the end of a read view is caught")
{
    Assisi::Testing::ThrowOnContractViolation guard;
    EventQueue queue;
    queue.Push(Damage{5});

    const EventSpan<Damage> events = queue.Read<Damage>();
    CHECK_NOTHROW((void)events[0]);
    CHECK_THROWS_AS((void)events[1], ContractViolation);
}

TEST_CASE("EventQueue guard: pushing a different type while reading is allowed")
{
    Assisi::Testing::ThrowOnContractViolation guard;
    EventQueue queue;
    for (int32_t i = 0; i < 4; ++i)
        queue.Push(Damage{i});

    int32_t seen = 0;
    // Healed lives in a different vector than the Damage view, so pushing it is
    // safe and must not trip the guard.
    CHECK_NOTHROW(([&]
    {
        for (const Damage &damage : queue.Read<Damage>())
        {
            (void)damage;
            ++seen;
            queue.Push(Healed{1});
        }
    }()));
    CHECK(seen == 4);
}

TEST_CASE("EventQueue guard: copying out then pushing after the loop is allowed")
{
    Assisi::Testing::ThrowOnContractViolation guard;
    EventQueue queue;
    for (int32_t i = 0; i < 4; ++i)
        queue.Push(Damage{i});

    std::vector<int32_t> collected;
    CHECK_NOTHROW(([&]
    {
        for (const Damage &damage : queue.Read<Damage>())
            collected.push_back(damage.amount);
    }()));
    // Deferring the push until after the loop is the sanctioned pattern.
    CHECK_NOTHROW(queue.Push(Damage{100}));
    CHECK(collected.size() == 4);
    CHECK(queue.Read<Damage>().size() == 5);
}

// A queue holds one scope. A universal event in a world's queue reaches nobody
// looking for it, and a world event in the universal queue reaches every world.
namespace
{

/// Belongs to the whole program, as an AEVENT(universal) would.
struct Broadcast
{
};

void DeclareBroadcast()
{
    DeclareUniversal(typeid(Broadcast));
}

} // namespace

TEST_CASE("EventQueue guard: a universal event pushed into a world's queue is caught")
{
    Assisi::Testing::ThrowOnContractViolation guard;
    DeclareBroadcast();
    EventQueue world;

    CHECK_THROWS_AS(world.Push(Broadcast{}), ContractViolation);
    CHECK_NOTHROW(world.Push(Damage{1}));
}

TEST_CASE("EventQueue guard: a world event pushed into the universal queue is caught")
{
    Assisi::Testing::ThrowOnContractViolation guard;
    DeclareBroadcast();
    EventQueue universal{EventScope::Universal};

    CHECK_THROWS_AS(universal.Push(Damage{1}), ContractViolation);
    CHECK_NOTHROW(universal.Push(Broadcast{}));
}

TEST_CASE("EventQueue guard: reading an event from the queue of the other scope is caught")
{
    Assisi::Testing::ThrowOnContractViolation guard;
    DeclareBroadcast();
    EventQueue world;
    EventQueue universal{EventScope::Universal};

    // Always empty, so a reader looking there would never hear of one.
    CHECK_THROWS_AS((void)world.Read<Broadcast>(), ContractViolation);
    CHECK_THROWS_AS((void)universal.Read<Damage>(), ContractViolation);
    CHECK_NOTHROW((void)world.Read<Damage>());
    CHECK_NOTHROW((void)universal.Read<Broadcast>());
}
#endif // !NDEBUG
