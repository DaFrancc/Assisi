/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestTimeOfDay.cpp
/// @brief The clock, at both ends of the rate range it has to work across.
///
/// A day/night cycle spanning twenty-four hours down to six seconds has two
/// failure modes that never show at the middle of that range. At the fast end a
/// system that assumed a slow sun falls apart; at the slow end the step gets
/// small enough to disappear into the arithmetic, and the clock silently runs
/// wrong rather than stopping. The second is the reason both clocks are doubles,
/// and the cases below demonstrate the float that would not do rather than
/// asserting the double that does.

#include <doctest/doctest.h>

#include <Assisi/Render/Celestial.hpp>
#include <Assisi/Runtime/TimeOfDay.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

using namespace Assisi::Runtime;

namespace
{
/// One frame at sixty hertz, which is what the fixed step actually is.
constexpr float kTick = 1.f / 60.f;
constexpr float kRealDaySeconds = 86400.f;

/// The declination the clock's annual half currently implies. What a frozen
/// season has to hold still and a running one has to move.
float SinDeclinationOf(const TimeOfDay &clock, float tiltDegrees = 23.44f)
{
    return Assisi::Render::SinDeclination(glm::radians(tiltDegrees),
                                          Assisi::Render::SolarLongitude(clock.dayOfYear, clock.yearLengthDays));
}

void AdvanceFor(TimeOfDay &clock, int32_t ticks)
{
    for (int32_t i = 0; i < ticks; ++i)
    {
        AdvanceTimeOfDay(clock, kTick);
    }
}
} // namespace

TEST_CASE("The daily clock wraps at midnight and carries into the day")
{
    TimeOfDay clock;
    clock.hour = 23.5;
    clock.day = 4;
    clock.dayLengthSeconds = 60.f;

    // A quarter of a minute-long day is six hours, which carries past midnight.
    AdvanceTimeOfDay(clock, 15.f);
    CHECK(clock.day == 5);
    CHECK(clock.hour == doctest::Approx(5.5));

    // A step longer than a whole day carries every day in it rather than one.
    clock.hour = 1.0;
    clock.day = 0;
    AdvanceTimeOfDay(clock, 60.f * 3.f);
    CHECK(clock.day == 3);
    CHECK(clock.hour == doctest::Approx(1.0));
}

TEST_CASE("The annual clock wraps at the year")
{
    TimeOfDay clock;
    clock.dayLengthSeconds = 60.f;
    clock.yearLengthDays = 12.f;
    clock.dayOfYear = 11.5;

    AdvanceTimeOfDay(clock, 60.f);
    CHECK(clock.dayOfYear == doctest::Approx(0.5));

    // A year shortened after the fact leaves the day of the year past its end.
    // Wrapping on read rather than only on write is what stops that reaching the
    // sky as a longitude nobody authored.
    clock.yearLengthDays = 4.f;
    clock.dayOfYear = 9.0;
    CHECK(Sanitized(clock).dayOfYear == doctest::Approx(1.0));
}

TEST_CASE("Nothing advances the clock that should not")
{
    TimeOfDay clock;
    clock.hour = 8.0;
    clock.dayOfYear = 2.0;
    const TimeOfDay before = clock;

    AdvanceTimeOfDay(clock, std::numeric_limits<float>::quiet_NaN());
    AdvanceTimeOfDay(clock, -5.f);
    AdvanceTimeOfDay(clock, 0.f);
    AdvanceTimeOfDay(clock, std::numeric_limits<float>::infinity());
    CHECK(clock.hour == before.hour);
    CHECK(clock.dayOfYear == before.dayOfYear);
    CHECK(clock.day == before.day);

    // A rate change is a plain field write, not a jump and not a step.
    clock.dayLengthSeconds = 6.f;
    CHECK(clock.hour == before.hour);
    CHECK(clock.dayOfYear == before.dayOfYear);
    CHECK(clock.jumpSerial == before.jumpSerial);
}

