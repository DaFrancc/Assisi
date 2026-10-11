/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestNetClock.cpp
/// @brief The client's clock: how far ahead of the server it starts, how it
/// steers the server's queue of its commands by running faster or slower, and
/// that the tick it stamps commands with never goes back.

#include <doctest/doctest.h>

#include <Assisi/NetSync/InputCommand.hpp>
#include <Assisi/NetSync/NetClock.hpp>

#include <algorithm>
#include <cstdint>
#include <deque>

using namespace Assisi::NetSync;

namespace
{
constexpr double kTickRate = 60.0;

/// The feedback a healthy connection produces: the server has the cushion it
/// wants and has never gone hungry.
ClockFeedback Healthy(std::uint64_t serverTick, std::uint32_t depth = 2)
{
    return ClockFeedback{.serverTick = serverTick, .inputBufferDepth = depth, .starvedTicks = 0};
}

/// A clock that has had its first contact at server tick @p serverTick over a
/// zero round trip, so its command tick is that plus the cushion and one frame.
NetClock Synced(std::uint64_t serverTick)
{
    NetClock clock(kTickRate);
    clock.OnSnapshot(Healthy(serverTick), 0);
    return clock;
}

/// One message on its way, and the real tick it lands on.
struct InFlight
{
    std::uint64_t arrives = 0;
    std::uint64_t tick = 0;     ///< A command's tick, or a snapshot's server tick.
    std::uint32_t depth = 0;    ///< A snapshot's queue depth.
    std::uint32_t starved = 0;  ///< A snapshot's starved ticks since the last one.
};

/// A server and a client exchanging commands and snapshots through delay lines,
/// in real ticks: the server steps once a real tick, the client as often as its
/// clock's rate says.
struct Loop
{
    /// Server ticks between snapshots: 20 Hz at 60.
    static constexpr std::uint64_t kSnapshotEvery = 3;

    NetClock clock{kTickRate};
    InputCommandQueue queue;
    std::deque<InFlight> up;
    std::deque<InFlight> down;
    double accumulator = 0.0;
    std::uint64_t now = 0;
    std::uint64_t serverTick = 1000;
    std::uint64_t upDelay = 3;
    std::uint64_t downDelay = 3;
    std::uint64_t starvedAtLastSnapshot = 0;
    std::uint64_t lastCommand = 0;
    std::uint32_t lastDepth = 0;
    std::uint32_t lastStarved = 0;
    bool commandWentBack = false;

    std::int32_t RoundTripMs() const
    {
        return static_cast<std::int32_t>(static_cast<double>(upDelay + downDelay) * 1000.0 / kTickRate);
    }

    void ClientStep()
    {
        clock.Tick();
        while (!down.empty() && down.front().arrives <= now)
        {
            clock.OnSnapshot(
                ClockFeedback{.serverTick = down.front().tick, .inputBufferDepth = down.front().depth,
                              .starvedTicks = down.front().starved},
                RoundTripMs());
            down.pop_front();
        }
        if (!clock.HasFeedback())
        {
            return;
        }
        if (clock.CommandTick() <= lastCommand)
        {
            commandWentBack = true;
        }
        lastCommand = clock.CommandTick();
        up.push_back(InFlight{.arrives = now + upDelay, .tick = clock.CommandTick()});
    }

    void ServerStep()
    {
        ++serverTick;
        while (!up.empty() && up.front().arrives <= now)
        {
            InputCommand command;
            command.tick = up.front().tick;
            (void)queue.Accept(command);
            up.pop_front();
        }
        (void)queue.Consume(serverTick);
        if (serverTick % kSnapshotEvery == 0)
        {
            lastDepth = static_cast<std::uint32_t>(queue.Depth());
            lastStarved = static_cast<std::uint32_t>(queue.StarvedTicks() - starvedAtLastSnapshot);
            starvedAtLastSnapshot = queue.StarvedTicks();
            down.push_back(
                InFlight{.arrives = now + downDelay, .tick = serverTick, .depth = lastDepth, .starved = lastStarved});
        }
    }

    /// One real tick: the server steps once, the client by its clock's rate.
    void Step()
    {
        ++now;
        ServerStep();
        accumulator += clock.HasFeedback() ? clock.Rate() : 1.0;
        while (accumulator >= 1.0)
        {
            accumulator -= 1.0;
            ClientStep();
        }
    }

