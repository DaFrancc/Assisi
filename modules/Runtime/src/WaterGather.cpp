/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Runtime/WaterGather.hpp>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/WorldMatrix.hpp>
#include <Assisi/Runtime/WindResolve.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Assisi::Runtime
{
namespace
{

/// The waves' highest crests stand about this many times the significant
/// height above the still water, and choppiness moves the surface as far again
/// sideways.
constexpr float kReachPerHeight = 2.0f;
/// An entity axis shorter than this has no direction across the world.
constexpr float kMinAxisLength = 1.0e-6f;

/// Which way @p wind blows across a body whose x axis is @p axisX, in the
/// body's frame; zero in still air.
glm::vec2 WindInBody(const Wind &wind, glm::vec2 axisX)
{
    if (!(wind.speed > 0.0f))
    {
        return glm::vec2(0.0f);
    }
    const glm::vec2 toward = GustsOf(wind).toward;
    return glm::vec2(glm::dot(toward, axisX), glm::dot(toward, glm::vec2(-axisX.y, axisX.x)));
}

Render::WaterLook LookOf(const Math::Color3<Math::ColorSpace::Linear> &color,
                         const Math::Color3<Math::ColorSpace::Linear> &crestColor, float clarity)
{
    return Render::WaterLook{.color = glm::max(glm::vec3(color), glm::vec3(0.0f)),
                             .crestColor = glm::max(glm::vec3(crestColor), glm::vec3(0.0f)),
                             .clarity = std::isfinite(clarity) ? std::max(clarity, kMinWaterClarity) : kMinWaterClarity};
}

uint64_t KeyOf(ECS::Entity entity)
{
    return static_cast<uint64_t>(entity.index) | (static_cast<uint64_t>(entity.generation) << 32u);
}

/// Sizes @p body's cascades and reach, picks its patches, and keeps it if any
/// is in view.
void AddBody(Render::WaterBody body, const WaterGatherView &view, Render::WaterFrame &frame)
{
    ++frame.placed;
    body.cascades = Render::LayoutCascades(body.waves);
    const float windHeight = Render::SignificantWaveHeight(Render::SpectrumOf(body.waves.wind));
    const float swellHeight = Render::SignificantWaveHeight(Render::SpectrumOf(body.waves.swell));
    body.significantHeight = std::sqrt(windHeight * windHeight + swellHeight * swellHeight);
    body.waveReach = kReachPerHeight * body.significantHeight * (1.0f + body.waves.choppiness);

    Render::SelectWaterNodes(
            body.region,
            Render::WaterGridView{.camera = view.camera,
                                  .frustum = view.frustum,
                                  .drawDistance = view.drawDistance,
                                  .waveReach = body.waveReach,
                                  .finestSpacing = Render::WaterFinestSpacing(Render::LongestWavelength(body.waves))},
            frame.scratch);
    if (frame.scratch.empty())
    {
        return;
    }
    body.firstNode = static_cast<uint32_t>(frame.nodes.size());
    body.nodeCount = static_cast<uint32_t>(frame.scratch.size());
    frame.nodes.insert(frame.nodes.end(), frame.scratch.begin(), frame.scratch.end());
    frame.bodies.push_back(body);
}

} // namespace

Render::WaterWaves OceanWaves(const Ocean &ocean, const Wind &wind)
{
    return Render::Sanitized(Render::WaterWaves{
            .wind = Render::WindSea{.windSpeed = wind.speed,
                                    .fetch = Render::kOceanFetch,
                                    .bearingDegrees = wind.bearingDegrees,
                                    .spread = ocean.spread},
            .swell = Render::Swell{.height = ocean.swellHeight,
                                   .period = ocean.swellPeriod,
                                   .bearingDegrees = ocean.swellBearingDegrees},
            .choppiness = ocean.choppiness,
            .seed = ocean.seed});
}

float LakeCrossing(const Wind &wind, glm::vec2 extent, glm::vec2 axisX)
{
    // Corner to corner along the wind, or the whole width where it blows
    // straight across.
    const glm::vec2 toward = GustsOf(wind).toward;
    const glm::vec2 inBody(std::abs(glm::dot(toward, axisX)), std::abs(glm::dot(toward, glm::vec2(-axisX.y, axisX.x))));
    float crossing = std::numeric_limits<float>::max();
    for (glm::length_t axis = 0; axis < glm::vec2::length(); ++axis)
    {
        if (inBody[axis] > kMinAxisLength)
        {
            crossing = std::min(crossing, extent[axis] / inBody[axis]);
        }
    }
    return crossing;
}

Render::WaterWaves LakeWaves(const Lake &lake, const Wind &wind, glm::vec2 extent, glm::vec2 axisX)
{
    const Render::LakeSettings settings = ToLakeSettings(lake);
    return Render::Sanitized(Render::WaterWaves{
            .wind = Render::WindSea{.windSpeed = wind.speed * (1.0f - settings.shelter),
                                    .fetch = LakeCrossing(wind, extent, axisX),
                                    .bearingDegrees = wind.bearingDegrees,
                                    .spread = settings.spread},
            .swell = Render::Swell{},
            .choppiness = settings.choppiness,
            .seed = settings.seed});
}

void GatherWater(const ECS::Scene &scene, const WaterGatherView &view, Render::WaterFrame &frame)
{
    frame.Clear();
    const Wind wind = ResolveWind(scene).wind;
    frame.gusts = GustsOf(wind);
    for (auto [entity, world, ocean] : scene.Query<ECS::WorldMatrix, Ocean>())
    {
        Render::WaterBody body;
        // Centred on the world's origin rather than the entity, so the patches
        // sit on the world's own grid wherever the entity was put.
        body.region.origin = glm::dvec3(0.0, ECS::WorldPositionOf(world).y, 0.0);
        body.key = KeyOf(entity);
        body.waves = OceanWaves(ocean, wind);
        body.windInBody = WindInBody(wind, body.region.axisX);
        body.look = LookOf(ocean.color, ocean.crestColor, ocean.clarity);
        AddBody(body, view, frame);
    }
    for (auto [entity, world, lake] : scene.Query<ECS::WorldMatrix, Lake>())
    {
        const glm::vec3 alongX(world.inCell[0]);
        const glm::vec3 alongZ(world.inCell[2]);
        const glm::vec2 flatX(alongX.x, alongX.z);
        const float flatLength = glm::length(flatX);
        if (flatLength < kMinAxisLength)
        {
            continue;
        }
        const glm::vec2 extent = lake.size * glm::vec2(glm::length(alongX), glm::length(alongZ));
        if (!(extent.x > 0.0f && extent.y > 0.0f))
        {
            continue;
        }
        const glm::vec2 axisX = flatX / flatLength;
        Render::WaterBody body;
        body.region = Render::WaterRegion{.origin = ECS::WorldPositionOf(world), .axisX = axisX, .halfExtent = 0.5f * extent};
        body.key = KeyOf(entity);
        const Render::LakeSettings settings = ToLakeSettings(lake);
        body.waves = LakeWaves(lake, wind, extent, axisX);
        body.windInBody = WindInBody(wind, axisX);
        body.crossing = LakeCrossing(wind, extent, axisX);
        body.windShare = 1.0f - settings.shelter;
        body.look = LookOf(Math::Color3<Math::ColorSpace::Linear>(settings.color),
                           Math::Color3<Math::ColorSpace::Linear>(settings.crestColor), settings.clarity);
        body.look.ripples = settings.ripples;
        AddBody(body, view, frame);
    }
}

} // namespace Assisi::Runtime
