/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/EventRoute.hpp>
#include <Assisi/Core/EventScope.hpp>
#include <Assisi/Testing/ThrowOnContractViolation.hpp>

#include <cstdint>
#include <typeinfo>

using namespace Assisi::Core;

namespace
{

/// About one world.
struct Hit
{
    int32_t damage = 0;
};

/// About the whole program, as an AEVENT(universal) would be.
struct Quit
{
    int32_t code = 0;
};

/// Two worlds and the queue the program shares.
struct TwoWorlds
{
    EventQueue firstQueue;
    EventQueue secondQueue;
    EventQueue universal{EventScope::Universal};
    EventRoute first{firstQueue, universal};
    EventRoute second{secondQueue, universal};

    TwoWorlds() { DeclareUniversal(typeid(Quit)); }
};

} // namespace

TEST_CASE("EventRoute: a world event goes to its world's queue, a universal one to the program's")
{
    TwoWorlds worlds;
    worlds.first.Push(Hit{.damage = 3});
    worlds.first.Push(Quit{.code = 1});

    REQUIRE(worlds.firstQueue.Read<Hit>().size() == 1);
    CHECK(worlds.firstQueue.Read<Hit>()[0].damage == 3);
    REQUIRE(worlds.universal.Read<Quit>().size() == 1);
    CHECK(worlds.universal.Read<Quit>()[0].code == 1);
}

TEST_CASE("EventRoute: a world event stays in its world, and a universal one reaches every world")
{
    TwoWorlds worlds;
    worlds.first.Push(Hit{.damage = 3});
    worlds.first.Push(Quit{.code = 1});

    CHECK(worlds.first.Read<Hit>().size() == 1);
    CHECK(worlds.second.Read<Hit>().empty());
    CHECK(worlds.first.Read<Quit>().size() == 1);
    CHECK(worlds.second.Read<Quit>().size() == 1);
}

TEST_CASE("EventRoute: a DropScope discards pushes into both queues, and the next push is kept")
{
    TwoWorlds worlds;
    {
        const EventRoute::DropScope dropping(worlds.first);
        worlds.first.Push(Hit{.damage = 1});
        worlds.first.Push(Quit{.code = 1});
    }
    worlds.first.Push(Hit{.damage = 2});
    worlds.first.Push(Quit{.code = 2});

    REQUIRE(worlds.first.Read<Hit>().size() == 1);
    CHECK(worlds.first.Read<Hit>()[0].damage == 2);
    REQUIRE(worlds.first.Read<Quit>().size() == 1);
    CHECK(worlds.first.Read<Quit>()[0].code == 2);
}

#ifndef NDEBUG
TEST_CASE("EventRoute guard: a route given a queue of the wrong scope is caught")
{
    // A host that handed a world's queue in as the universal one would have
    // every world's universal events land in that one world.
    const Assisi::Testing::ThrowOnContractViolation guard;
    EventQueue world;
    EventQueue otherWorld;
    EventQueue universal{EventScope::Universal};

    CHECK_THROWS_AS((EventRoute{world, otherWorld}), ContractViolation);
    CHECK_THROWS_AS((EventRoute{universal, universal}), ContractViolation);
    CHECK_NOTHROW((EventRoute{world, universal}));
}
#endif // !NDEBUG
