/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCelestial.cpp
/// @brief Where the sun and the moon are, checked against what a sky actually
/// does.
///
/// The frames here compose four deep and every stage has a sign in it, so a wrong
/// one is visually plausible — a moon merely on the other side of the ecliptic, a
/// summer that happens in December. Nothing on screen catches that. What catches
/// it is asserting the consequences: the day is twelve hours long at every
/// latitude on an equinox, the noon sun is as high as the latitude and the
/// declination say, the full moon is exactly opposite the sun, and the year at
/// the Arctic Circle contains a day with no sunrise in it.
///
/// Where a quantity can be reached two ways, both are computed and compared. A
/// formula asserting its own output proves nothing.

#include <doctest/doctest.h>

#include <Assisi/Math/Angles.hpp>
#include <Assisi/Render/Celestial.hpp>

#include <cmath>
#include <cstdint>

using namespace Assisi::Render;

namespace
{
/// Samples per simulated day for every hour-by-hour sweep below: one a minute.
/// Fine enough that a day length counted by sampling agrees with the closed form
/// to a hundredth of an hour, which is what makes the two-route check tight
/// enough to catch a sign rather than merely a typo.
constexpr int32_t kMinutesPerDay = 24 * 60;

/// @name Earth's own numbers, which most of these cases are posed against
///
/// Named because they recur and because the expected values below — a 15.42-hour
/// midsummer at forty-five degrees, a sunrise swinging 34.2 degrees, an eclipse
/// season that walks — are all consequences of exactly these. Change one and the
/// oracle changes with it, which is the point of not spelling them inline.
/// @{
constexpr float kEarthTiltDegrees = 23.44f;
constexpr float kMoonInclinationDegrees = 5.15f;
constexpr float kSynodicMonthDays = 29.53f;
constexpr float kNodeCycleYears = 18.6f;
/// @}

/// A node longitude with nothing special about it. Cases that are not about the
/// node use this so that a result which quietly depended on one would show.
constexpr float kArbitraryNodeDegrees = 61.0f;

/// How far above the horizon a body has to be before a sample counts as "up" in
/// the sweeps that follow the moon across the sky. Clear of the horizon, where
/// the direction is changing fastest against the sampling.
constexpr float kWellUp = 0.1f;

Observer ObserverAt(float latitudeDegrees, float tiltDegrees = kEarthTiltDegrees, float bearingDegrees = 0.0f)
{
    return Observer{.latitudeRadians = glm::radians(latitudeDegrees),
                    .axialTiltRadians = glm::radians(tiltDegrees),
                    .bearingRadians = glm::radians(bearingDegrees)};
}

SkyClock ClockAt(double hour, double dayOfYear, float yearLengthDays)
{
    return SkyClock{.hourAngle = HourAngle(hour), .solarLongitude = SolarLongitude(dayOfYear, yearLengthDays)};
}

MoonOrbit OrbitOf(float inclinationDegrees, float nodeDegrees, float phaseTurns)
{
    return MoonOrbit{.inclinationRadians = glm::radians(inclinationDegrees),
                     .nodeRadians = glm::radians(nodeDegrees),
                     .elongationRadians =
                         static_cast<float>(glm::two_pi<double>() * static_cast<double>(phaseTurns))};
}

float ElevationDegrees(const glm::vec3 &direction)
{
    return glm::degrees(std::asin(std::clamp(direction.y, -1.0f, 1.0f)));
}

/// Bearing of a direction measured north of east, which is how a sunrise point is
/// quoted: zero is due east, positive swings toward the north.
float AzimuthFromEastDegrees(const glm::vec3 &direction)
{
    return glm::degrees(std::atan2(-direction.z, direction.x));
}

float LengthOf(const glm::vec3 &v)
{
    return std::sqrt(glm::dot(v, v));
}

/// Hours the sun spends above the horizon, counted rather than computed. The
/// independent route @ref DaylightHours is checked against.
float SampledDaylightHours(const Observer &observer, double dayOfYear, float yearLengthDays)
{
    int32_t above = 0;
    for (int32_t minute = 0; minute < kMinutesPerDay; ++minute)
    {
        const double hour = 24.0 * static_cast<double>(minute) / static_cast<double>(kMinutesPerDay);
        if (SunDirection(ClockAt(hour, dayOfYear, yearLengthDays), observer).y > 0.0f)
        {
            ++above;
        }
    }
    return 24.0f * static_cast<float>(above) / static_cast<float>(kMinutesPerDay);
}

/// The "up" a horizon-locked or screen-locked disk image would use: the zenith
/// projected tangent to the sky. Present only as the negative control — it is what
/// MoonImageUp exists instead of, and the test below shows why.
glm::vec3 HorizonLockedUp(const glm::vec3 &toBody)
{
    const glm::vec3 tangent = glm::vec3(0.0f, 1.0f, 0.0f) - toBody * toBody.y;
    const float length = LengthOf(tangent);
    return length > 0.0f ? tangent / length : glm::vec3(0.0f, 0.0f, -1.0f);
}
} // namespace

