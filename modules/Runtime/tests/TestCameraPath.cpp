/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCameraPath.cpp
/// @brief The scripted route a benchmark flies: which path plays when, and
/// where each one puts the camera.

#include <doctest/doctest.h>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Math/Matrix.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/CameraPath.hpp>

#include <cmath>
#include <vector>

using namespace Assisi;
using namespace Assisi::Runtime;

namespace
{
/// Positions come out of trig and a matrix inverse, so they are compared to
/// within this rather than exactly.
constexpr float kTolerance = 1e-4f;

bool Near(const glm::vec3 &a, const glm::vec3 &b)
{
    return glm::length(a - b) < kTolerance;
}

bool Near(const glm::dvec3 &a, const glm::dvec3 &b)
{
    return glm::length(a - b) < static_cast<double>(kTolerance);
}

CameraLinePath Line(const glm::dvec3 &from, const glm::dvec3 &to, float seconds, int32_t order)
{
    CameraLinePath line;
    line.from = from;
    line.to = to;
    line.seconds = seconds;
    line.order = order;
    return line;
}

CameraRouteLeg LineLeg(const CameraLinePath &line)
{
    CameraRouteLeg leg;
    leg.line = line;
    return leg;
}
} // namespace

TEST_CASE("A line path moves the camera from one end to the other")
{
    const CameraRouteLeg leg = LineLeg(Line({0.f, 0.f, 0.f}, {10.f, 0.f, 0.f}, 2.f, 0));

    CHECK(Near(EvaluateCameraLeg(leg, 0.f).eye, {0.f, 0.f, 0.f}));
    CHECK(Near(EvaluateCameraLeg(leg, 0.5f).eye, {5.f, 0.f, 0.f}));
    CHECK(Near(EvaluateCameraLeg(leg, 1.f).eye, {10.f, 0.f, 0.f}));

    // Clamped, so a frame that lands past the end holds the last pose.
    CHECK(Near(EvaluateCameraLeg(leg, 2.f).eye, {10.f, 0.f, 0.f}));
}

TEST_CASE("A line path looks along its travel, or at its target")
{
    CameraRouteLeg leg = LineLeg(Line({0.f, 0.f, 0.f}, {0.f, 0.f, -10.f}, 1.f, 0));

    const CameraAim along = EvaluateCameraLeg(leg, 0.5f);
    CHECK(glm::normalize(along.target - along.eye).z == doctest::Approx(-1.0));

    leg.line.lookAlongPath = false;
    leg.line.lookAt = {3.f, 4.f, 5.f};
    CHECK(Near(EvaluateCameraLeg(leg, 0.5f).target, {3.f, 4.f, 5.f}));
}

TEST_CASE("A circle path starts at its start angle and sweeps round its centre")
{
    CameraRouteLeg leg;
    leg.isCircle = true;
    leg.circle.center = {1.f, 0.f, 1.f};
    leg.circle.radius = 4.f;
    leg.circle.height = 2.f;
    leg.circle.startDegrees = 0.f;
    leg.circle.sweepDegrees = 90.f;
    leg.circle.lookAt = {1.f, 0.f, 1.f};

    CHECK(Near(EvaluateCameraLeg(leg, 0.f).eye, {5.f, 2.f, 1.f}));
    CHECK(Near(EvaluateCameraLeg(leg, 1.f).eye, {1.f, 2.f, 5.f}));
    CHECK(Near(EvaluateCameraLeg(leg, 0.5f).target, {1.f, 0.f, 1.f}));

    // A negative sweep goes the other way round.
    leg.circle.sweepDegrees = -90.f;
    CHECK(Near(EvaluateCameraLeg(leg, 1.f).eye, {1.f, 2.f, -3.f}));
}

