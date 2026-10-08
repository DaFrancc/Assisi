/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/App/FixedStepClock.hpp>

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

using namespace Assisi::App;

namespace
{

/// The rate both physics and the display run at in these tests. Step and
/// interval are computed from it by the same expression, so a snapped frame is
/// exactly one step.
constexpr double kRateHz = 60.0;

/// The most a measured frame boundary strays from the display's grid: well
/// inside the snap tolerance, and large enough that the raw times cross step
/// boundaries.
constexpr double kJitterSeconds = 0.0004;

/// Enough frames to cross a step boundary many times over when unsnapped.
constexpr uint32_t kFrames = 2000;

/// Enough frames for a 59.94 Hz display reported as 60 Hz to drift by well
/// over a second if nothing pays the drift back.
constexpr uint32_t kLongRunFrames = 100000;

/// The cap the engine ships with.
constexpr uint32_t kMaxSteps = 8;

/// The time between consecutive frame boundaries on a grid of @p intervalSeconds,
/// each boundary moved by up to kJitterSeconds either way. Measured from
/// timestamps rather than jittered per frame, as a real present cadence is: a
/// late boundary makes one frame long and the next short.
std::vector<double> GridFrameTimes(double intervalSeconds, uint32_t frames)
{
    std::minstd_rand random(1u);
    std::uniform_real_distribution<double> jitter(-kJitterSeconds, kJitterSeconds);

    std::vector<double> times;
    times.reserve(frames);
    double previous = 0.0;
    for (uint32_t i = 1; i <= frames; ++i)
    {
        const double boundary = static_cast<double>(i) * intervalSeconds + jitter(random);
        times.push_back(boundary - previous);
        previous = boundary;
    }
    return times;
}

} // namespace

TEST_CASE("FixedStepClock: frame times wobbling around the refresh interval run exactly one step a frame")
{
    const double step = 1.0 / kRateHz;
    const std::vector<double> times = GridFrameTimes(1.0 / kRateHz, kFrames);

    FrameTimeSnapper snapper;
    FixedStepClock snapped(step, kMaxSteps);
    FixedStepClock raw(step, kMaxSteps);
    bool rawVaried = false;
    for (const double measured : times)
    {
        CHECK(snapped.Advance(snapper.Snap(measured, step)) == 1);
        CHECK(snapped.Alpha() == 0.0);
        rawVaried = rawVaried || raw.Advance(measured) != 1;
    }

    // The same times unsnapped do not run one step a frame, or the check above
    // would pass without snapping doing anything.
    CHECK(rawVaried);
}

TEST_CASE("FixedStepClock: frame times near twice the interval run exactly two steps a frame")
{
    const double step = 1.0 / kRateHz;
    const std::vector<double> times = GridFrameTimes(2.0 / kRateHz, kFrames);

    FrameTimeSnapper snapper;
    FixedStepClock clock(step, kMaxSteps);
    for (const double measured : times)
    {
        CHECK(clock.Advance(snapper.Snap(measured, step)) == 2);
    }
}

TEST_CASE("FixedStepClock: a display slower than reported keeps simulated time within a step of real time")
{
    // A 59.94 Hz display reported as 60 Hz: every frame snaps to 1/60 while
    // lasting 1/59.94, and without the drift paid back the game clock would
    // fall behind by over a second across the run.
    constexpr double kActualHz = 59.94;
    const double step = 1.0 / kRateHz;
    const std::vector<double> times = GridFrameTimes(1.0 / kActualHz, kLongRunFrames);

    FrameTimeSnapper snapper;
    FixedStepClock clock(step, kMaxSteps);
    double realSeconds = 0.0;
    uint64_t steps = 0;
    for (const double measured : times)
    {
        realSeconds += measured;
        steps += clock.Advance(snapper.Snap(measured, step));
    }

    const double simulatedSeconds = (static_cast<double>(steps) + clock.Alpha()) * step;
    CHECK(std::abs(simulatedSeconds - realSeconds) <= FrameTimeSnapper::DriftDeadZoneSeconds + 1e-9);
    CHECK(std::abs(simulatedSeconds - realSeconds) < step);
}

TEST_CASE("FixedStepClock: with no interval, or off the grid, the measured time passes through")
{
    const double step = 1.0 / kRateHz;
    FrameTimeSnapper snapper;

    // Unlimited frame rate, or snapping turned off: no interval is known.
    CHECK(snapper.Snap(0.0171, 0.0) == 0.0171);

    // A hitch is real time, and leaves no drift behind: the next frame on the
    // grid is snapped to exactly the interval.
    CHECK(snapper.Snap(0.040, step) == 0.040);
    CHECK(snapper.DriftSeconds() == 0.0);
    CHECK(snapper.Snap(step, step) == step);
}

TEST_CASE("FixedStepClock: unsnapped time steps as the raw accumulator does")
{
    // The same arithmetic the loop always ran: one step per whole step held.
    const double step = 1.0 / kRateHz;
    FixedStepClock clock(step, kMaxSteps);

    CHECK(clock.Advance(step * 0.5) == 0);
    CHECK(clock.Alpha() == doctest::Approx(0.5));
    CHECK(clock.Advance(step * 0.75) == 1);
    CHECK(clock.Alpha() == doctest::Approx(0.25));
    CHECK(clock.Advance(step * 2.0) == 2);
    CHECK(clock.Alpha() == doctest::Approx(0.25));
}

TEST_CASE("FixedStepClock: past the cap the time is dropped, not carried")
{
    const double step = 1.0 / kRateHz;
    FixedStepClock clock(step, kMaxSteps);

    CHECK(clock.Advance(1.0) == kMaxSteps);
    CHECK(clock.Alpha() < 1.0);

    // Carried, the next ordinary frame would run the cap again.
    CHECK(clock.Advance(step) == 1);
}