TEST_CASE("the equinox sun rises due east, sets due west and reaches the latitude's complement at noon")
{
    // Day zero of any year is the March equinox, so the declination is zero
    // whatever the tilt: the whole point of measuring the year from there.
    constexpr float kYear = 12.0f;

    for (const float latitude : {0.0f, 45.0f, 67.0f, 80.0f, 90.0f})
    {
        CAPTURE(latitude);
        const Observer observer = ObserverAt(latitude);

        const glm::vec3 sunrise = SunDirection(ClockAt(6.0, 0.0, kYear), observer);
        CHECK(sunrise.x == doctest::Approx(1.0f).epsilon(1e-5));
        CHECK(sunrise.y == doctest::Approx(0.0f).epsilon(1e-5));
        CHECK(sunrise.z == doctest::Approx(0.0f).epsilon(1e-5));

        const glm::vec3 sunset = SunDirection(ClockAt(18.0, 0.0, kYear), observer);
        CHECK(sunset.x == doctest::Approx(-1.0f).epsilon(1e-5));
        CHECK(sunset.y == doctest::Approx(0.0f).epsilon(1e-5));

        const glm::vec3 noon = SunDirection(ClockAt(12.0, 0.0, kYear), observer);
        CHECK(ElevationDegrees(noon) == doctest::Approx(90.0f - std::abs(latitude)).epsilon(1e-4));

        // Unit length is an identity of the orthonormal frame, not something a
        // normalise restores — so it holds at the pole, where the frame is most
        // nearly degenerate.
        for (const double hour : {0.0, 3.0, 6.0, 11.0, 17.0, 23.0})
        {
            CHECK(LengthOf(SunDirection(ClockAt(hour, 0.0, kYear), observer)) ==
                  doctest::Approx(1.0f).epsilon(1e-5));
        }
    }
}

TEST_CASE("an equinox is twelve hours of daylight at every latitude")
{
    constexpr float kYear = 12.0f;
    for (const float latitude : {0.0f, 30.0f, 45.0f, 67.0f, 80.0f, 89.0f})
    {
        CAPTURE(latitude);
        // Both equinoxes: a declination leaking in at longitude zero would be
        // caught by the first, and one leaking in with the wrong sign by the
        // second.
        CHECK(SampledDaylightHours(ObserverAt(latitude), 0.0, kYear) == doctest::Approx(12.0f).epsilon(0.005));
        CHECK(SampledDaylightHours(ObserverAt(latitude), 6.0, kYear) == doctest::Approx(12.0f).epsilon(0.005));
    }
}

