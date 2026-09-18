/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestAngles.cpp
/// @brief What AngleBetween has to answer where the obvious formula stops
/// answering anything.

#include <doctest/doctest.h>

#include <Assisi/Math/Angles.hpp>

#include <cmath>

using Assisi::Math::AngleBetween;

TEST_CASE("AngleBetween: the ordinary angles")
{
    CHECK(AngleBetween(glm::vec3(1.f, 0.f, 0.f), glm::vec3(1.f, 0.f, 0.f)) == doctest::Approx(0.f));
    CHECK(AngleBetween(glm::vec3(1.f, 0.f, 0.f), glm::vec3(0.f, 1.f, 0.f)) ==
          doctest::Approx(glm::half_pi<float>()));
    CHECK(AngleBetween(glm::vec3(0.f, 3.f, 0.f), glm::vec3(0.f, 0.f, 7.f)) ==
          doctest::Approx(glm::half_pi<float>()));
}

TEST_CASE("AngleBetween: a degenerate direction measures no angle rather than a NaN")
{
    CHECK(AngleBetween(glm::vec3(0.f), glm::vec3(1.f, 0.f, 0.f)) == 0.f);
    CHECK(AngleBetween(glm::vec3(1.f, 0.f, 0.f), glm::vec3(0.f)) == 0.f);
    CHECK(!std::isnan(AngleBetween(glm::vec3(0.f), glm::vec3(0.f))));
}

TEST_CASE("AngleBetween: the small angles survive, where acos(dot) does not")
{
    // A 24-hour day cycle turns the sun about 1.2e-6 radians per frame at 60 Hz.
    // This is the figure the whole function exists for: measured through a dot
    // product, its cosine has already rounded to exactly 1 and the angle comes
    // back as exactly zero, so a shadow cadence built on it would never notice
    // the sun moving at all.
    // Checked against the angle's own size rather than through Approx, which
    // calls everything near zero equal to everything else near zero — and that
    // is the exact confusion these numbers have to be kept out of.
    for (const float angle : {1e-3f, 1e-5f, 1.2e-6f, 1e-7f})
    {
        const glm::vec3 from(0.f, -1.f, 0.f);
        const glm::vec3 to(0.f, -std::cos(angle), std::sin(angle));

        CHECK(std::abs(AngleBetween(from, to) - angle) <= angle * 0.01f);
    }

    // The formula this replaces, on the same input. At a thousandth of a radian
    // it is already 2% low, and below about a hundred-thousandth it returns
    // exactly zero — so a cadence built on it would report a sun that never
    // moves, at every day length anyone would want.
    const glm::vec3 down(0.f, -1.f, 0.f);
    const glm::vec3 turned(0.f, -std::cos(1e-6f), std::sin(1e-6f));
    CHECK(std::acos(std::min(glm::dot(down, turned), 1.f)) == 0.f);
    CHECK(AngleBetween(down, turned) > 0.f);
}

TEST_CASE("AngleBetween: a small angle is measured the same from either side")
{
    const glm::vec3 from(0.f, -1.f, 0.f);
    const glm::vec3 to(0.f, -std::cos(1e-5f), std::sin(1e-5f));
    CHECK(AngleBetween(from, to) == doctest::Approx(AngleBetween(to, from)));
}
