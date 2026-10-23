/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBodyOwnership.cpp
/// @brief The host's rules for which player simulates which body, on groups and
///        distances made up here, with no scene or physics world.

#include <doctest/doctest.h>

#include <Assisi/NetSync/BodyOwnership.hpp>

#include <cstdint>
#include <vector>

using namespace Assisi::NetSync;

namespace
{

constexpr std::uint32_t kAlice = 2;
constexpr std::uint32_t kBob = 3;

/// Rules with round numbers: given at 2 m, lost past 4 m, a 10-tick touch
/// dwell and a 30-tick hold.
OwnershipConfig Rules()
{
    OwnershipConfig config;
    config.ownershipDistance = 2.f;
    config.leaveDistance = 4.f;
    config.touchDwellTicks = 10;
    config.holdTicks = 30;
    config.maxBodiesPerClient = 8;
    return config;
}

/// One body, @p id, with @p players near it.
BodyGroup Lone(std::uint32_t id, std::vector<PlayerReach> players)
{
    return BodyGroup{.bodies = {NetId{id}}, .players = std::move(players)};
}

PlayerReach At(std::uint32_t client, float distance, bool touching = false)
{
    return PlayerReach{.distance = distance, .client = client, .touching = touching};
}

/// Runs @p ticks ticks of @p groups from @p tick, and returns the tick after.
std::uint64_t Run(BodyOwnership &ownership, const std::vector<BodyGroup> &groups, std::uint64_t tick,
                  std::uint32_t ticks)
{
    for (std::uint32_t i = 0; i < ticks; ++i)
    {
        ownership.Assign(groups, tick++);
    }
    return tick;
}

} // namespace

TEST_CASE("A body goes to the player inside the ownership distance, and to no one further")
{
    BodyOwnership ownership(Rules());
    ownership.Assign(std::vector<BodyGroup>{Lone(1, {At(kAlice, 3.f)})}, 1);
    CHECK(ownership.OwnerOf(NetId{1}) == 0u);

    ownership.Assign(std::vector<BodyGroup>{Lone(1, {At(kAlice, 1.5f), At(kBob, 1.8f)})}, 2);
    CHECK(ownership.OwnerOf(NetId{1}) == kAlice);
}

TEST_CASE("A body stays with its player inside the leave distance while another is nearer, and goes beyond it")
{
    BodyOwnership ownership(Rules());
    std::uint64_t tick = Run(ownership, {Lone(1, {At(kAlice, 1.f)})}, 1, 1);
    REQUIRE(ownership.OwnerOf(NetId{1}) == kAlice);

    // Past the hold, so only the distances decide.
    tick = Run(ownership, {Lone(1, {At(kAlice, 3.5f), At(kBob, 0.5f)})}, tick, Rules().holdTicks * 2);
    CHECK(ownership.OwnerOf(NetId{1}) == kAlice);

    tick = Run(ownership, {Lone(1, {At(kAlice, 4.5f), At(kBob, 0.5f)})}, tick, 1);
    CHECK(ownership.OwnerOf(NetId{1}) != kAlice);
}

TEST_CASE("A player touching a body its owner does not takes it after the dwell, and only once")
{
    BodyOwnership ownership(Rules());
    std::uint64_t tick = Run(ownership, {Lone(1, {At(kAlice, 1.f)})}, 1, Rules().holdTicks);
    REQUIRE(ownership.OwnerOf(NetId{1}) == kAlice);

    const std::vector<BodyGroup> pushed{Lone(1, {At(kAlice, 1.5f), At(kBob, 0.6f, /*touching=*/ true)})};
    tick = Run(ownership, pushed, tick, Rules().touchDwellTicks);
    CHECK(ownership.OwnerOf(NetId{1}) == kAlice);
    tick = Run(ownership, pushed, tick, 1);
    REQUIRE(ownership.OwnerOf(NetId{1}) == kBob);
    const std::uint64_t changed = ownership.Holdings().at(NetId{1}).changedTick;

    // Bob pushing on, Alice beside it untouching: it stays Bob's.
    tick = Run(ownership, pushed, tick, Rules().holdTicks * 3);
    CHECK(ownership.OwnerOf(NetId{1}) == kBob);
    CHECK(ownership.Holdings().at(NetId{1}).changedTick == changed);
}

TEST_CASE("A touch that stops before the dwell starts again from nothing")
{
    BodyOwnership ownership(Rules());
    std::uint64_t tick = Run(ownership, {Lone(1, {At(kAlice, 1.f)})}, 1, Rules().holdTicks);
    const std::vector<BodyGroup> touched{Lone(1, {At(kAlice, 1.5f), At(kBob, 0.6f, true)})};
    const std::vector<BodyGroup> untouched{Lone(1, {At(kAlice, 1.5f), At(kBob, 0.9f)})};
    tick = Run(ownership, touched, tick, Rules().touchDwellTicks - 1);
    tick = Run(ownership, untouched, tick, 1);
    tick = Run(ownership, touched, tick, Rules().touchDwellTicks - 1);
    CHECK(ownership.OwnerOf(NetId{1}) == kAlice);
}