TEST_CASE("noon elevation is the latitude against the declination, over a lattice of both")
{
    constexpr float kYear = 360.0f;
    // Including the tropic, where the sun stands exactly overhead at one solstice
    // and the elevation formula's absolute value changes which side it takes.
    for (const float latitude : {-45.0f, 0.0f, kEarthTiltDegrees, 45.0f, 67.0f, 80.0f})
    {
        for (const int32_t dayIndex : {0, 30, 90, 150, 180, 270, 359})
        {
            CAPTURE(latitude);
            CAPTURE(dayIndex);
            const Observer observer = ObserverAt(latitude);
            const double dayOfYear = static_cast<double>(dayIndex);
            const float sinDec = SinDeclination(observer.axialTiltRadians, SolarLongitude(dayOfYear, kYear));
            const float declination = glm::degrees(std::asin(std::clamp(sinDec, -1.0f, 1.0f)));

            const glm::vec3 noon = SunDirection(ClockAt(12.0, dayOfYear, kYear), observer);
            CHECK(ElevationDegrees(noon) == doctest::Approx(90.0f - std::abs(latitude - declination)).epsilon(1e-4));
        }
    }
}

TEST_CASE("day length at the solstices agrees between the closed form and counting the hours")
{
    // A long year so a solstice is a whole simulated day rather than a moment the
    // sampling steps over.
    constexpr float kYear = 360.0f;
    constexpr double kMidsummer = 90.0;
    constexpr double kMidwinter = 270.0;

    struct Case
    {
        float latitude;
        double dayOfYear;
        float hours;
    };
    // 15.42 and 8.58 at forty-five degrees, and the two saturated ends at the
    // Arctic Circle: the sun that does not set and the sun that does not rise.
    const Case cases[] = {
        {45.0f, kMidsummer, 15.42f}, {45.0f, kMidwinter, 8.58f},   {67.0f, kMidsummer, 24.0f},
        {67.0f, kMidwinter, 0.0f},   {-45.0f, kMidsummer, 8.58f},  {0.0f, kMidsummer, 12.0f},
    };

    for (const Case &item : cases)
    {
        CAPTURE(item.latitude);
        CAPTURE(item.dayOfYear);
        const Observer observer = ObserverAt(item.latitude);
        const float sinDec = SinDeclination(observer.axialTiltRadians, SolarLongitude(item.dayOfYear, kYear));

        const float closedForm = DaylightHours(observer.latitudeRadians, sinDec);
        const float counted = SampledDaylightHours(observer, item.dayOfYear, kYear);

        CHECK(closedForm == doctest::Approx(item.hours).epsilon(0.005));
        CHECK(counted == doctest::Approx(closedForm).epsilon(0.005));
    }
}

TEST_CASE("polar night and the midnight sun are steady states, not moments")
{
    // A very long year holds the declination near its extreme across the ten days
    // sampled, which is what makes this a claim about a season rather than about
    // one day.
    constexpr float kYear = 3650.0f;
    const Observer observer = ObserverAt(80.0f);

    for (int32_t offset = -5; offset <= 5; ++offset)
    {
        const double summerDay = 0.25 * static_cast<double>(kYear) + static_cast<double>(offset);
        const double winterDay = 0.75 * static_cast<double>(kYear) + static_cast<double>(offset);
        CAPTURE(offset);

        for (int32_t hour = 0; hour < 24; ++hour)
        {
            CAPTURE(hour);
            const double h = static_cast<double>(hour);
            CHECK(SunDirection(ClockAt(h, summerDay, kYear), observer).y > 0.0f);
            CHECK(SunDirection(ClockAt(h, winterDay, kYear), observer).y < 0.0f);
        }

        CHECK(DaylightHours(observer.latitudeRadians,
                            SinDeclination(observer.axialTiltRadians, SolarLongitude(summerDay, kYear))) ==
              doctest::Approx(24.0f));
        CHECK(DaylightHours(observer.latitudeRadians,
                            SinDeclination(observer.axialTiltRadians, SolarLongitude(winterDay, kYear))) ==
              doctest::Approx(0.0f));
    }
}