    /// Runs @p ticks real ticks and says whether the last @p settled of them
    /// were all at rest: nothing starved, the depth at the cushion, the clock at
    /// rate one.
    bool RunAndSettle(std::int32_t ticks, std::int32_t settled)
    {
        bool rested = true;
        for (std::int32_t i = 0; i < ticks; ++i)
        {
            Step();
            if (i >= ticks - settled)
            {
                const bool cushioned = lastDepth >= 1 && lastDepth <= 3;
                rested = rested && lastStarved == 0 && cushioned && clock.Rate() == 1.0;
            }
        }
        return rested;
    }
};

/// Real ticks to settle from first contact or a change of delay, and the tail
/// of them that has to be at rest.
constexpr std::int32_t kSettleTicks = 1800;
constexpr std::int32_t kRestTicks = 60;

} // namespace

TEST_CASE("before any feedback the clock is free-running")
{
    NetClock clock(kTickRate);
    CHECK_FALSE(clock.HasFeedback());
    CHECK(clock.EstimatedServerTick() == 0);

    clock.Tick();
    clock.Tick();
    CHECK(clock.CommandTick() == 2);
}

TEST_CASE("the first snapshot leads by the round trip, a frame and the cushion")
{
    NetClock clock(kTickRate);
    for (int32_t i = 0; i < 500; ++i)
        clock.Tick(); // free-running counter is at an arbitrary value

    // 100 ms at 60 Hz is six ticks there and back: the snapshot is already a
    // trip down old, and the command still has a trip up to make.
    clock.OnSnapshot(Healthy(1000), 100);

    CHECK(clock.HasFeedback());
    CHECK(clock.CommandTick() == 1000 + 6 + 1 + 2);
    CHECK(clock.Rate() == 1.0);
    // No correction is counted for the initial sync — there was nothing to
    // correct away from.
    CHECK(clock.CorrectionCount() == 0);
}

TEST_CASE("a higher round-trip time buys a longer lead")
{
    NetClock fast(kTickRate);
    NetClock slow(kTickRate);

    fast.OnSnapshot(Healthy(100), 20);
    slow.OnSnapshot(Healthy(100), 200);

    CHECK(slow.Lead() > fast.Lead());
    CHECK(fast.Lead() >= 1);
}

TEST_CASE("the lead is capped no matter how absurd the round-trip estimate is")
{
    NetClockConfig config;
    config.maxLead = 10;
    NetClock clock(kTickRate, config);

    clock.OnSnapshot(Healthy(50), 100000);
    CHECK(clock.Lead() == config.maxLead);
}

TEST_CASE("a deep queue slows the clock and never moves the command tick back")
{
    // Moving it back would stamp new commands with ticks the server already
    // holds commands for, and it drops them as copies: whatever the player did
    // in those ticks would never arrive.
    NetClock clock = Synced(100);
    for (int32_t i = 0; i < 10; ++i)
        clock.Tick();
    const std::uint64_t before = clock.CommandTick();

    clock.OnSnapshot(Healthy(110, 20), 0);

    CHECK(clock.CommandTick() == before);
    CHECK(clock.Rate() == doctest::Approx(1.0 - NetClockConfig{}.maxDilation));
    CHECK(clock.CorrectionCount() == 0);
}

TEST_CASE("a starved queue speeds the clock up, and only so far")
{
    NetClock clock = Synced(100);
    for (int32_t i = 0; i < 10; ++i)
        clock.Tick();
    const std::uint64_t before = clock.CommandTick();

    ClockFeedback starving = Healthy(110, 0);
    starving.starvedTicks = 50;
    clock.OnSnapshot(starving, 0);

    CHECK(clock.CommandTick() == before);
    CHECK(clock.Rate() == doctest::Approx(1.0 + NetClockConfig{}.maxDilation));
    CHECK(clock.CorrectionCount() == 0);
}

TEST_CASE("a queue within a tick of the cushion runs at rate one")
{
    const std::uint32_t cushion = NetClockConfig{}.targetBufferDepth;
    for (const std::uint32_t depth : {cushion - 1, cushion, cushion + 1})
    {
        CAPTURE(depth);
        NetClock clock = Synced(100);
        for (int32_t i = 0; i < 10; ++i)
            clock.Tick();
        clock.OnSnapshot(Healthy(110, depth), 0);
        CHECK(clock.Rate() == 1.0);
    }
}

