/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCameraPath.cpp
/// @brief The scripted route a benchmark flies: which path plays when, and
/// where each one puts the camera.

#include <doctest/doctest.h>

#include <Assisi/ECS/Scene.hpp>
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

CameraLinePath Line(const glm::vec3 &from, const glm::vec3 &to, float seconds, int32_t order)
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
    CHECK(glm::normalize(along.target - along.eye).z == doctest::Approx(-1.f));

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

TEST_CASE("The camera transform sits at the eye and faces the target")
{
    const CameraAim aim{.eye = {1.f, 2.f, 3.f}, .target = {1.f, 2.f, -7.f}};
    const Transform transform = CameraTransformFor(aim);

    CHECK(Near(glm::vec3(transform.worldMatrix[3]), aim.eye));
    CHECK(Near(ForwardDirection(transform), {0.f, 0.f, -1.f}));
}

TEST_CASE("Looking straight down still gives a usable camera")
{
    const CameraAim aim{.eye = {0.f, 10.f, 0.f}, .target = {0.f, 0.f, 0.f}};
    const Transform transform = CameraTransformFor(aim);

    CHECK(Near(ForwardDirection(transform), {0.f, -1.f, 0.f}));
    const glm::mat4 view = ViewMatrix(transform);
    for (int32_t column = 0; column < 4; ++column)
    {
        for (int32_t row = 0; row < 4; ++row)
        {
            CHECK_FALSE(std::isnan(view[column][row]));
        }
    }
}