TEST_CASE("the sunrise point swings north and south through the year and comes back")
{
    constexpr float kYear = 360.0f;
    const Observer observer = ObserverAt(45.0f);

    auto sunriseAzimuth = [&](double dayOfYear) {
        for (int32_t minute = 1; minute < kMinutesPerDay; ++minute)
        {
            const double hour = 24.0 * static_cast<double>(minute) / static_cast<double>(kMinutesPerDay);
            const glm::vec3 previous =
                SunDirection(ClockAt(hour - 24.0 / static_cast<double>(kMinutesPerDay), dayOfYear, kYear), observer);
            const glm::vec3 current = SunDirection(ClockAt(hour, dayOfYear, kYear), observer);
            if (previous.y <= 0.0f && current.y > 0.0f)
            {
                return AzimuthFromEastDegrees(current);
            }
        }
        FAIL("no sunrise found");
        return 0.0f;
    };

    // The amplitude is asin(sin dec / cos lat) — 34.2 degrees at forty-five with
    // Earth's tilt. Due east at both equinoxes, and it returns to due east on the
    // year's wrap rather than drifting, which is what pins the modulo.
    //
    // Half a degree absolute, not a relative epsilon: the sample taken is the
    // first minute the sun is ALREADY up, by which time it has moved a fifth of a
    // degree along the horizon. That offset is the sampling's, and against a
    // thirty-four degree swing it decides nothing.
    CHECK(std::abs(sunriseAzimuth(0.0) - 0.0f) < 0.5f);
    CHECK(std::abs(sunriseAzimuth(90.0) - 34.2f) < 0.5f);
    CHECK(std::abs(sunriseAzimuth(180.0) - 0.0f) < 0.5f);
    CHECK(std::abs(sunriseAzimuth(270.0) + 34.2f) < 0.5f);
    CHECK(std::abs(sunriseAzimuth(360.0) - 0.0f) < 0.5f);
}

TEST_CASE("a world with no axial tilt reproduces the untilted arc bit for bit")
{
    constexpr float kYear = 12.0f;
    for (const float latitude : {-30.0f, 0.0f, 45.0f, 80.0f})
    {
        const Observer observer = ObserverAt(latitude, 0.0f);
        const float sinPhi = std::sin(observer.latitudeRadians);
        const float cosPhi = std::cos(observer.latitudeRadians);

        for (const int32_t dayIndex : {0, 3, 7, 11})
        {
            for (const int32_t hourIndex : {0, 4, 9, 13, 20})
            {
                CAPTURE(latitude);
                CAPTURE(dayIndex);
                CAPTURE(hourIndex);
                const SkyClock clock = ClockAt(static_cast<double>(hourIndex), static_cast<double>(dayIndex), kYear);
                const glm::vec3 actual = SunDirection(clock, observer);

                // Exact, not approximate: at zero tilt the declination's sine is
                // an exact zero and its cosine an exact one, so the seasonal
                // terms drop out of the arithmetic rather than rounding away.
                const float sinH = std::sin(clock.hourAngle);
                const float cosH = std::cos(clock.hourAngle);
                CHECK(actual.x == -sinH);
                CHECK(actual.y == cosH * cosPhi);
                CHECK(actual.z == cosH * sinPhi);
            }
        }
    }
}

TEST_CASE("a bearing turns the whole sky about up and changes no elevation")
{
    constexpr float kYear = 12.0f;
    constexpr float kBearing = 37.0f;
    const Observer plain = ObserverAt(45.0f, kEarthTiltDegrees, 0.0f);
    const Observer turned = ObserverAt(45.0f, kEarthTiltDegrees, kBearing);

    for (const double hour : {1.0, 6.0, 9.5, 12.0, 15.0, 21.0})
    {
        CAPTURE(hour);
        const glm::vec3 a = SunDirection(ClockAt(hour, 3.0, kYear), plain);
        const glm::vec3 b = SunDirection(ClockAt(hour, 3.0, kYear), turned);

        CHECK(b.y == doctest::Approx(a.y).epsilon(1e-5));

        // Compared as a difference brought into (-180, 180] rather than as two
        // azimuths, so a sample that happens to straddle due west is not a
        // failure about the wrap.
        float swept = AzimuthFromEastDegrees(b) - AzimuthFromEastDegrees(a);
        swept -= 360.0f * std::round(swept / 360.0f);
        CHECK(swept == doctest::Approx(kBearing).epsilon(1e-4));
    }
}

