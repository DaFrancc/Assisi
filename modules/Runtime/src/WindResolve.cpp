/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Runtime/WindResolve.hpp>

#include <Assisi/ECS/Scene.hpp>

#include <algorithm>
#include <cmath>

namespace Assisi::Runtime
{
namespace
{

constexpr float kFullTurnDegrees = 360.f;

Wind Sanitized(Wind wind)
{
    wind.speed = std::isfinite(wind.speed) ? std::clamp(wind.speed, kStillAir.speed, kMaxWindSpeed) : kStillAir.speed;
    wind.bearingDegrees = std::isfinite(wind.bearingDegrees) ? std::fmod(wind.bearingDegrees, kFullTurnDegrees)
                                                             : kStillAir.bearingDegrees;
    wind.gustFactor = std::isfinite(wind.gustFactor) ? std::clamp(wind.gustFactor, kSteadyGustFactor, kMaxGustFactor)
                                                     : kDefaultGustFactor;
    return wind;
}

} // namespace

WindResolution ResolveWind(const ECS::Scene &scene)
{
    WindResolution resolution;
    for (auto [entity, wind] : scene.Query<Wind>())
    {
        (void)entity;
        ++resolution.count;
        if (resolution.count == 1)
        {
            resolution.wind = Sanitized(wind);
        }
    }
    return resolution;
}

Render::Gusts GustsOf(const Wind &wind)
{
    const float bearing = glm::radians(wind.bearingDegrees);
    // North is -z and east +x, so a bearing turns from -z toward +x.
    return Render::Gusts{.toward = glm::vec2(std::sin(bearing), -std::cos(bearing)),
                         .speed = wind.speed,
                         .gustFactor = wind.gustFactor};
}

} // namespace Assisi::Runtime