TEST_CASE("Bodies touching each other go to one player together")
{
    BodyOwnership ownership(Rules());
    const BodyGroup pile{.bodies = {NetId{1}, NetId{2}, NetId{3}}, .players = {At(kAlice, 1.f), At(kBob, 3.f)}};
    ownership.Assign(std::vector<BodyGroup>{pile}, 1);
    CHECK(ownership.OwnerOf(NetId{1}) == kAlice);
    CHECK(ownership.OwnerOf(NetId{2}) == kAlice);
    CHECK(ownership.OwnerOf(NetId{3}) == kAlice);
}

TEST_CASE("A body Alice's crate is pushed into becomes Alice's at once, whoever had it")
{
    BodyOwnership ownership(Rules());
    std::uint64_t tick = Run(ownership, {Lone(1, {At(kAlice, 1.f)}), Lone(2, {At(kBob, 1.f)})}, 1, 1);
    REQUIRE(ownership.OwnerOf(NetId{2}) == kBob);

    const BodyGroup merged{.bodies = {NetId{1}, NetId{2}}, .players = {At(kAlice, 1.f), At(kBob, 1.5f)}};
    tick = Run(ownership, {merged}, tick, 1);
    CHECK(ownership.OwnerOf(NetId{1}) == kAlice);
    CHECK(ownership.OwnerOf(NetId{2}) == kAlice);
}

TEST_CASE("A player is given at most its limit of bodies, the nearest")
{
    OwnershipConfig rules = Rules();
    rules.maxBodiesPerClient = 2;
    BodyOwnership ownership(rules);
    ownership.Assign(std::vector<BodyGroup>{Lone(1, {At(kAlice, 1.5f)}), Lone(2, {At(kAlice, 0.5f)}),
                                            Lone(3, {At(kAlice, 1.f)})},
                     1);
    CHECK(ownership.OwnerOf(NetId{1}) == 0u);
    CHECK(ownership.OwnerOf(NetId{2}) == kAlice);
    CHECK(ownership.OwnerOf(NetId{3}) == kAlice);
}

TEST_CASE("The host's own player is a player like any other, with no limit")
{
    OwnershipConfig rules = Rules();
    rules.maxBodiesPerClient = 0;
    BodyOwnership ownership(rules);
    ownership.Assign(std::vector<BodyGroup>{Lone(1, {At(HostClientId.value, 1.f), At(kAlice, 1.5f)}),
                                            Lone(2, {At(kAlice, 0.5f)})},
                     1);
    CHECK(ownership.OwnerOf(NetId{1}) == HostClientId.value);
    CHECK(ownership.OwnerOf(NetId{2}) == 0u);
}

TEST_CASE("A body taken back is given to no one for the hold, its group's player included")
{
    BodyOwnership ownership(Rules());
    const std::vector<BodyGroup> pair{
        BodyGroup{.bodies = {NetId{1}, NetId{2}}, .players = {At(kAlice, 1.f), At(kBob, 1.5f)}}};
    std::uint64_t tick = Run(ownership, pair, 1, 1);
    REQUIRE(ownership.OwnerOf(NetId{1}) == kAlice);

    ownership.TakeBack(NetId{1}, tick);
    tick = Run(ownership, pair, tick, Rules().holdTicks - 1);
    CHECK(ownership.OwnerOf(NetId{1}) == 0u);
    CHECK(ownership.OwnerOf(NetId{2}) == kAlice);

    tick = Run(ownership, pair, tick, 2);
    CHECK(ownership.OwnerOf(NetId{1}) == kAlice);
}

TEST_CASE("A player who leaves has its bodies given on at once")
{
    BodyOwnership ownership(Rules());
    std::uint64_t tick = Run(ownership, {Lone(1, {At(kAlice, 1.f), At(kBob, 1.5f)})}, 1, 1);
    REQUIRE(ownership.OwnerOf(NetId{1}) == kAlice);

    ownership.ForgetPlayer(kAlice);
    tick = Run(ownership, {Lone(1, {At(kBob, 1.5f)})}, tick, 1);
    CHECK(ownership.OwnerOf(NetId{1}) == kBob);
}

TEST_CASE("A body no longer handed in is forgotten")
{
    BodyOwnership ownership(Rules());
    ownership.Assign(std::vector<BodyGroup>{Lone(1, {At(kAlice, 1.f)})}, 1);
    REQUIRE(ownership.OwnerOf(NetId{1}) == kAlice);
    ownership.Assign(std::vector<BodyGroup>{}, 2);
    CHECK(ownership.OwnerOf(NetId{1}) == 0u);
    CHECK(ownership.Holdings().empty());
}

