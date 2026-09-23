/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/CameraPath.hpp>

#include <algorithm>
#include <cmath>

namespace Assisi::Runtime
{
namespace
{
/// Below this, a direction is too short to normalise or to build a basis from.
constexpr float kDegenerateLength = 1e-6f;

/// |dot(forward, up)| above this means the view is straight up or down, where
/// world +Y cannot orient the camera.
constexpr float kParallelDot = 0.999f;

[[nodiscard]] float LegSeconds(const CameraRouteLeg &leg)
{
    return leg.isCircle ? leg.circle.seconds : leg.line.seconds;
}

[[nodiscard]] std::int32_t LegOrder(const CameraRouteLeg &leg)
{
    return leg.isCircle ? leg.circle.order : leg.line.order;
}

[[nodiscard]] CameraAim EvaluateLine(const CameraLinePath &line, float progress)
{
    CameraAim aim;
    aim.eye = glm::mix(line.from, line.to, progress);

    const glm::vec3 travel = line.to - line.from;
    // A zero-length line has no direction to look along, so it falls back to
    // the authored target rather than aiming at its own eye.
    const bool alongPath = line.lookAlongPath && glm::length(travel) > kDegenerateLength;
    aim.target = alongPath ? aim.eye + travel : line.lookAt;
    return aim;
}

[[nodiscard]] CameraAim EvaluateCircle(const CameraCirclePath &circle, float progress)
{
    const float angle = glm::radians(circle.startDegrees + circle.sweepDegrees * progress);

    CameraAim aim;
    aim.eye = circle.center +
              glm::vec3(std::cos(angle) * circle.radius, circle.height, std::sin(angle) * circle.radius);
    aim.target = circle.lookAt;
    return aim;
}
} // namespace

std::vector<CameraRouteLeg> BuildCameraRoute(ECS::Scene &scene)
{
    std::vector<CameraRouteLeg> route;
    for (auto [entity, line] : scene.Query<CameraLinePath>())
    {
        if (line.seconds > 0.f)
        {
            route.push_back({.line = line, .circle = {}, .entity = entity, .isCircle = false});
        }
    }
    for (auto [entity, circle] : scene.Query<CameraCirclePath>())
    {
        if (circle.seconds > 0.f)
        {
            route.push_back({.line = {}, .circle = circle, .entity = entity, .isCircle = true});
        }
    }

    // Stable on the entity index as well as the order, so a tie resolves the
    // same way whichever pool the query walked first.
    std::sort(route.begin(), route.end(),
              [](const CameraRouteLeg &a, const CameraRouteLeg &b)
        {
            if (LegOrder(a) != LegOrder(b))
            {
                return LegOrder(a) < LegOrder(b);
            }
            return a.entity.index < b.entity.index;
        });
    return route;
}

float CameraRouteSeconds(const std::vector<CameraRouteLeg> &route)
{
    float total = 0.f;
    for (const CameraRouteLeg &leg : route)
    {
        total += LegSeconds(leg);
    }
    return total;
}

CameraAim EvaluateCameraLeg(const CameraRouteLeg &leg, float progress)
{
    const float clamped = std::clamp(progress, 0.f, 1.f);
    return leg.isCircle ? EvaluateCircle(leg.circle, clamped) : EvaluateLine(leg.line, clamped);
}

CameraAim EvaluateCameraRoute(const std::vector<CameraRouteLeg> &route, float seconds)
{
    if (route.empty())
    {
        return {};
    }

    float legStart = 0.f;
    for (const CameraRouteLeg &leg : route)
    {
        const float length = LegSeconds(leg);
        if (seconds < legStart + length)
        {
            return EvaluateCameraLeg(leg, (seconds - legStart) / length);
        }
        legStart += length;
    }
    return EvaluateCameraLeg(route.back(), 1.f);
}

Transform CameraTransformFor(const CameraAim &aim)
{
    glm::vec3 forward = aim.target - aim.eye;
    if (glm::length(forward) <= kDegenerateLength)
    {
        forward = glm::vec3(0.f, 0.f, -1.f);
    }
    forward = glm::normalize(forward);

    const bool vertical = std::abs(glm::dot(forward, glm::vec3(0.f, 1.f, 0.f))) > kParallelDot;
    const glm::vec3 up = vertical ? glm::vec3(0.f, 0.f, 1.f) : glm::vec3(0.f, 1.f, 0.f);

    Transform transform;
    transform.worldMatrix = glm::inverse(glm::lookAt(aim.eye, aim.eye + forward, up));
    transform.position = aim.eye;
    transform.rotation = glm::quat_cast(glm::mat3(transform.worldMatrix));
    return transform;
}

} // namespace Assisi::Runtime
