/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Runtime/WaterGather.hpp>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/WorldMatrix.hpp>
#include <Assisi/Runtime/WindResolve.hpp>

#include <algorithm>
#include <cmath>

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

glm::vec2 BearingDirection(float degrees)
{
    const float bearing = glm::radians(degrees);
    // North is -z and east +x, so a bearing turns from -z toward +x.
    return glm::vec2(std::sin(bearing), -std::cos(bearing));
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

    Render::SelectWaterNodes(body.region,
                             Render::WaterGridView{.camera = view.camera,
                                                   .frustum = view.frustum,
                                                   .drawDistance = view.drawDistance,
                                                   .waveReach = body.waveReach},
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

Render::WaterWaves LakeWaves(const Lake &lake, const Wind &wind, glm::vec2 extent, glm::vec2 axisX)
{
    const glm::vec2 along = BearingDirection(wind.bearingDegrees);
    const glm::vec2 axisZ(-axisX.y, axisX.x);
    const float fetch = std::abs(glm::dot(along, axisX)) * extent.x + std::abs(glm::dot(along, axisZ)) * extent.y;
    return Render::Sanitized(Render::WaterWaves{
            .wind = Render::WindSea{.windSpeed = wind.speed,
                                    .fetch = fetch,
                                    .bearingDegrees = wind.bearingDegrees,
                                    .spread = lake.spread},
            .swell = Render::Swell{},
            .choppiness = lake.choppiness,
            .seed = lake.seed});
}

void GatherWater(const ECS::Scene &scene, const WaterGatherView &view, Render::WaterFrame &frame)
{
    frame.Clear();
    const Wind wind = ResolveWind(scene).wind;
    for (auto [entity, world, ocean] : scene.Query<ECS::WorldMatrix, Ocean>())
    {
        Render::WaterBody body;
        // Centred on the world's origin rather than the entity, so the patches
        // sit on the world's own grid wherever the entity was put.
        body.region.origin = glm::dvec3(0.0, ECS::WorldPositionOf(world).y, 0.0);
        body.key = KeyOf(entity);
        body.waves = OceanWaves(ocean, wind);
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
        body.waves = LakeWaves(lake, wind, extent, axisX);
        body.look = LookOf(lake.color, lake.crestColor, lake.clarity);
        AddBody(body, view, frame);
    }
}

} // namespace Assisi::Runtime
