/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestWindResolve.cpp
/// @brief A level's wind is its one Wind, finite and in range, or still air
/// when it has none; a second Wind is counted, not used.

#include <doctest/doctest.h>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/WindComponents.hpp>
#include <Assisi/Runtime/WindResolve.hpp>

#include <limits>

using namespace Assisi;
using Assisi::Runtime::Wind;

namespace
{

Wind Resolved(const Wind &wind)
{
    ECS::Scene scene;
    REQUIRE(scene.Add(scene.Create(), wind) != nullptr);
    return Runtime::ResolveWind(scene).wind;
}

} // namespace

TEST_CASE("A level with no Wind has still air")
{
    const ECS::Scene scene;
    const Runtime::WindResolution resolved = Runtime::ResolveWind(scene);
    CHECK(resolved.count == 0u);
    CHECK(resolved.wind.speed == 0.0f);
}

TEST_CASE("A level's Wind is its wind, wherever it is")
{
    ECS::Scene scene;
    (void)scene.Create();
    REQUIRE(scene.Add(scene.Create(), Wind{.speed = 12.0f, .bearingDegrees = 250.0f}) != nullptr);

    const Runtime::WindResolution resolved = Runtime::ResolveWind(scene);
    CHECK(resolved.count == 1u);
    CHECK(resolved.wind.speed == 12.0f);
    CHECK(resolved.wind.bearingDegrees == 250.0f);
}

TEST_CASE("A Wind out of range is brought into it")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(Resolved(Wind{.speed = 99.0f}).speed == Runtime::kMaxWindSpeed);
    CHECK(Resolved(Wind{.speed = -3.0f}).speed == 0.0f);
    CHECK(Resolved(Wind{.speed = nan}).speed == 0.0f);
    CHECK(Resolved(Wind{.bearingDegrees = 450.0f}).bearingDegrees == doctest::Approx(90.0f));
    CHECK(Resolved(Wind{.bearingDegrees = nan}).bearingDegrees == 0.0f);
    CHECK(Resolved(Wind{.gustFactor = 0.5f}).gustFactor == Runtime::kSteadyGustFactor);
    CHECK(Resolved(Wind{.gustFactor = 7.0f}).gustFactor == Runtime::kMaxGustFactor);
    CHECK(Resolved(Wind{.gustFactor = nan}).gustFactor == Runtime::kDefaultGustFactor);
}

TEST_CASE("The gusts ride the wind: its direction across the world, its speed and its gust factor")
{
    const Render::Gusts east = Runtime::GustsOf(Wind{.speed = 6.0f, .bearingDegrees = 90.0f, .gustFactor = 1.7f});
    CHECK(east.toward.x == doctest::Approx(1.0f));
    CHECK(east.toward.y == doctest::Approx(0.0f));
    CHECK(east.speed == 6.0f);
    CHECK(east.gustFactor == 1.7f);
    // North is -z.
    const Render::Gusts north = Runtime::GustsOf(Wind{.bearingDegrees = 0.0f});
    CHECK(north.toward.y == doctest::Approx(-1.0f));
    CHECK(Runtime::GustsOf(Runtime::kStillAir).speed == 0.0f);
}

TEST_CASE("A second Wind is counted, and the first is the wind")
{
    ECS::Scene scene;
    REQUIRE(scene.Add(scene.Create(), Wind{.speed = 5.0f}) != nullptr);
    REQUIRE(scene.Add(scene.Create(), Wind{.speed = 20.0f}) != nullptr);

    const Runtime::WindResolution resolved = Runtime::ResolveWind(scene);
    CHECK(resolved.count == 2u);
    CHECK(resolved.wind.speed == 5.0f);
}