TEST_CASE("the sun sweeps its arc at a constant rate")
{
    constexpr float kYear = 100000.0f; // effectively a frozen declination
    const Observer observer = ObserverAt(45.0f);

    float first = 0.0f;
    for (int32_t step = 0; step < 48; ++step)
    {
        const double hour = 0.5 * static_cast<double>(step);
        const glm::vec3 a = SunDirection(ClockAt(hour, 40.0, kYear), observer);
        const glm::vec3 b = SunDirection(ClockAt(hour + 0.5, 40.0, kYear), observer);
        const float turned = Assisi::Math::AngleBetween(a, b);
        if (step == 0)
        {
            first = turned;
            CHECK(first > 0.0f);
        }
        CAPTURE(step);
        CHECK(turned == doctest::Approx(first).epsilon(1e-4));
    }
}

TEST_CASE("an uninclined moon lies on the ecliptic, and a full one is exactly opposite the sun")
{
    constexpr float kYear = 12.0f;
    for (const float latitude : {0.0f, 45.0f, 80.0f})
    {
        for (const float tilt : {0.0f, kEarthTiltDegrees, 60.0f})
        {
            for (const double hour : {2.0, 7.5, 13.0, 19.25})
            {
                for (const double day : {0.0, 2.5, 8.0})
                {
                    CAPTURE(latitude);
                    CAPTURE(tilt);
                    CAPTURE(hour);
                    CAPTURE(day);
                    const Observer observer = ObserverAt(latitude, tilt);
                    const SkyClock clock = ClockAt(hour, day, kYear);
                    const glm::vec3 pole = EclipticPole(clock, observer);

                    const glm::vec3 flat = MoonDirection(clock, observer, OrbitOf(0.0f, kArbitraryNodeDegrees,0.3f));
                    CHECK(glm::dot(flat, pole) == doctest::Approx(0.0f).epsilon(1e-5));

                    // Both bodies on the ecliptic and half a turn apart is what a
                    // full moon is. It catches an obliquity applied to one and
                    // not the other, which nothing on screen would.
                    const glm::vec3 full = MoonDirection(clock, observer, OrbitOf(0.0f, kArbitraryNodeDegrees,0.5f));
                    CHECK(glm::dot(SunDirection(clock, observer), full) == doctest::Approx(-1.0f).epsilon(1e-5));
                }
            }
        }
    }
}

TEST_CASE("with neither obliquity nor inclination the moon is the sun, lagging by its phase")
{
    constexpr float kYear = 12.0f;
    const Observer observer = ObserverAt(38.0f, 0.0f);

    for (const float phase : {0.0f, 0.125f, 0.25f, 0.5f, 0.75f, 0.9f})
    {
        for (const double hour : {0.0, 5.0, 11.0, 16.5, 22.0})
        {
            CAPTURE(phase);
            CAPTURE(hour);
            const glm::vec3 moon = MoonDirection(ClockAt(hour, 4.0, kYear), observer, OrbitOf(0.0f, kArbitraryNodeDegrees, phase));
            // The elongation is the lag: a full moon rises as the sun sets
            // because it is exactly twelve hours behind it.
            const glm::vec3 laggedSun =
                SunDirection(ClockAt(hour - 24.0 * static_cast<double>(phase), 4.0, kYear), observer);
            CHECK(glm::dot(moon, laggedSun) == doctest::Approx(1.0f).epsilon(1e-5));
        }
    }
}

