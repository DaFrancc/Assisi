/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Runtime/TimeOfDay.hpp>

#include <Assisi/Render/Sky.hpp>

#include <algorithm>
#include <cmath>

namespace Assisi::Runtime
{

namespace
{
/// Clamps to [low, high], substituting @p fallback for a non-finite value.
/// std::clamp alone returns NaN unchanged — both of its comparisons are false —
/// and one NaN in a clock reaches every direction the frame derives from it.
float ClampFiniteClock(float value, float low, float high, float fallback)
{
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

/// @p value brought into [0, @p period), for a positive period. std::fmod keeps
/// the sign of its left operand, which for a negative input is the wrong half of
/// the cycle.
double Wrapped(double value, double period)
{
    if (!std::isfinite(value) || !(period > 0.0))
    {
        return 0.0;
    }
    const double remainder = std::fmod(value, period);
    return remainder < 0.0 ? remainder + period : remainder;
}
} // namespace

TimeOfDay Sanitized(TimeOfDay clock)
{
    const TimeOfDay defaults;
    clock.dayLengthSeconds = ClampFiniteClock(clock.dayLengthSeconds, kMinDayLengthSeconds, kMaxDayLengthSeconds,
                                              defaults.dayLengthSeconds);
    clock.yearLengthDays =
        ClampFiniteClock(clock.yearLengthDays, kMinYearLengthDays, kMaxYearLengthDays, defaults.yearLengthDays);

    clock.hour = std::isfinite(clock.hour) ? Wrapped(clock.hour, 24.0) : defaults.hour;
    // Wrapped on read rather than only on write, because a year shortened after
    // the fact leaves a day of the year past its end and nothing else would
    // notice.
    clock.dayOfYear = Wrapped(clock.dayOfYear, static_cast<double>(clock.yearLengthDays));
    clock.day = std::max(clock.day, 0);
    return clock;
}

Sun Sanitized(Sun sun)
{
    const Sun defaults;
    sun.latitudeDegrees = ClampFiniteClock(sun.latitudeDegrees, Render::kMinLatitudeDegrees,
                                           Render::kMaxLatitudeDegrees, defaults.latitudeDegrees);
    sun.axialTiltDegrees = ClampFiniteClock(sun.axialTiltDegrees, Render::kMinAxialTiltDegrees,
                                            Render::kMaxAxialTiltDegrees, defaults.axialTiltDegrees);
    sun.bearingDegrees = ClampFiniteClock(sun.bearingDegrees, -180.f, 180.f, defaults.bearingDegrees);
    return sun;
}

Moon Sanitized(Moon moon)
{
    const Moon defaults;
    moon.intensity = ClampFiniteClock(moon.intensity, 0.f, Render::kMaxSkyChannel, defaults.intensity);
    moon.color = Assisi::Math::Color3(Render::SanitizedSkyChannels(glm::vec3(moon.color), glm::vec3(defaults.color)));
    moon.sizeDegrees = ClampFiniteClock(moon.sizeDegrees, Render::kMinSunSizeDegrees, Render::kMaxSunSizeDegrees,
                                        defaults.sizeDegrees);
    moon.diskColor =
        Assisi::Math::Color3(Render::SanitizedSkyChannels(glm::vec3(moon.diskColor), glm::vec3(defaults.diskColor)));
    moon.diskIntensity = ClampFiniteClock(moon.diskIntensity, Render::kMinSunDiskIntensity,
                                          Render::kMaxSunDiskIntensity, defaults.diskIntensity);
    moon.atmosphericTint = ClampFiniteClock(moon.atmosphericTint, 0.f, 1.f, defaults.atmosphericTint);

    moon.cycleDays = ClampFiniteClock(moon.cycleDays, Render::kMinMoonCycleDays, kMaxYearLengthDays,
                                      defaults.cycleDays);
    moon.phaseAtEpoch = ClampFiniteClock(moon.phaseAtEpoch, 0.f, 1.f, defaults.phaseAtEpoch);
    moon.inclinationDegrees = ClampFiniteClock(moon.inclinationDegrees, Render::kMinMoonInclinationDegrees,
                                               Render::kMaxMoonInclinationDegrees, defaults.inclinationDegrees);
    moon.nodeAtEpochDegrees = ClampFiniteClock(moon.nodeAtEpochDegrees, -180.f, 180.f, defaults.nodeAtEpochDegrees);
    moon.nodeCycleYears = ClampFiniteClock(moon.nodeCycleYears, Render::kMinNodeCycleYears, kMaxYearLengthDays,
                                           defaults.nodeCycleYears);
    return moon;
}

void AdvanceTimeOfDay(TimeOfDay &clock, float dtSeconds)
{
    if (!std::isfinite(dtSeconds) || dtSeconds <= 0.f)
    {
        return;
    }

    const float dayLength = ClampFiniteClock(clock.dayLengthSeconds, kMinDayLengthSeconds, kMaxDayLengthSeconds,
                                             TimeOfDay{}.dayLengthSeconds);
    const double days = static_cast<double>(dtSeconds) / static_cast<double>(dayLength);

    // Both clocks advance by the same simulated days, each behind its own freeze.
    if (!clock.paused)
    {
        clock.hour += days * 24.0;
        if (clock.hour >= 24.0)
        {
            const double whole = std::floor(clock.hour / 24.0);
            clock.day += static_cast<int32_t>(whole);
            clock.hour -= 24.0 * whole;
        }
    }

    if (!clock.seasonsPaused)
    {
        const float yearLength = ClampFiniteClock(clock.yearLengthDays, kMinYearLengthDays, kMaxYearLengthDays,
                                                  TimeOfDay{}.yearLengthDays);
        clock.dayOfYear = Wrapped(clock.dayOfYear + days, static_cast<double>(yearLength));
    }
}

bool Jump(TimeOfDay &clock, int32_t day, double hour)
{
    if (!std::isfinite(hour))
    {
        return false;
    }

    const double wrapped = Wrapped(hour, 24.0);
    // Whole days carried out of the hour, so Jump(0, 30.0) is tomorrow at six
    // rather than an hour nothing else can read.
    const double carried = std::floor(hour / 24.0);
    clock.day = std::max(day + static_cast<int32_t>(carried), 0);
    clock.hour = wrapped;
    ++clock.jumpSerial;
    return true;
}

bool JumpForwardTo(TimeOfDay &clock, double hour)
{
    if (!std::isfinite(hour))
    {
        return false;
    }
    const double target = Wrapped(hour, 24.0);
    const int32_t day = target > clock.hour ? clock.day : clock.day + 1;
    return Jump(clock, day, target);
}

bool JumpSeason(TimeOfDay &clock, double dayOfYear)
{
    if (!std::isfinite(dayOfYear))
    {
        return false;
    }
    const float yearLength = ClampFiniteClock(clock.yearLengthDays, kMinYearLengthDays, kMaxYearLengthDays,
                                              TimeOfDay{}.yearLengthDays);
    clock.dayOfYear = Wrapped(dayOfYear, static_cast<double>(yearLength));
    ++clock.jumpSerial;
    return true;
}

float SunAngularVelocity(const TimeOfDay &clock)
{
    if (clock.paused)
    {
        return 0.f;
    }
    const float dayLength = ClampFiniteClock(clock.dayLengthSeconds, kMinDayLengthSeconds, kMaxDayLengthSeconds,
                                             TimeOfDay{}.dayLengthSeconds);
    return glm::two_pi<float>() / dayLength;
}

float MoonAngularVelocity(const TimeOfDay &clock, const Moon &moon)
{
    const float sun = SunAngularVelocity(clock);
    if (sun <= 0.f)
    {
        return 0.f;
    }
    // The moon crosses the sky once a day like everything else, less the one turn
    // per lunation it gives up moving east against the stars.
    const float cycleDays = ClampFiniteClock(moon.cycleDays, Render::kMinMoonCycleDays, kMaxYearLengthDays,
                                             Moon{}.cycleDays);
    return sun * std::abs(1.f - 1.f / cycleDays);
}

Render::SkyClock ClockAngles(const TimeOfDay &rawClock)
{
    const TimeOfDay clock = Sanitized(rawClock);
    return Render::SkyClock{.hourAngle = Render::HourAngle(clock.hour),
                            .solarLongitude = Render::SolarLongitude(clock.dayOfYear, clock.yearLengthDays)};
}

Render::Observer ObserverOf(const Sun &rawSun)
{
    const Sun sun = Sanitized(rawSun);
    return Render::Observer{.latitudeRadians = glm::radians(sun.latitudeDegrees),
                            .axialTiltRadians = glm::radians(sun.axialTiltDegrees),
                            .bearingRadians = glm::radians(sun.bearingDegrees)};
}

Render::MoonOrbit OrbitOf(const TimeOfDay &rawClock, const Moon &rawMoon)
{
    const TimeOfDay clock = Sanitized(rawClock);
    const Moon moon = Sanitized(rawMoon);

    // Days as one double rather than a day count and an hour: the phase has to
    // keep its precision past ten million days, and splitting it would round the
    // hour away long before that.
    const double days = static_cast<double>(clock.day) + clock.hour / 24.0;

    const double phase = days / static_cast<double>(moon.cycleDays) + static_cast<double>(moon.phaseAtEpoch);
    const double nodeTurns = days / (static_cast<double>(moon.nodeCycleYears) * static_cast<double>(clock.yearLengthDays));

    return Render::MoonOrbit{
        .inclinationRadians = glm::radians(moon.inclinationDegrees),
        // Regressing: westward along the ecliptic, which is what a real node does
        // and what keeps eclipse seasons from standing on two fixed dates.
        .nodeRadians = static_cast<float>(glm::radians(static_cast<double>(moon.nodeAtEpochDegrees)) -
                                          glm::two_pi<double>() * nodeTurns),
        .elongationRadians = static_cast<float>(glm::two_pi<double>() * phase)};
}

} // namespace Assisi::Runtime