TEST_CASE("A route plays its paths by order, and skips the ones with no length")
{
    ECS::Scene scene;

    const ECS::Entity second = scene.Create();
    (void)scene.Add<CameraLinePath>(second, Line({10.f, 0.f, 0.f}, {20.f, 0.f, 0.f}, 1.f, 5));

    const ECS::Entity first = scene.Create();
    CameraCirclePath circle;
    circle.seconds = 2.f;
    circle.order = 1;
    (void)scene.Add<CameraCirclePath>(first, circle);

    const ECS::Entity parked = scene.Create();
    (void)scene.Add<CameraLinePath>(parked, Line({0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, 0.f, 0));

    const std::vector<CameraRouteLeg> route = BuildCameraRoute(scene);
    REQUIRE(route.size() == 2);
    CHECK(route[0].entity == first);
    CHECK(route[0].isCircle);
    CHECK(route[1].entity == second);
    CHECK(CameraRouteSeconds(route) == doctest::Approx(3.f));
}

TEST_CASE("Paths with the same order play in entity order")
{
    ECS::Scene scene;
    const ECS::Entity a = scene.Create();
    const ECS::Entity b = scene.Create();
    (void)scene.Add<CameraLinePath>(b, Line({}, {1.f, 0.f, 0.f}, 1.f, 0));
    (void)scene.Add<CameraLinePath>(a, Line({}, {1.f, 0.f, 0.f}, 1.f, 0));

    const std::vector<CameraRouteLeg> route = BuildCameraRoute(scene);
    REQUIRE(route.size() == 2);
    CHECK(route[0].entity == a);
    CHECK(route[1].entity == b);
}

TEST_CASE("A route hands over from one path to the next at the boundary")
{
    const std::vector<CameraRouteLeg> route{
        LineLeg(Line({0.f, 0.f, 0.f}, {10.f, 0.f, 0.f}, 2.f, 0)),
        LineLeg(Line({0.f, 5.f, 0.f}, {0.f, 15.f, 0.f}, 4.f, 1)),
    };

    CHECK(Near(EvaluateCameraRoute(route, 1.f).eye, {5.f, 0.f, 0.f}));
    CHECK(Near(EvaluateCameraRoute(route, 2.f).eye, {0.f, 5.f, 0.f}));
    CHECK(Near(EvaluateCameraRoute(route, 4.f).eye, {0.f, 10.f, 0.f}));

    // Either side of the route holds its first and last pose.
    CHECK(Near(EvaluateCameraRoute(route, -1.f).eye, {0.f, 0.f, 0.f}));
    CHECK(Near(EvaluateCameraRoute(route, 100.f).eye, {0.f, 15.f, 0.f}));
}

TEST_CASE("The camera world matrix sits at the eye and faces the target")
{
    const CameraAim aim{.eye = {1.0, 2.0, 3.0}, .target = {1.0, 2.0, -7.0}};
    const ECS::WorldMatrix world = CameraWorldFor(aim);

    CHECK(Near(ECS::WorldPositionOf(world), aim.eye));
    CHECK(Near(ForwardDirection(world.inCell), glm::vec3{0.f, 0.f, -1.f}));
}

TEST_CASE("A camera hundreds of kilometres out sits exactly at its eye")
{
    // A whole number of 2^-9 m steps, so the eye is exact as a float offset in
    // its cell and the position comes back to the bit.
    const CameraAim aim{.eye = {300000.25, 12.5, -456789.75}, .target = {300000.25, 12.5, -456799.75}};
    const ECS::WorldMatrix world = CameraWorldFor(aim);

    CHECK(ECS::WorldPositionOf(world) == aim.eye);
    CHECK(Near(ForwardDirection(world.inCell), glm::vec3{0.f, 0.f, -1.f}));
}

TEST_CASE("Looking straight down still gives a usable camera")
{
    const CameraAim aim{.eye = {0.0, 10.0, 0.0}, .target = {0.0, 0.0, 0.0}};
    const ECS::WorldMatrix world = CameraWorldFor(aim);

    CHECK(Near(ForwardDirection(world.inCell), glm::vec3{0.f, -1.f, 0.f}));
    const glm::mat4 view = ViewMatrix(world.inCell);
    for (int32_t column = 0; column < 4; ++column)
    {
        for (int32_t row = 0; row < 4; ++row)
        {
            CHECK_FALSE(std::isnan(view[column][row]));
        }
    }
}