TEST_CASE("A body just given waits out the hold before a player touching it takes it over")
{
    BodyOwnership ownership(Rules());
    std::uint64_t tick = Run(ownership, {Lone(1, {At(kAlice, 1.f)})}, 1, 1);
    REQUIRE(ownership.OwnerOf(NetId{1}) == kAlice);

    const std::vector<BodyGroup> pushed{Lone(1, {At(kAlice, 1.5f), At(kBob, 0.6f, /*touching=*/ true)})};
    tick = Run(ownership, pushed, tick, Rules().holdTicks - 2);
    CHECK(ownership.OwnerOf(NetId{1}) == kAlice);
    tick = Run(ownership, pushed, tick, 2);
    CHECK(ownership.OwnerOf(NetId{1}) == kBob);
}

TEST_CASE("With no one holding a body, a player touching it beats a nearer one")
{
    // A long body is touched at one end by a player further from its centre
    // than another standing by its middle.
    BodyOwnership ownership(Rules());
    ownership.Assign(std::vector<BodyGroup>{Lone(1, {At(kBob, 1.f), At(kAlice, 1.9f, /*touching=*/ true)})}, 1);
    CHECK(ownership.OwnerOf(NetId{1}) == kAlice);
}

namespace
{

constexpr std::uint32_t kCarol = 4;

/// Whether @p netId is contested: the host simulating it for the players
/// touching it.
bool Contested(const BodyOwnership &ownership, NetId netId)
{
    return ownership.OwnerOf(netId) == 0u && ownership.Holdings().at(netId).contestedTick != 0;
}

} // namespace

TEST_CASE("A body two players touch is the host's to simulate, at once and inside the hold")
{
    BodyOwnership ownership(Rules());
    std::uint64_t tick = Run(ownership, {Lone(1, {At(kAlice, 0.6f, true)})}, 1, 1);
    REQUIRE(ownership.OwnerOf(NetId{1}) == kAlice);

    tick = Run(ownership, {Lone(1, {At(kAlice, 0.6f, true), At(kBob, 0.7f, true)})}, tick, 1);
    CHECK(Contested(ownership, NetId{1}));
}

TEST_CASE("A contest outlasts the second player's touch by the dwell, then the one still touching has it at once")
{
    BodyOwnership ownership(Rules());
    std::uint64_t tick = Run(ownership, {Lone(1, {At(kAlice, 0.6f, true)})}, 1, 1);
    tick = Run(ownership, {Lone(1, {At(kAlice, 0.6f, true), At(kBob, 0.7f, true)})}, tick, 1);
    REQUIRE(Contested(ownership, NetId{1}));

    // Inside the hold the contest began, which the one left touching does not
    // wait out.
    const std::vector<BodyGroup> alone{Lone(1, {At(kAlice, 0.6f, true), At(kBob, 1.5f)})};
    tick = Run(ownership, alone, tick, Rules().touchDwellTicks - 1);
    CHECK(Contested(ownership, NetId{1}));
    tick = Run(ownership, alone, tick, 1);
    CHECK(ownership.OwnerOf(NetId{1}) == kAlice);
    CHECK(ownership.Holdings().at(NetId{1}).contestedTick == 0u);
}

TEST_CASE("The host's own player contests a body like any other")
{
    BodyOwnership ownership(Rules());
    Run(ownership, {Lone(1, {At(HostClientId.value, 0.6f, true), At(kAlice, 0.7f, true)})}, 1, 1);
    CHECK(Contested(ownership, NetId{1}));
}

TEST_CASE("A body three players touch stays contested while any two of them still do")
{
    BodyOwnership ownership(Rules());
    std::uint64_t tick =
        Run(ownership, {Lone(1, {At(kAlice, 0.9f, true), At(kBob, 0.6f, true), At(kCarol, 0.7f, true)})}, 1, 1);
    REQUIRE(Contested(ownership, NetId{1}));
    tick = Run(ownership, {Lone(1, {At(kAlice, 1.5f), At(kBob, 0.6f, true), At(kCarol, 0.7f, true)})}, tick,
               Rules().touchDwellTicks * 2);
    CHECK(Contested(ownership, NetId{1}));
}

TEST_CASE("A body a contested one touches is contested with it")
{
    BodyOwnership ownership(Rules());
    std::uint64_t tick = Run(ownership, {Lone(1, {At(kAlice, 0.6f, true), At(kBob, 0.7f, true)})}, 1, 1);
    const BodyGroup pile{.bodies = {NetId{1}, NetId{2}}, .players = {At(kAlice, 0.6f, true), At(kBob, 1.5f)}};
    tick = Run(ownership, {pile}, tick, 1);
    CHECK(Contested(ownership, NetId{1}));
    CHECK(Contested(ownership, NetId{2}));
}
