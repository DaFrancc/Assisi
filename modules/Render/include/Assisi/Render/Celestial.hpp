/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Celestial.hpp
/// @brief Where the sun and the moon are, from a clock and a place on a planet.
///
/// Pure geometry: two angles for the clock, three for the observer, three for the
/// moon's orbit, and unit vectors out. Nothing here knows about components,
/// scenes or frames, which is what lets a day at the north pole be asserted
/// rather than looked at.
///
/// **The seasons are not a feature here, they are the geometry.** The sun rides a
/// circle about the celestial pole at whatever declination the day of the year
/// gives it, so the day length, the noon elevation, the point on the horizon it
/// rises at, polar night and the midnight sun all fall out of one formula. An
/// axial tilt of zero makes the declination identically zero, which is a
/// permanent equinox — today's behaviour recovered as a limit rather than as a
/// branch.
///
/// Up is +Y, east is +X, and so north is −Z and south is +Z, since east crossed
/// into up must give north for a right-handed frame. The observer's bearing turns
/// every result about +Y at the end, which is what moves sunrise off due east
/// without turning the level.

#include <algorithm>
#include <cmath>

#include <Assisi/Math/GLM.hpp>

namespace Assisi::Render
{

/// @brief Half-width of the band a body's direct light fades over as it crosses
/// the horizon, as the vertical component of the direction to it. sin 3°.
///
/// The sun lights the world above this band and the moon below it, and both are
/// **exactly zero** at the geometric horizon — which is what makes the handoff
/// between them happen at black rather than at whichever intensity the two
/// happen to be equal at. Widening it buys a gentler swap at the cost of the
/// golden hour, since direct light is only a quarter of noon two degrees up.
inline constexpr float kHorizonBandSine = 0.05234f;

/// @name The ranges an observer and an orbit are held inside
/// @{

/// The pole is a place. There is no divergence to avoid: the direction formula
/// is a circle about the celestial pole and contains no tangent, so latitude 90
/// gives a sun circling at constant elevation, which is what the pole sees.
inline constexpr float kMinLatitudeDegrees = -90.0f;
inline constexpr float kMaxLatitudeDegrees = 90.0f;

/// Zero is the no-seasons limit and is meant to be reachable; 90 lays the axis in
/// the orbital plane, putting the sun overhead at the pole on the solstices.
inline constexpr float kMinAxialTiltDegrees = 0.0f;
inline constexpr float kMaxAxialTiltDegrees = 90.0f;

/// 89, not 90: the moon's disk image is oriented to the ecliptic pole, and an
/// orbit inclined a full right angle would carry the moon through that pole,
/// where "up" has no answer. One degree short keeps @ref MoonImageUp total.
inline constexpr float kMinMoonInclinationDegrees = 0.0f;
inline constexpr float kMaxMoonInclinationDegrees = 89.0f;

/// A phase cycle shorter than a day is a strobe, not a moon.
inline constexpr float kMinMoonCycleDays = 1.0f;

/// The node's period is quoted in years because the node is a longitude in the
/// ecliptic, and the ecliptic's own period is the year.
inline constexpr float kMinNodeCycleYears = 0.01f;
/// @}

/// @brief The shortest a projected-out component may be and still be worth
/// normalising, in units of the unit vectors it came from.
///
/// Every "take this direction, remove the part along that one, and point what is
/// left" in the sky is the same operation, and each has one case where what is
/// left is nothing: the two directions were parallel. Below this the remainder is
/// rounding rather than direction, and the caller substitutes something arbitrary
/// and says so.
inline constexpr float kMinTangentLength = 1e-6f;

/// @brief Width of the moon's terminator, in units of the cosine between the
/// surface normal and the direction to the sun.
///
/// The moon is not shaded by a cosine — regolith is retro-reflective and its lit
/// face is near uniform, so a Lambert term reads as a billiard ball. The
/// terminator is still where the sphere turns away from the sun; this is only how
/// abruptly it gets there.
inline constexpr float kMoonTerminatorSoftness = 0.05f;

/// @brief Where on the planet the sky is being watched from, and what planet it is.
struct Observer
{
    /// North positive. The equator sees the celestial equator overhead; a pole
    /// sees the celestial pole overhead and a sun that never rises or sets.
    float latitudeRadians = 0.0f;
    /// The angle between the planet's spin axis and its orbital plane. Zero is a
    /// world with no seasons.
    float axialTiltRadians = 0.0f;
    /// A turn about world up applied to every direction, so the level's east need
    /// not be the world's +X.
    float bearingRadians = 0.0f;
};

/// @brief The two angles a date and a time come to.
struct SkyClock
{
    /// Zero at apparent solar noon, growing eastward: the sun crosses the
    /// meridian at 12:00 every day and there is no equation of time.
    float hourAngle = 0.0f;
    /// The sun's longitude along the ecliptic, zero at the March equinox.
    float solarLongitude = 0.0f;
};

/// @brief The moon's orbit, as of some instant.
struct MoonOrbit
{
    /// Tilt of the orbital plane against the **ecliptic**, not the equator.
    float inclinationRadians = 0.0f;
    /// Ecliptic longitude of the ascending node, measured from the March equinox.
    /// A real node regresses, so this is an instant's value rather than a
    /// constant of the orbit.
    float nodeRadians = 0.0f;
    /// How far east of the sun the moon is, along the ecliptic. Zero is new,
    /// half a turn is full — the phase, expressed as the geometry that causes it.
    float elongationRadians = 0.0f;
};

namespace Detail
{

/// @brief The observer's own frame: east, the meridian's crossing of the
/// celestial equator, and the celestial pole.
///
/// Orthonormal by construction, which is what makes every direction below a unit
/// vector as an identity rather than after a normalise.
struct ObserverFrame
{
    glm::vec3 east{1.0f, 0.0f, 0.0f};
    /// Due south at elevation 90 − latitude: where the celestial equator crosses
    /// the meridian, and where the sun sits at noon on an equinox.
    glm::vec3 meridian{0.0f, 1.0f, 0.0f};
    /// Due north at elevation equal to the latitude. Everything in the sky turns
    /// about this once a day.
    glm::vec3 pole{0.0f, 0.0f, -1.0f};
};

[[nodiscard]] inline ObserverFrame FrameOf(const Observer &observer)
{
    const float sinPhi = std::sin(observer.latitudeRadians);
    const float cosPhi = std::cos(observer.latitudeRadians);
    return ObserverFrame{.east = glm::vec3(1.0f, 0.0f, 0.0f),
                         .meridian = glm::vec3(0.0f, cosPhi, sinPhi),
                         .pole = glm::vec3(0.0f, sinPhi, -cosPhi)};
}

/// @brief @p direction turned about world up by @p bearing.
[[nodiscard]] inline glm::vec3 TurnedByBearing(const glm::vec3 &direction, float bearing)
{
    const float s = std::sin(bearing);
    const float c = std::cos(bearing);
    return glm::vec3(c * direction.x + s * direction.z, direction.y, c * direction.z - s * direction.x);
}

/// @brief The ecliptic's frame as the observer sees it right now: the March
/// equinox point, the point a quarter turn east of it along the ecliptic, and the
/// ecliptic pole.
///
/// The sun needs none of this — the clock is apparent solar time, so the sun's
/// hour angle is the clock. The moon does, because it is placed against the sun
/// in ecliptic longitude while the observer sees it in hour angle, and the two
/// differ by the sun's right ascension.
struct EclipticFrame
{
    glm::vec3 equinox{1.0f, 0.0f, 0.0f};
    glm::vec3 quadrature{0.0f, 1.0f, 0.0f};
    glm::vec3 pole{0.0f, 0.0f, 1.0f};
};

[[nodiscard]] inline EclipticFrame FrameFor(const SkyClock &clock, const Observer &observer)
{
    const ObserverFrame observerFrame = FrameOf(observer);
    const float sinEps = std::sin(observer.axialTiltRadians);
    const float cosEps = std::cos(observer.axialTiltRadians);

    // The sun's right ascension: where along the celestial equator its ecliptic
    // longitude projects. One arc tangent per frame, and the only place the two
    // coordinate systems have to be reconciled.
    const float rightAscension =
        std::atan2(std::sin(clock.solarLongitude) * cosEps, std::cos(clock.solarLongitude));
    const float siderealTime = clock.hourAngle + rightAscension;
    const float sinLst = std::sin(siderealTime);
    const float cosLst = std::cos(siderealTime);

    const glm::vec3 equinox = cosLst * observerFrame.meridian - sinLst * observerFrame.east;
    const glm::vec3 rightAngleEast = sinLst * observerFrame.meridian + cosLst * observerFrame.east;

    return EclipticFrame{.equinox = equinox,
                         .quadrature = cosEps * rightAngleEast + sinEps * observerFrame.pole,
                         .pole = cosEps * observerFrame.pole - sinEps * rightAngleEast};
}

/// @brief Some unit vector at a right angle to @p direction, for the cases where
/// which one does not matter because the caller is about to be told it is
/// arbitrary.
[[nodiscard]] inline glm::vec3 AnyPerpendicular(const glm::vec3 &direction)
{
    // Cross with whichever axis the direction leans on least. A unit vector
    // cannot lean on all three at once — its smallest component is at most
    // 1/sqrt(3) — so the chosen axis is always far enough from parallel for the
    // cross product to keep its precision.
    const float x = std::abs(direction.x);
    const float y = std::abs(direction.y);
    const float z = std::abs(direction.z);
    glm::vec3 axis(1.0f, 0.0f, 0.0f);
    if (y <= x && y <= z)
    {
        axis = glm::vec3(0.0f, 1.0f, 0.0f);
    }
    else if (z <= x && z <= y)
    {
        axis = glm::vec3(0.0f, 0.0f, 1.0f);
    }

    const glm::vec3 crossed = glm::cross(direction, axis);
    const float length = std::sqrt(glm::dot(crossed, crossed));
    // Only a zero-length direction reaches the fallback, and it has no
    // perpendicular to find.
    return length > 0.0f ? crossed / length : glm::vec3(0.0f, 0.0f, -1.0f);
}

} // namespace Detail

/// @brief The hour angle of @p hour, in radians: zero at noon, growing eastward,
/// −π at midnight.
[[nodiscard]] inline float HourAngle(double hour)
{
    return static_cast<float>(glm::two_pi<double>() * (hour / 24.0) - glm::pi<double>());
}

/// @brief The sun's ecliptic longitude on @p dayOfYear of a year @p yearLengthDays
/// long. Zero at the March equinox, a quarter turn at the June solstice.
[[nodiscard]] inline float SolarLongitude(double dayOfYear, float yearLengthDays)
{
    // A year of no length has no seasons to be partway through, and the honest
    // answer is the equinox the year is measured from — the same permanent
    // equinox a world with no axial tilt has every day of its year.
    if (!(yearLengthDays > 0.0f) || !std::isfinite(yearLengthDays))
    {
        return 0.0f;
    }
    return static_cast<float>(glm::two_pi<double>() * (dayOfYear / static_cast<double>(yearLengthDays)));
}

/// @brief The sine of the sun's declination — how far north or south of the
/// celestial equator it is today.
///
/// At @p axialTiltRadians of zero this is exactly zero for every longitude, which
/// is the no-seasons limit: every day an equinox, no branch anywhere.
[[nodiscard]] inline float SinDeclination(float axialTiltRadians, float solarLongitude)
{
    return std::sin(axialTiltRadians) * std::sin(solarLongitude);
}

/// @brief The unit direction **to** the sun. The light travels the other way.
///
/// A circle about the celestial pole at constant declination, which is what a day
/// is. Written that way rather than as elevation and azimuth because it costs the
/// same trigonometry and produces the vector directly, and because unit length is
/// then an identity of the orthonormal frame rather than something a normalise
/// has to restore.
[[nodiscard]] inline glm::vec3 SunDirection(const SkyClock &clock, const Observer &observer)
{
    const Detail::ObserverFrame frame = Detail::FrameOf(observer);
    const float sinDec = SinDeclination(observer.axialTiltRadians, clock.solarLongitude);
    // The declination never leaves [−tilt, tilt], so the positive root is the
    // right one for any tilt up to a right angle and no arc sine is taken.
    const float cosDec = std::sqrt(std::max(1.0f - sinDec * sinDec, 0.0f));
    const glm::vec3 direction = cosDec * (std::cos(clock.hourAngle) * frame.meridian -
                                          std::sin(clock.hourAngle) * frame.east) +
                                sinDec * frame.pole;
    return Detail::TurnedByBearing(direction, observer.bearingRadians);
}

/// @brief How many hours of the day the sun spends above the horizon at
/// @p latitudeRadians on a day of declination @p sinDeclination.
///
/// A readout and a test oracle. The renderer never consults it — whether the sun
/// is up is `SunDirection(...).y > 0` and nothing else, so this cannot disagree
/// with what is drawn by being wrong; it can only be a wrong number on a panel.
///
/// Over one day the sun's elevation traces `swing * cos(H) + midpoint`, and it
/// crosses the horizon where that is zero. A swing narrower than its own midpoint
/// never crosses at all — which is polar night when the midpoint is below and the
/// midnight sun when it is above, both stated as the inequality that causes them
/// rather than as latitudes to compare against. Asking the question that way also
/// removes the division from every case where it would have been by zero, so
/// there is no guard on it and none needed: at the pole the swing is exactly zero
/// and the inequality has already answered.
[[nodiscard]] inline float DaylightHours(float latitudeRadians, float sinDeclination)
{
    const float cosDec = std::sqrt(std::max(1.0f - sinDeclination * sinDeclination, 0.0f));
    const float swing = std::cos(latitudeRadians) * cosDec;
    const float midpoint = std::sin(latitudeRadians) * sinDeclination;

    if (!(swing > std::abs(midpoint)))
    {
        return midpoint > 0.0f ? 24.0f : 0.0f;
    }
    return (24.0f / glm::pi<float>()) * std::acos(-midpoint / swing);
}

/// @brief The ecliptic pole, in the observer's frame, right now.
///
/// Fixed to the celestial sphere, so it turns once a day about the celestial pole
/// and its elevation changes through the night. That motion is why the moon's
/// image turns against the horizon, which is what the real moon does.
[[nodiscard]] inline glm::vec3 EclipticPole(const SkyClock &clock, const Observer &observer)
{
    return Detail::TurnedByBearing(Detail::FrameFor(clock, observer).pole, observer.bearingRadians);
}

/// @brief The unit direction **to** the moon.
///
/// Three frames deep — observer from equatorial, equatorial from ecliptic,
/// ecliptic from the orbit — because the moon is placed relative to the sun along
/// the ecliptic while it is seen relative to the horizon. Every stage is a pair of
/// orthonormal vectors, so the result is unit length by identity.
///
/// With a zero inclination the moon lies on the ecliptic at longitude
/// `solarLongitude + elongation`, so a zero elongation puts it exactly where the
/// sun is and a half turn puts it exactly opposite — which is what a new moon and
/// a full moon are.
[[nodiscard]] inline glm::vec3 MoonDirection(const SkyClock &clock, const Observer &observer,
                                             const MoonOrbit &orbit)
{
    const Detail::EclipticFrame frame = Detail::FrameFor(clock, observer);

    const float sinNode = std::sin(orbit.nodeRadians);
    const float cosNode = std::cos(orbit.nodeRadians);
    // The node itself, and the point a quarter turn along the ecliptic from it.
    const glm::vec3 node = cosNode * frame.equinox + sinNode * frame.quadrature;
    const glm::vec3 ahead = cosNode * frame.quadrature - sinNode * frame.equinox;

    // How far round the orbit from the node the moon is. The inclination tilts
    // the "ahead" leg out of the ecliptic and leaves the node fixed, which is
    // what an inclination is.
    const float argument = (clock.solarLongitude + orbit.elongationRadians) - orbit.nodeRadians;
    const glm::vec3 tilted =
        std::cos(orbit.inclinationRadians) * ahead + std::sin(orbit.inclinationRadians) * frame.pole;
    const glm::vec3 direction = std::cos(argument) * node + std::sin(argument) * tilted;
    return Detail::TurnedByBearing(direction, observer.bearingRadians);
}

/// @brief Which way the top of the moon's disk image points, tangent to the sky
/// at the moon. Unit, and perpendicular to @p toMoon.
///
/// The ecliptic pole, projected onto the sky there. Not the zenith and not the
/// screen: those degenerate exactly where the moon transits overhead, which it
/// does every month anywhere near the equator, and the image would spin through
/// half a turn as it passed. The ecliptic pole degenerates only at the ecliptic
/// pole, and the inclination clamp is what keeps the moon away from it — so the
/// projection is never shorter than the cosine of that clamp.
///
/// It is also the right up for the picture. The moon's own axis is a degree and a
/// half from the ecliptic pole, and the sun lies in the ecliptic, so the horns of
/// a crescent point along this and the maria sit where they should against the
/// terminator.
[[nodiscard]] inline glm::vec3 MoonImageUp(const glm::vec3 &toMoon, const glm::vec3 &eclipticPole)
{
    const glm::vec3 tangent = eclipticPole - toMoon * glm::dot(toMoon, eclipticPole);
    const float length = std::sqrt(glm::dot(tangent, tangent));
    return length > kMinTangentLength ? tangent / length : Detail::AnyPerpendicular(toMoon);
}

/// @brief How much of the moon's visible face is lit, in [0, 1]. Zero is new, one
/// is full.
///
/// The elongation and nothing else: the fraction of the disk in sunlight is set by
/// the angle between the direction to the sun and the direction to the moon, which
/// is why a phase needs no state and cannot drift out of step with the sky.
[[nodiscard]] inline float MoonLitFraction(const glm::vec3 &toSun, const glm::vec3 &toMoon)
{
    return 0.5f * (1.0f - std::clamp(glm::dot(toSun, toMoon), -1.0f, 1.0f));
}

/// @brief How much of a body's direct light survives its height above the
/// horizon: zero at and below it, one once it is clear of the band.
///
/// Exactly zero at the horizon rather than merely small, which is the whole point.
/// The sun and the moon hand the world's lighting between them where both of
/// these are zero, so nothing about the swap depends on the two being comparably
/// bright — and they are not, at any hour, in either tinting mode.
[[nodiscard]] inline float HorizonRamp(float y)
{
    return glm::smoothstep(0.0f, kHorizonBandSine, y);
}

/// @brief How far into the night it is, by the sun's depth below the horizon: zero
/// while the sun is up, one once it is a few degrees down.
[[nodiscard]] inline float NightGate(float sunY)
{
    return HorizonRamp(-sunY);
}

} // namespace Assisi::Render