TEST_CASE("A twenty-four hour day needs a double hour, and a float would run it slow")
{
    // The step at the slowest rate, and a float's spacing in the evening hours.
    // The step is 2.4 of them, so a float rounds every tick down to 2 — and the
    // clock loses roughly a sixth of every evening, invisibly.
    constexpr double kSteps = 216000.0; // one simulated hour at 60 Hz
    TimeOfDay clock;
    clock.hour = 16.0;
    clock.dayLengthSeconds = kRealDaySeconds;

    AdvanceFor(clock, static_cast<int32_t>(kSteps));
    CHECK(clock.hour == doctest::Approx(17.0).epsilon(1e-7));

    float asFloat = 16.f;
    const float step = kTick * 24.f / kRealDaySeconds;
    for (int32_t i = 0; i < static_cast<int32_t>(kSteps); ++i)
    {
        asFloat += step;
    }
    // Falsifiable in the other direction: this is the bug the double avoids, and
    // if a float ever became good enough the assertion would say so.
    CHECK(std::abs(asFloat - 17.f) > 0.1f);
}

TEST_CASE("A twenty-four hour day needs a double day of the year, and a float would not move at all")
{
    // Worse than the hour by two orders: the step is a fortieth of a float's
    // spacing at eight, so every tick rounds to nothing and the year stands still
    // for ever.
    const float step = kTick / kRealDaySeconds;
    float asFloat = 8.f;
    CHECK(asFloat + step == 8.f);

    TimeOfDay clock;
    clock.dayOfYear = 8.0;
    clock.dayLengthSeconds = kRealDaySeconds;
    clock.yearLengthDays = 365.f;

    AdvanceFor(clock, 6000); // a hundred simulated seconds
    CHECK(clock.dayOfYear - 8.0 == doctest::Approx(100.0 / 86400.0).epsilon(1e-6));
    CHECK(clock.dayOfYear > 8.0);
}

TEST_CASE("Every rate from a real day to six seconds behaves the same way")
{
    // No branch anywhere reads the rate, so a day is a day at every one of these
    // and the angular velocity times the period is one turn at all of them.
    for (const float dayLength : {86400.f, 1800.f, 600.f, 60.f, 6.f})
    {
        CAPTURE(dayLength);
        TimeOfDay clock;
        clock.hour = 7.25;
        clock.day = 2;
        clock.dayOfYear = 1.0;
        clock.dayLengthSeconds = dayLength;

        AdvanceTimeOfDay(clock, dayLength);
        CHECK(clock.hour == doctest::Approx(7.25).epsilon(1e-9));
        CHECK(clock.day == 3);
        CHECK(clock.dayOfYear == doctest::Approx(2.0).epsilon(1e-9));

        CHECK(SunAngularVelocity(clock) * dayLength == doctest::Approx(glm::two_pi<float>()).epsilon(1e-5));
    }

    TimeOfDay paused;
    paused.paused = true;
    CHECK(SunAngularVelocity(paused) == 0.f);
    CHECK(MoonAngularVelocity(paused, Moon{}) == 0.f);

    // The moon crosses the sky once a day less the turn it gives up per lunation,
    // so it is slower than the sun and by a knowable amount.
    TimeOfDay running;
    const Moon moon;
    CHECK(MoonAngularVelocity(running, moon) < SunAngularVelocity(running));
    CHECK(MoonAngularVelocity(running, moon) ==
          doctest::Approx(SunAngularVelocity(running) * (1.f - 1.f / moon.cycleDays)));
}