TEST_CASE("the lit fraction is the elongation, reaching new and full where the geometry says")
{
    constexpr float kYear = 12.0f;
    const Observer observer = ObserverAt(45.0f);
    const SkyClock clock = ClockAt(9.0, 5.0, kYear);
    const glm::vec3 sun = SunDirection(clock, observer);

    CHECK(MoonLitFraction(sun, MoonDirection(clock, observer, OrbitOf(0.0f, 90.0f, 0.0f))) ==
          doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(MoonLitFraction(sun, MoonDirection(clock, observer, OrbitOf(0.0f, 90.0f, 0.5f))) ==
          doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(MoonLitFraction(sun, MoonDirection(clock, observer, OrbitOf(0.0f, 90.0f, 0.25f))) ==
          doctest::Approx(0.5f).epsilon(1e-4));

    // An inclined orbit cannot reach either extreme exactly, and the shortfall is
    // (1 - cos i) / 2 — 0.002 at the moon's own inclination. That bound is why a
    // total eclipse is not the same event as a new moon.
    const float shortfall = 0.5f * (1.0f - std::cos(glm::radians(kMoonInclinationDegrees)));
    const float newMoon = MoonLitFraction(sun, MoonDirection(clock, observer, OrbitOf(kMoonInclinationDegrees, kArbitraryNodeDegrees,0.0f)));
    const float fullMoon = MoonLitFraction(sun, MoonDirection(clock, observer, OrbitOf(kMoonInclinationDegrees, kArbitraryNodeDegrees,0.5f)));
    CHECK(newMoon >= 0.0f);
    CHECK(newMoon <= shortfall + 1e-5f);
    CHECK(fullMoon <= 1.0f);
    CHECK(fullMoon >= 1.0f - shortfall - 1e-5f);
}

TEST_CASE("a regressing node makes close alignments rare and a fixed one makes them impossible")
{
    constexpr float kYear = 12.0f;
    const Observer observer = ObserverAt(45.0f);

    // The moon's ecliptic latitude at each of two hundred new moons. What matters
    // is the spread: a node that moves visits every alignment eventually, and one
    // that stands still visits exactly one for ever.
    auto latitudesAtNewMoon = [&](float cycleDays, float nodeCycleYears, float &minimum, float &maximum) {
        minimum = 180.0f;
        maximum = 0.0f;
        for (int32_t index = 0; index < 200; ++index)
        {
            const double days = static_cast<double>(index) * static_cast<double>(cycleDays);
            const float node =
                kArbitraryNodeDegrees - 360.0f * static_cast<float>(days / (static_cast<double>(nodeCycleYears) *
                                                                            static_cast<double>(kYear)));
            const SkyClock clock = ClockAt(0.0, days, kYear);
            const glm::vec3 moon = MoonDirection(clock, observer, OrbitOf(kMoonInclinationDegrees, node, 0.0f));
            const float beta = std::abs(glm::degrees(
                std::asin(std::clamp(glm::dot(moon, EclipticPole(clock, observer)), -1.0f, 1.0f))));
            minimum = std::min(minimum, beta);
            maximum = std::max(maximum, beta);
        }
    };

    float minimum = 0.0f;
    float maximum = 0.0f;
    latitudesAtNewMoon(kSynodicMonthDays, kNodeCycleYears, minimum, maximum);
    CHECK(minimum < 0.5f);
    CHECK(maximum > 4.0f);

    // A node that does not regress, with a month that divides the year: every new
    // moon falls at the same point of the same orbit, so the alignment that
    // happens is the only one that ever will.
    float fixedMin = 0.0f;
    float fixedMax = 0.0f;
    latitudesAtNewMoon(kYear, 1e9f, fixedMin, fixedMax);
    CHECK(fixedMax - fixedMin < 0.01f);
}

TEST_CASE("a first-quarter moon is in the afternoon sky with the sun still up")
{
    constexpr float kYear = 12.0f;
    const Observer observer = ObserverAt(45.0f);
    const SkyClock clock = ClockAt(15.0, 0.0, kYear);

    const glm::vec3 sun = SunDirection(clock, observer);
    const glm::vec3 moon = MoonDirection(clock, observer, OrbitOf(kMoonInclinationDegrees, kArbitraryNodeDegrees,0.25f));

    CHECK(sun.y > 0.0f);
    CHECK(moon.y > 0.0f);
    CHECK(MoonLitFraction(sun, moon) == doctest::Approx(0.5f).epsilon(0.01));
}

TEST_CASE("direct light is exactly zero at the horizon and saturates above the band")
{
    CHECK(HorizonRamp(0.0f) == 0.0f);
    CHECK(HorizonRamp(-0.0f) == 0.0f);
    CHECK(NightGate(0.0f) == 0.0f);
    CHECK(NightGate(-0.0f) == 0.0f);
    CHECK(HorizonRamp(-0.5f) == 0.0f);
    CHECK(NightGate(0.5f) == 0.0f);
    CHECK(HorizonRamp(1.0f) == 1.0f);
    CHECK(NightGate(-1.0f) == 1.0f);

    // Monotone, so nothing about the handoff depends on where in the band it is
    // sampled.
    float previous = 0.0f;
    for (int32_t step = 0; step <= 64; ++step)
    {
        const float y = kHorizonBandSine * static_cast<float>(step) / 64.0f;
        const float ramp = HorizonRamp(y);
        CHECK(ramp >= previous);
        previous = ramp;
    }
    CHECK(previous == doctest::Approx(1.0f));
}

TEST_CASE("the moon's image up is defined everywhere the moon can go")
{
    constexpr float kYear = 12.0f;
    constexpr int32_t kStepMinutes = 2;

    for (const float latitude : {0.0f, 45.0f, 67.0f, 90.0f, -45.0f})
    {
        // Earth's numbers, and both of the clamps: a planet lying almost in its
        // own orbital plane, and an orbit carrying the moon to within a degree of
        // the ecliptic pole, which is the direction the image's up comes from.
        for (const float tilt : {kEarthTiltDegrees, kMaxAxialTiltDegrees - 1.0f})
        {
            for (const float inclination : {kMoonInclinationDegrees, kMaxMoonInclinationDegrees})
            {
                CAPTURE(latitude);
                CAPTURE(tilt);
                CAPTURE(inclination);
                const Observer observer = ObserverAt(latitude, tilt);
                // Continuity is asserted only where a rate can be argued for. On
                // a world tilted like Earth's with a moon inclined like Earth's,
                // the moon stays eighty-five degrees clear of the ecliptic pole
                // and the projection turns no faster than the sky does. Push
                // either to its clamp and the frame itself races — the sun's
                // right ascension swings fifty-fold near the solstices of an
                // almost-toppled planet — so a bound there would be a number
                // about the configuration rather than about the function. What
                // the extremes still have to deliver is a defined answer, which
                // is what the two assertions inside the loop are.
                const bool tightBound = tilt < 45.0f && inclination < 45.0f;

                glm::vec3 previous{0.0f};
                bool havePrevious = false;
                // A full synodic month, so every phase, every hour and both
                // crossings of the ecliptic are covered.
                for (int32_t minute = 0; minute < 30 * 24 * 60; minute += kStepMinutes)
                {
                    const double days = static_cast<double>(minute) / (24.0 * 60.0);
                    const SkyClock clock = ClockAt(days * 24.0, days, kYear);
                    const glm::vec3 moon =
                        MoonDirection(clock, observer,
                                      OrbitOf(inclination, kArbitraryNodeDegrees,
                                              static_cast<float>(days / static_cast<double>(kSynodicMonthDays))));
                    const glm::vec3 up = MoonImageUp(moon, EclipticPole(clock, observer));

                    REQUIRE(LengthOf(up) == doctest::Approx(1.0f).epsilon(1e-4));
                    REQUIRE(std::abs(glm::dot(up, moon)) < 1e-4f);
                    if (havePrevious && tightBound)
                    {
                        REQUIRE(glm::degrees(Assisi::Math::AngleBetween(previous, up)) < 1.0f);
                    }
                    previous = up;
                    havePrevious = true;
                }
            }
        }
    }
}

TEST_CASE("a moon crossing the zenith keeps its image up, where a horizon-locked one flips")
{
    // The directions are constructed rather than orbited, because the point is
    // what the function does at the zenith and not whether some orbit reaches it.
    // The moon sweeps through straight up; the ecliptic pole sits well away.
    const glm::vec3 pole = glm::normalize(glm::vec3(0.0f, std::sin(glm::radians(40.0f)),
                                                    -std::cos(glm::radians(40.0f))));

    glm::vec3 previousImage{0.0f};
    glm::vec3 previousHorizon{0.0f};
    bool havePrevious = false;
    float worstImageTurn = 0.0f;
    float worstHorizonTurn = 0.0f;

    for (int32_t step = -20; step <= 20; ++step)
    {
        // Offset by half a step so the sweep straddles the zenith rather than
        // landing on it. Landing on it is the easy case: the zenith-locked
        // tangent is the zero vector there and any fallback reads as two
        // ninety-degree turns, which understates what the viewer sees.
        const float theta = glm::radians(0.05f * (static_cast<float>(step) + 0.5f));
        const glm::vec3 moon(std::sin(theta), std::cos(theta), 0.0f);
        const glm::vec3 image = MoonImageUp(moon, pole);
        const glm::vec3 horizon = HorizonLockedUp(moon);

        CHECK(LengthOf(image) == doctest::Approx(1.0f).epsilon(1e-4));
        CHECK(std::abs(glm::dot(image, moon)) < 1e-4f);

        if (havePrevious)
        {
            worstImageTurn = std::max(worstImageTurn, glm::degrees(Assisi::Math::AngleBetween(previousImage, image)));
            worstHorizonTurn =
                std::max(worstHorizonTurn, glm::degrees(Assisi::Math::AngleBetween(previousHorizon, horizon)));
        }
        previousImage = image;
        previousHorizon = horizon;
        havePrevious = true;
    }

    CHECK(worstImageTurn < 1.0f);
    // The zenith-locked alternative turns the picture over between one sample and
    // the next. This assertion is the reason the ecliptic pole is the basis.
    CHECK(worstHorizonTurn > 170.0f);
}

TEST_CASE("the moon's image turns against the horizon as it crosses the sky")
{
    constexpr float kYear = 100000.0f;
    const Observer observer = ObserverAt(45.0f);

    float first = 0.0f;
    float last = 0.0f;
    bool haveFirst = false;
    // A full moon crosses the meridian at midnight, so the sweep runs from noon to
    // noon rather than from midnight: the arc has to be one unbroken stretch of
    // samples for its two ends to be moonrise and moonset. Starting at midnight
    // splits it around the wrap and compares two points half a minute apart.
    for (int32_t minute = 0; minute < kMinutesPerDay; ++minute)
    {
        const double hour = 12.0 + 24.0 * static_cast<double>(minute) / static_cast<double>(kMinutesPerDay);
        const SkyClock clock = ClockAt(hour, 0.0, kYear);
        const glm::vec3 moon = MoonDirection(clock, observer, OrbitOf(kMoonInclinationDegrees, kArbitraryNodeDegrees,0.5f));
        if (moon.y <= kWellUp)
        {
            continue;
        }
        const glm::vec3 image = MoonImageUp(moon, EclipticPole(clock, observer));
        const glm::vec3 vertical = HorizonLockedUp(moon);
        // Signed about the line of sight. Unsigned would read as almost no change
        // at all: the parallactic angle is antisymmetric about the transit, so
        // moonrise and moonset carry the same magnitude and opposite senses, and
        // taking magnitudes would hide exactly the rotation being asserted.
        const float sense = glm::dot(glm::cross(vertical, image), moon) < 0.0f ? -1.0f : 1.0f;
        const float tilt = sense * glm::degrees(Assisi::Math::AngleBetween(image, vertical));
        if (!haveFirst)
        {
            first = tilt;
            haveFirst = true;
        }
        last = tilt;
    }

    REQUIRE(haveFirst);
    // Parallactic rotation: the real moon shows it, and a screen-locked image
    // would hold this angle fixed all night.
    CHECK(std::abs(last - first) > 20.0f);
}
