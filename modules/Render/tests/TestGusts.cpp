/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Render/Gusts.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace Assisi::Render;

namespace
{

/// A light, gusty breeze toward the north-east.
Gusts Breeze()
{
    return Gusts{.toward = glm::normalize(glm::vec2(1.0f, -1.0f)), .speed = 4.0f, .gustFactor = 1.5f};
}

/// Points spread over a few kilometres, the same every run.
std::vector<glm::dvec2> Spread(uint32_t count)
{
    constexpr double kGolden = 0.6180339887498949;
    constexpr double kAcross = 3000.0;
    std::vector<glm::dvec2> points;
    points.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        const double x = std::fmod(static_cast<double>(i) * kGolden, 1.0);
        const double z = (static_cast<double>(i) + 0.5) / static_cast<double>(count);
        points.emplace_back(x * kAcross, z * kAcross);
    }
    return points;
}

} // namespace

TEST_CASE("The gusts around the camera are the gusts in the world, hours into play and far from the origin")
{
    const Gusts gusts = Breeze();
    for (const double seconds : {0.0, 37.25, 7200.0, 1.0e6})
    {
        CAPTURE(seconds);
        const glm::dvec3 camera(250000.5, 12.0, -480000.25);
        const GustConstants constants = MakeGustConstants(gusts, camera, seconds);
        for (const glm::vec2 offset : {glm::vec2(0.0f), glm::vec2(13.5f, -40.0f), glm::vec2(-900.0f, 600.0f)})
        {
            CAPTURE(offset.x);
            const float world = GustSpeed(gusts, glm::dvec2(camera.x, camera.z) + glm::dvec2(offset), seconds);
            CHECK(GustSpeedNear(constants, offset) == doctest::Approx(world).epsilon(1e-3));
        }
    }
}

TEST_CASE("Gusts reach the gust factor and lulls the same below, around the steady speed")
{
    const Gusts gusts = Breeze();
    float lowest = gusts.speed;
    float highest = gusts.speed;
    double sum = 0.0;
    const std::vector<glm::dvec2> points = Spread(20000u);
    for (const glm::dvec2 point : points)
    {
        const float speed = GustSpeed(gusts, point, 100.0);
        lowest = std::min(lowest, speed);
        highest = std::max(highest, speed);
        sum += static_cast<double>(speed);
    }
    const float swing = gusts.speed * (gusts.gustFactor - 1.0f);
    CHECK(highest <= gusts.speed + swing * 1.0001f);
    CHECK(lowest >= gusts.speed - swing * 1.0001f);
    // Among this many points some gusts come near the strongest, and some lulls
    // near the deepest.
    CHECK(highest > gusts.speed + 0.9f * swing);
    CHECK(lowest < gusts.speed - 0.9f * swing);
    CHECK(sum / static_cast<double>(points.size()) == doctest::Approx(gusts.speed).epsilon(0.03));
}

TEST_CASE("A steady wind has no gusts, and still air none at all")
{
    Gusts steady = Breeze();
    steady.gustFactor = 1.0f;
    Gusts still = Breeze();
    still.speed = 0.0f;
    for (const glm::dvec2 point : Spread(200u))
    {
        CHECK(GustSpeed(steady, point, 50.0) == doctest::Approx(steady.speed));
        CHECK(GustSpeed(still, point, 50.0) == 0.0f);
    }
}

TEST_CASE("A wind gustier than twice its speed lulls to still air, never to a wind the other way")
{
    Gusts wild = Breeze();
    wild.gustFactor = 3.0f;
    for (const glm::dvec2 point : Spread(2000u))
    {
        CHECK(GustSpeed(wild, point, 10.0) >= 0.0f);
    }
}

TEST_CASE("The gusts drift downwind with the wind")
{
    // Carried downwind, a gust has only aged; held still, the wind has also
    // carried it away.
    const Gusts gusts = Breeze();
    constexpr double kStep = 1.5;
    const glm::dvec2 carried = glm::dvec2(gusts.toward) * (static_cast<double>(gusts.speed) * kStep);
    double followed = 0.0;
    double held = 0.0;
    for (const glm::dvec2 point : Spread(4000u))
    {
        const float now = GustSpeed(gusts, point, 200.0);
        followed += std::abs(static_cast<double>(GustSpeed(gusts, point + carried, 200.0 + kStep) - now));
        held += std::abs(static_cast<double>(GustSpeed(gusts, point, 200.0 + kStep) - now));
    }
    CHECK(followed < 0.8 * held);
}

TEST_CASE("The pattern repeats only after its period, kilometres downwind")
{
    const Gusts gusts = Breeze();
    const glm::dvec2 point(812.5, -37.0);
    const glm::dvec2 period =
        glm::dvec2(gusts.toward) * (static_cast<double>(kGustPeriod) * static_cast<double>(kGustLengthAlong));
    CHECK(GustSpeed(gusts, point + period, 60.0) == doctest::Approx(GustSpeed(gusts, point, 60.0)).epsilon(1e-3));
    CHECK(GustNoise(glm::vec3(3.25f, 7.5f, 1.125f)) ==
          doctest::Approx(GustNoise(glm::vec3(3.25f + static_cast<float>(kGustPeriod), 7.5f, 1.125f))));
}