TEST_CASE("starvation from before the first command could arrive is not acted on")
{
    // The server cannot have a command for a tick before the first one the
    // client stamped; starving there says nothing about the lead. Acting on it
    // is what ran the lead up to its cap.
    NetClock clock = Synced(100);
    const std::uint64_t first = clock.CommandTick();

    ClockFeedback early = Healthy(first - 1, 0);
    early.starvedTicks = 1;
    clock.Tick();
    clock.OnSnapshot(early, 0);

    CHECK(clock.Rate() == 1.0);
    CHECK(clock.CorrectionCount() == 0);
}

TEST_CASE("an older snapshot arriving late changes nothing")
{
    NetClock clock = Synced(100);
    for (int32_t i = 0; i < 100; ++i)
        clock.Tick();
    clock.OnSnapshot(Healthy(200), 0);
    const std::uint64_t estimate = clock.EstimatedServerTick();

    clock.OnSnapshot(Healthy(199, 20), 0);

    CHECK(clock.EstimatedServerTick() == estimate);
    CHECK(clock.Rate() == 1.0);
}

TEST_CASE("the same snapshot read again is not news")
{
    // The client hands the clock the last snapshot every tick, whether or not a
    // new one came. Taken as new each time, the server would seem to stand
    // still between snapshots.
    NetClock clock = Synced(100);
    for (int32_t i = 0; i < 10; ++i)
        clock.Tick();
    clock.OnSnapshot(Healthy(110), 0);

    clock.Tick();
    clock.OnSnapshot(Healthy(110), 0);

    CHECK(clock.EstimatedServerTick() == 111);
}

TEST_CASE("a client that fell behind the server jumps forward once")
{
    // A stall: the server ran on while the client did not, so every command it
    // sends now is already too late. Only a jump catches up; the skipped ticks
    // starve, and that is not acted on as well.
    NetClock clock = Synced(100);
    clock.OnSnapshot(Healthy(200), 0);

    CHECK(clock.CorrectionCount() == 1);
    CHECK(clock.CommandTick() == 200 + 1 + NetClockConfig{}.targetBufferDepth);

    ClockFeedback skipped = Healthy(201, 0);
    skipped.starvedTicks = 3;
    clock.Tick();
    clock.OnSnapshot(skipped, 0);
    CHECK(clock.CorrectionCount() == 1);
    CHECK(clock.Rate() == 1.0);
}

TEST_CASE("the estimated server tick advances between snapshots")
{
    NetClock clock(kTickRate);
    clock.OnSnapshot(Healthy(500), 40);
    CHECK(clock.EstimatedServerTick() == 500);

    for (int32_t i = 0; i < 5; ++i)
        clock.Tick();
    // No new snapshot: the server is assumed to have advanced at the same rate.
    CHECK(clock.EstimatedServerTick() == 505);

    // A snapshot replaces the estimate rather than compounding with it.
    clock.OnSnapshot(Healthy(504), 40);
    CHECK(clock.EstimatedServerTick() == 504);
}

TEST_CASE("Reset returns the clock to its pre-handshake state")
{
    NetClock clock(kTickRate);
    clock.OnSnapshot(Healthy(700), 80);
    for (int32_t i = 0; i < 10; ++i)
        clock.Tick();

    clock.Reset();
    CHECK_FALSE(clock.HasFeedback());
    CHECK(clock.CommandTick() == 0);
    CHECK(clock.Lead() == 0);
    CHECK(clock.Rate() == 1.0);
    CHECK(clock.CorrectionCount() == 0);
    CHECK(clock.EstimatedServerTick() == 0);
}

TEST_CASE("against a server, the clock settles with the queue at its cushion and never goes back")
{
    Loop loop;
    CHECK(loop.RunAndSettle(kSettleTicks, kRestTicks));
    CHECK(loop.clock.CorrectionCount() == 0);
    CHECK_FALSE(loop.commandWentBack);

    // The connection gets slower, then faster again; it settles each time, and
    // the command tick still only ever goes forward.
    loop.upDelay += 6;
    loop.downDelay += 6;
    CHECK(loop.RunAndSettle(kSettleTicks, kRestTicks));
    loop.upDelay -= 6;
    loop.downDelay -= 6;
    CHECK(loop.RunAndSettle(kSettleTicks, kRestTicks));
    CHECK_FALSE(loop.commandWentBack);
}