TEST_CASE("The two freezes are independent, so a frozen season can sit under a running day")
{
    // A noon shadow swinging through the year, or a season held while the day
    // runs. Both are authoring views rather than errors, and they are what the
    // day of the year being its own state buys.
    TimeOfDay frozenSeason;
    frozenSeason.dayLengthSeconds = 60.f;
    frozenSeason.yearLengthDays = 12.f;
    frozenSeason.dayOfYear = 3.0;
    frozenSeason.seasonsPaused = true;
    const float declinationBefore = SinDeclinationOf(frozenSeason);

    AdvanceFor(frozenSeason, 60 * 60 * 7); // a simulated week
    // Bitwise: the declination is a pure function of a value nothing wrote.
    CHECK(SinDeclinationOf(frozenSeason) == declinationBefore);
    CHECK(frozenSeason.day == 7);

    TimeOfDay frozenDay;
    frozenDay.dayLengthSeconds = 60.f;
    frozenDay.yearLengthDays = 12.f;
    frozenDay.hour = 12.0;
    frozenDay.paused = true;
    const float declinationAtNoon = SinDeclinationOf(frozenDay);

    AdvanceFor(frozenDay, 60 * 60 * 3);
    CHECK(frozenDay.hour == 12.0);
    CHECK(frozenDay.day == 0);
    CHECK(SinDeclinationOf(frozenDay) != declinationAtNoon);
}

TEST_CASE("A jump is counted as a cut, and a step never is")
{
    TimeOfDay clock;
    clock.hour = 13.0;
    clock.day = 3;
    const uint32_t before = clock.jumpSerial;

    AdvanceFor(clock, 600);
    CHECK(clock.jumpSerial == before);

    CHECK(Jump(clock, 5, 6.0));
    CHECK(clock.day == 5);
    CHECK(clock.hour == doctest::Approx(6.0));
    CHECK(clock.jumpSerial == before + 1);

    // Whole days carried out of the hour, so a caller need not do the arithmetic
    // to say "thirty hours from the start of day five".
    CHECK(Jump(clock, 5, 30.0));
    CHECK(clock.day == 6);
    CHECK(clock.hour == doctest::Approx(6.0));

    // A negative hour lands in the right half of the previous cycle rather than
    // in the wrong one, which is what fmod alone would give.
    CHECK(Jump(clock, 5, -1.0));
    CHECK(clock.hour == doctest::Approx(23.0));

    const TimeOfDay untouched = clock;
    CHECK_FALSE(Jump(clock, 2, std::numeric_limits<double>::quiet_NaN()));
    CHECK(clock.hour == untouched.hour);
    CHECK(clock.day == untouched.day);
    CHECK(clock.jumpSerial == untouched.jumpSerial);
}

TEST_CASE("Sleeping until morning goes forward, never backward")
{
    TimeOfDay night;
    night.hour = 23.0;
    night.day = 4;
    CHECK(JumpForwardTo(night, 6.0));
    CHECK(night.day == 5);
    CHECK(night.hour == doctest::Approx(6.0));

    TimeOfDay earlyMorning;
    earlyMorning.hour = 5.0;
    earlyMorning.day = 4;
    CHECK(JumpForwardTo(earlyMorning, 6.0));
    CHECK(earlyMorning.day == 4);
    CHECK(earlyMorning.hour == doctest::Approx(6.0));

    CHECK_FALSE(JumpForwardTo(earlyMorning, std::numeric_limits<double>::infinity()));
}

TEST_CASE("A season cut is a cut, and wraps into the year")
{
    TimeOfDay clock;
    clock.yearLengthDays = 12.f;
    const uint32_t before = clock.jumpSerial;

    CHECK(JumpSeason(clock, 15.0));
    CHECK(clock.dayOfYear == doctest::Approx(3.0));
    // The same serial as an hour cut: a season cut moves the sun as far as one,
    // and a consumer holding history has to forget for the same reason.
    CHECK(clock.jumpSerial == before + 1);

    CHECK(JumpSeason(clock, -1.0));
    CHECK(clock.dayOfYear == doctest::Approx(11.0));

    const double held = clock.dayOfYear;
    CHECK_FALSE(JumpSeason(clock, std::numeric_limits<double>::quiet_NaN()));
    CHECK(clock.dayOfYear == held);
}

