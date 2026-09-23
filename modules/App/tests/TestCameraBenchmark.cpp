/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCameraBenchmark.cpp
/// @brief A benchmark waits for the world, warms up, then flies its route in
/// exactly the time it was given.

#include <doctest/doctest.h>

#include <Assisi/App/CameraBenchmark.hpp>

#include <cstdint>
#include <vector>

using namespace Assisi;
using namespace Assisi::App;

namespace
{
constexpr float kTolerance = 1e-4f;

/// One line from x = 0 to x = 10, authored at @p seconds.
std::vector<Runtime::CameraRouteLeg> StraightRoute(float seconds)
{
    Runtime::CameraRouteLeg leg;
    leg.line.from = {0.f, 0.f, 0.f};
    leg.line.to = {10.f, 0.f, 0.f};
    leg.line.seconds = seconds;
    return {leg};
}

/// Advances through the warm-up with the world loaded, at time @p now.
void WarmUp(CameraBenchmark &benchmark, double now)
{
    for (std::int32_t i = 0; i < kBenchmarkWarmupFrames; ++i)
    {
        (void)benchmark.Advance(true, now);
    }
}
} // namespace

TEST_CASE("A benchmark does not start until the world has loaded")
{
    // Frames spent loading, one second apart: far longer than the run itself.
    constexpr std::int32_t kLoadingFrames = 1000;

    CameraBenchmark benchmark(StraightRoute(5.f), 10.0);
    for (std::int32_t i = 0; i < kLoadingFrames; ++i)
    {
        CHECK(benchmark.Advance(false, static_cast<double>(i)) == BenchmarkPhase::Settling);
    }
    CHECK(benchmark.Advance(true, static_cast<double>(kLoadingFrames)) == BenchmarkPhase::WarmingUp);
}

TEST_CASE("The warm-up holds the first pose for its frames, then the run starts")
{
    CameraBenchmark benchmark(StraightRoute(5.f), 10.0);
    (void)benchmark.Advance(true, 0.0);

    for (std::int32_t i = 0; i < kBenchmarkWarmupFrames - 1; ++i)
    {
        CHECK(benchmark.Advance(true, 1.0) == BenchmarkPhase::WarmingUp);
        CHECK(glm::length(benchmark.Aim().eye) < kTolerance);
    }
    CHECK(benchmark.Advance(true, 1.0) == BenchmarkPhase::Running);
    CHECK(benchmark.ElapsedSeconds() == doctest::Approx(0.0));
}

TEST_CASE("The route is scaled to fill the run, whatever its authored length")
{
    // Authored at 5 s, run for 10 s: halfway through the run is halfway along.
    CameraBenchmark benchmark(StraightRoute(5.f), 10.0);
    (void)benchmark.Advance(true, 0.0);
    WarmUp(benchmark, 100.0);
    REQUIRE(benchmark.Phase() == BenchmarkPhase::Running);

    CHECK(benchmark.Advance(true, 105.0) == BenchmarkPhase::Running);
    CHECK(benchmark.Aim().eye.x == doctest::Approx(5.f));
}

TEST_CASE("A shots run stops at evenly spaced points and wants each picture once, after it settles")
{
    constexpr std::int32_t kShots = 3;
    CameraBenchmark benchmark(StraightRoute(5.f), 10.0, kShots);
    (void)benchmark.Advance(true, 0.0);
    WarmUp(benchmark, 0.0);
    REQUIRE(benchmark.Phase() == BenchmarkPhase::Running);

    // The clock plays no part: the poses are the route's ends and middle.
    const float expectedX[kShots] = {0.f, 5.f, 10.f};
    std::vector<std::int32_t> shotsTaken;
    for (std::int32_t frame = 0; frame < kShots * kBenchmarkShotSettleFrames; ++frame)
    {
        (void)benchmark.Advance(true, 1000.0 * static_cast<double>(frame));
        if (benchmark.Phase() != BenchmarkPhase::Running)
        {
            break;
        }
        const std::int32_t shot = benchmark.ShotThisFrame();
        if (shot >= 0)
        {
            shotsTaken.push_back(shot);
            CHECK(benchmark.Aim().eye.x == doctest::Approx(expectedX[shot]));
        }
    }
    CHECK(shotsTaken == std::vector<std::int32_t>{0, 1, 2});
    CHECK(benchmark.Phase() == BenchmarkPhase::Finished);
}

TEST_CASE("The run finishes at its length and holds the last pose")
{
    CameraBenchmark benchmark(StraightRoute(20.f), 3.0);
    (void)benchmark.Advance(true, 0.0);
    WarmUp(benchmark, 50.0);

    CHECK(benchmark.Advance(true, 52.9) == BenchmarkPhase::Running);
    CHECK(benchmark.Advance(true, 53.0) == BenchmarkPhase::Finished);
    CHECK(benchmark.ElapsedSeconds() == doctest::Approx(3.0));
    CHECK(benchmark.Aim().eye.x == doctest::Approx(10.f));

    CHECK(benchmark.Advance(true, 99.0) == BenchmarkPhase::Finished);
    CHECK(benchmark.Aim().eye.x == doctest::Approx(10.f));
}
