/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestNetClock.cpp
/// @brief The client's lead estimation: does it run far enough ahead of the
/// server, and does it react to the queue depth the server reports back?

#include <doctest/doctest.h>

#include <Assisi/NetSync/NetClock.hpp>

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

TEST_CASE("the first snapshot adopts the target lead outright")
{
    NetClock clock(kTickRate);
    for (int32_t i = 0; i < 500; ++i)
        clock.Tick(); // free-running counter is at an arbitrary value

    // 100 ms RTT at 60 Hz: ~3 ticks each way, plus a command frame, plus the
    // 2-tick cushion.
    clock.OnSnapshot(Healthy(1000), 100);

    CHECK(clock.HasFeedback());
    CHECK(clock.Lead() == clock.TargetLead());
    CHECK(clock.CommandTick() == 1000 + clock.TargetLead());
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

    CHECK(slow.TargetLead() > fast.TargetLead());
    CHECK(fast.TargetLead() >= 1);
}

TEST_CASE("the lead is capped no matter how absurd the round-trip estimate is")
{
    NetClockConfig config;
    config.maxLead = 10;
    NetClock clock(kTickRate, config);

    clock.OnSnapshot(Healthy(50), 100000);
    CHECK(clock.TargetLead() == config.maxLead);
}

TEST_CASE("a starved server queue grows the lead")
{
    NetClock clock(kTickRate);
    clock.OnSnapshot(Healthy(100), 30);
    const std::uint32_t settled = clock.TargetLead();

    // The server ran out of commands: whatever the RTT arithmetic says, the
    // client is not far enough ahead.
    ClockFeedback starving = Healthy(110, 0);
    starving.starvedTicks  = 3;
    for (int32_t i = 0; i < 10; ++i)
        clock.Tick();
    clock.OnSnapshot(starving, 30);

    CHECK(clock.TargetLead() > settled);
}

TEST_CASE("an overfull server queue shrinks the lead — gradually")
{
    NetClockConfig config;
    config.targetBufferDepth = 2;
    NetClock clock(kTickRate, config);

    clock.OnSnapshot(Healthy(100), 60);
    const std::uint32_t settled = clock.TargetLead();

    // Sitting on far more cushion than needed is pure added latency.
    std::uint64_t serverTick = 100;
    for (int32_t round = 0; round < 3; ++round)
    {
        for (int32_t i = 0; i < 10; ++i)
            clock.Tick();
        serverTick += 10;
        clock.OnSnapshot(Healthy(serverTick, 8), 60);
    }

    CHECK(clock.TargetLead() <= settled);
    // Shrinking is one tick at a time on purpose: dropping straight to the
    // arithmetic target would re-starve a connection whose jitter is exactly
    // what the extra cushion was absorbing.
    CHECK(clock.TargetLead() >= 1);
}

TEST_CASE("a deeply overfull server queue shrinks the lead as readily as a barely overfull one" *
          doctest::should_fail())
{
    // ENG-117, open. NetClock.cpp:45 guards the decrement with `target > excess`
    // instead of a floor on `target`, so the deeper the overrun the less likely
    // the shrink is to happen at all — the inverse of what the comment above it
    // ("shrink one tick at a time") describes, and wrong in exactly the case
    // that costs the most latency.
    //
    // The two halves below are identical but for the reported depth. RTT is zero
    // so the arithmetic target is the smallest it can be (0 one-way + 1 command
    // frame + 2 cushion = 3), which is what makes `excess` able to exceed it.
    //
    // should_fail until the shrink is fixed; the fix removes this decorator.
    NetClockConfig config;
    config.targetBufferDepth = 2;

    const auto leadAfterOverfullReport = [&config](std::uint32_t depth)
                                         {
                                             NetClock clock(kTickRate, config);
                                             clock.OnSnapshot(Healthy(100), 0); // first contact, adopts the target
                                             for (int32_t i = 0; i < 10; ++i)
                                                 clock.Tick();
                                             clock.OnSnapshot(Healthy(110, depth), 0);
                                             return clock.TargetLead();
                                         };

    // Barely over: excess (1) is below the arithmetic target, so the decrement runs.
    const std::uint32_t barely = leadAfterOverfullReport(3);
    // Deeply over: excess (18) is above it, so the decrement is skipped entirely.
    const std::uint32_t deeply = leadAfterOverfullReport(20);

    CAPTURE(barely);
    CAPTURE(deeply);
    // A queue holding ten times the cushion it was asked for cannot justify a
    // *longer* lead than one holding one tick too many.
    CHECK(deeply <= barely);
}

TEST_CASE("a stable connection tracks without snapping")
{
    NetClock clock(kTickRate);
    clock.OnSnapshot(Healthy(1000), 50);

    const std::uint32_t initialCorrections = clock.CorrectionCount();
    std::uint64_t serverTick         = 1000;

    // Both clocks advance at the same rate and the connection stays healthy;
    // nothing here should ever justify a snap.
    for (int32_t round = 0; round < 20; ++round)
    {
        for (int32_t i = 0; i < 3; ++i)
            clock.Tick();
        serverTick += 3;
        clock.OnSnapshot(Healthy(serverTick), 50);
    }

    CHECK(clock.CorrectionCount() == initialCorrections);
    CHECK(clock.Lead() == clock.TargetLead());
}

TEST_CASE("a large drift snaps the clock and is counted")
{
    NetClock clock(kTickRate);
    clock.OnSnapshot(Healthy(1000), 50);
    const std::uint32_t before = clock.CorrectionCount();

    // The client ran for a long time with no snapshot (a stall), so it is now
    // far further ahead than it should be.
    for (int32_t i = 0; i < 120; ++i)
        clock.Tick();
    clock.OnSnapshot(Healthy(1010), 50);

    CHECK(clock.CorrectionCount() == before + 1);
    CHECK(clock.CommandTick() == 1010 + clock.TargetLead());
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
    CHECK(clock.CorrectionCount() == 0);
    CHECK(clock.EstimatedServerTick() == 0);
}