TEST_CASE("A hand-edited clock is brought back into range")
{
    TimeOfDay clock;
    clock.hour = std::numeric_limits<double>::quiet_NaN();
    clock.day = -3;
    clock.dayLengthSeconds = 0.f;
    clock.yearLengthDays = std::numeric_limits<float>::infinity();
    clock.dayOfYear = -2.0;

    const TimeOfDay safe = Sanitized(clock);
    CHECK(safe.hour == doctest::Approx(TimeOfDay{}.hour));
    CHECK(safe.day == 0);
    CHECK(safe.dayLengthSeconds == doctest::Approx(kMinDayLengthSeconds));
    CHECK(safe.yearLengthDays == doctest::Approx(TimeOfDay{}.yearLengthDays));
    CHECK(safe.dayOfYear >= 0.0);

    Sun sun;
    sun.latitudeDegrees = 400.f;
    sun.axialTiltDegrees = -10.f;
    sun.bearingDegrees = std::numeric_limits<float>::quiet_NaN();
    const Sun safeSun = Sanitized(sun);
    // Ninety, not eighty-nine: the pole is a real place and the direction formula
    // is finite there.
    CHECK(safeSun.latitudeDegrees == doctest::Approx(Assisi::Render::kMaxLatitudeDegrees));
    CHECK(safeSun.axialTiltDegrees == doctest::Approx(0.f));
    CHECK(safeSun.bearingDegrees == doctest::Approx(Sun{}.bearingDegrees));

    Moon moon;
    moon.cycleDays = 0.1f;
    moon.inclinationDegrees = 120.f;
    moon.phaseAtEpoch = 4.f;
    moon.nodeCycleYears = 0.f;
    const Moon safeMoon = Sanitized(moon);
    CHECK(safeMoon.cycleDays == doctest::Approx(Assisi::Render::kMinMoonCycleDays));
    // Held a degree off a right angle, because at ninety the orbit would carry the
    // moon through the ecliptic pole, which is where its image's up comes from.
    CHECK(safeMoon.inclinationDegrees == doctest::Approx(Assisi::Render::kMaxMoonInclinationDegrees));
    CHECK(safeMoon.phaseAtEpoch == doctest::Approx(1.f));
    CHECK(safeMoon.nodeCycleYears == doctest::Approx(Assisi::Render::kMinNodeCycleYears));
}

TEST_CASE("The moon's node regresses as the days pass")
{
    TimeOfDay clock;
    clock.yearLengthDays = 12.f;
    // Midnight of day zero, because the clock's own epoch is noon and half a day
    // of regression would otherwise be folded into the reading.
    clock.hour = 0.0;
    Moon moon;
    moon.nodeCycleYears = 1.f; // one turn per year, so a year of days is a full turn

    const Assisi::Render::MoonOrbit atEpoch = OrbitOf(clock, moon);
    CHECK(atEpoch.nodeRadians == doctest::Approx(glm::radians(moon.nodeAtEpochDegrees)));

    clock.day = 3; // a quarter of this year
    const Assisi::Render::MoonOrbit later = OrbitOf(clock, moon);
    // Westward — the sense a real node takes, and the whole reason eclipse seasons
    // walk through the year instead of standing on two dates.
    CHECK(later.nodeRadians < atEpoch.nodeRadians);
    CHECK(atEpoch.nodeRadians - later.nodeRadians == doctest::Approx(glm::half_pi<float>()).epsilon(1e-4));

    // The elongation is the phase, and a full cycle of days returns it.
    clock.day = 0;
    clock.hour = 0.0;
    moon.phaseAtEpoch = 0.f;
    CHECK(OrbitOf(clock, moon).elongationRadians == doctest::Approx(0.f));
    clock.day = static_cast<int32_t>(moon.cycleDays);
    clock.hour = 24.0 * static_cast<double>(moon.cycleDays - std::floor(moon.cycleDays));
    CHECK(OrbitOf(clock, moon).elongationRadians == doctest::Approx(glm::two_pi<float>()).epsilon(1e-4));
}
