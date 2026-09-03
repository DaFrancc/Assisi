/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Runtime/LightingSystem.hpp>

#include <Assisi/Runtime/SkyComponents.hpp>
#include <Assisi/Runtime/LightComponents.hpp>
#include <Assisi/Runtime/Components.hpp>

#include <Assisi/Render/GpuMarker.hpp>

#include <algorithm>
#include <cstdint>

namespace Assisi::Runtime
{

namespace
{
// glm::normalize of a zero vector is NaN, and a zero direction can come
// straight from a hand-edited level file — fall back to straight down
// instead of NaN-poisoning the GPU light buffer.
constexpr glm::vec3 kFallbackLightDirection{0.f, -1.f, 0.f};

glm::vec3 SafeDirection(const glm::vec3 &direction)
{
    const float lengthSq = glm::dot(direction, direction);
    return lengthSq > 0.f ? direction / glm::sqrt(lengthSq) : kFallbackLightDirection;
}

constexpr ShadowCaster AsShadowCaster(bool castsShadows)
{
    return castsShadows ? ShadowCaster::Yes : ShadowCaster::No;
}
} // namespace

bool LightingSystem::Initialize(nvrhi::IDevice *device, nvrhi::ICommandList *commandList, int32_t width,
                                int32_t height, float nearZ, float farZ, const glm::mat4 &projection)
{
    if (!_grid.Initialize(device))
        return false;

    _grid.BuildClusters(commandList, width, height, nearZ, farZ, glm::inverse(projection));
    return true;
}

void LightingSystem::Resize(nvrhi::ICommandList *commandList, int32_t width, int32_t height, float nearZ, float farZ,
                            const glm::mat4 &projection)
{
    _grid.BuildClusters(commandList, width, height, nearZ, farZ, glm::inverse(projection));
}

glm::vec3 LightingSystem::WorldSpotDirection(const glm::mat4 &worldMatrix, const glm::vec3 &localDirection)
{
    return SafeDirection(glm::mat3(worldMatrix) * localDirection);
}

glm::vec3 AdvanceDaylight(const DirectionalLight &light, float seconds)
{
    // A light not on the cycle is left exactly where it was aimed, which is what
    // makes running this over every directional light cost one compare on the
    // ones that are not.
    if (!light.daylightCycle || !std::isfinite(seconds))
    {
        return light.direction;
    }
    const float period = std::max(light.daylightPeriodSeconds, kMinDaylightPeriodSeconds);

    // Turned about a horizontal axis, which is what makes this a day rather than
    // a circuit. Turning about world up would sweep the sun around at whatever
    // elevation it was authored at and never take it below the horizon — a polar
    // summer, with no night in it — and it would leave the default sun, aimed
    // straight down, exactly where it started: that aim is parallel to world up,
    // so it maps onto itself and nothing moves at all.
    //
    // Which horizontal axis is arbitrary without somewhere on a planet to stand,
    // so it is world X: the sun rises on one side, passes overhead and sets on
    // the other. An author who wants a different bearing turns the level or aims
    // the light off-axis, and an aim off this axis traces a tilted arc, which is
    // what a day away from the equator looks like.
    const float turns = seconds / period;
    const float angle = turns * glm::two_pi<float>();
    const glm::mat4 rotation = glm::rotate(glm::mat4(1.f), angle, kDaylightAxis);
    return SafeDirection(glm::vec3(rotation * glm::vec4(light.direction, 0.f)));
}

glm::vec3 LightingSystem::SunlightColor(const glm::vec3 &color, const glm::vec3 &directionToSun,
                                        const Render::SkySettings *atmosphere)
{
    if (atmosphere == nullptr)
    {
        return color;
    }
    // White through the air, not the authored colour through it: while
    // tintedBySky is on the sky IS the colour, and the inspector greys the
    // authored one out to say so. Multiplying it in here would make that grey a
    // lie.
    return Render::SunlightTransmittance(directionToSun, *atmosphere);
}

std::optional<LightingSystem::ShadowSun> LightingSystem::ShadowCastingSun() const
{
    // Bounded by _dirLightCount rather than the vector's size: lights past the
    // buffer's capacity were dropped on upload, so the shader has no index for
    // them and a shadow map for one would light nothing.
    for (uint32_t i = 0; i < _dirLightCount; ++i)
    {
        if (_dirShadowFlags[i] == ShadowCaster::Yes)
        {
            return ShadowSun{.index = i, .direction = glm::vec3(_dirLights[i].directionIntensity)};
        }
    }
    return std::nullopt;
}

void LightingSystem::SetSpotShadowView(uint32_t index, uint32_t firstView)
{
    if (index < _spotLights.size())
    {
        _spotLights[index].shadowView = firstView;
    }
}

void LightingSystem::SetPointShadowView(uint32_t index, uint32_t firstView)
{
    if (index < _pointLights.size())
    {
        _pointLights[index].shadowView.x = firstView;
    }
}

void LightingSystem::Update(nvrhi::ICommandList *commandList, Assisi::ECS::Scene &scene, const glm::mat4 &view)
{
    Gather(scene);
    Upload(commandList, view);
}

void LightingSystem::Gather(Assisi::ECS::Scene &scene)
{
    // Reuse the staging buffers' capacity across frames; clear() keeps storage.
    _pointLights.clear();
    _spotLights.clear();
    _dirLights.clear();
    _pointShadowFlags.clear();
    _spotShadowFlags.clear();
    _dirShadowFlags.clear();
    _shadowSpots.clear();
    _shadowPoints.clear();

    // World position comes from the propagated worldMatrix, not transform.position:
    // a parented light's local position is relative to its parent. For a root,
    // worldMatrix[3] equals position.
    //
    // A spot light's `direction` is LOCAL and is rotated into world space by the
    // same matrix, so a headlight or a held torch aims where its parent faces — and
    // an unparented light is aimed by its own rotation. A direction is a vector, not
    // a normal, so the plain upper-left 3x3 is the correct transform (no
    // inverse-transpose); SafeDirection renormalises, absorbing any scale.
    //
    // One scope over all three queries: same CPU-side staging-array rebuild, and the
    // per-type split is already in the counters below.
    {
        ASSISI_PROFILE_SCOPE("light-gather");

        for (auto [entity, transform, light] : scene.Query<Transform, PointLight>())
        {
            const glm::vec3 position = glm::vec3(transform.worldMatrix[3]);
            // Every light starts holding no tile. The shadow pass stamps the
            // winners between here and Upload, so a light it never reaches is
            // unshadowed by default rather than by last frame's answer.
            _pointLights.push_back({
                    .positionRadius = {position, light.radius},
                    .colorIntensity = {light.color, light.intensity},
                    .shadowView = {Render::kNoShadowView, 0u, 0u, 0u},
                });
            _pointShadowFlags.push_back(AsShadowCaster(light.castsShadows));
            if (light.castsShadows)
            {
                _shadowPoints.push_back(LocalLight{
                        .index = static_cast<uint32_t>(_pointLights.size() - 1u),
                        .position = position,
                        .range = light.radius,
                        .intensity = light.intensity,
                        .shadowPriority = light.shadowPriority,
                        .shadowAlwaysOn = light.shadowAlwaysOn});
            }
        }

        for (auto [entity, transform, light] : scene.Query<Transform, SpotLight>())
        {
            const float innerCos = glm::cos(glm::radians(light.innerAngle));
            const float outerCos = glm::cos(glm::radians(light.outerAngle));
            const glm::vec3 position = glm::vec3(transform.worldMatrix[3]);
            const glm::vec3 direction = WorldSpotDirection(transform.worldMatrix, light.direction);
            _spotLights.push_back({
                    .positionRadius = {position, light.radius},
                    .directionInner = {direction, innerCos},
                    .colorIntensity = {light.color, light.intensity},
                    .outerCutoff    = outerCos,
                    .shadowView     = Render::kNoShadowView,
                });
            _spotShadowFlags.push_back(AsShadowCaster(light.castsShadows));
            if (light.castsShadows)
            {
                _shadowSpots.push_back(LocalLight{
                        .index = static_cast<uint32_t>(_spotLights.size() - 1u),
                        .position = position,
                        .direction = direction,
                        .range = light.radius,
                        .intensity = light.intensity,
                        .outerAngleDegrees = light.outerAngle,
                        .shadowPriority = light.shadowPriority,
                        .shadowAlwaysOn = light.shadowAlwaysOn});
            }
        }

        for (auto [entity, light] : scene.Query<DirectionalLight>())
        {
            // A sun taking its colour from the sky is lit by the beam as it
            // arrives at the ground rather than as it left: dimmer and oranger the
            // lower it is, which is what makes a lit world agree with the sky over
            // it. Extinction dims as well as tints, and both ride in the colour —
            // the authored intensity stays exactly what was authored.
            //
            // Only what lights surfaces is tinted. The sky scatters the light's
            // own `color` and applies this same extinction itself, so nothing
            // applies it twice.
            const Skybox *skybox = light.tintedBySky ? scene.Get<Skybox>(entity) : nullptr;
            const glm::vec3 direction = SafeDirection(light.direction);
            const std::optional<Render::SkySettings> atmosphere =
                skybox != nullptr ? std::optional<Render::SkySettings>(ToSkySettings(*skybox)) : std::nullopt;
            const glm::vec3 color =
                SunlightColor(AuthoredSunColor(light), -direction, atmosphere ? &*atmosphere : nullptr);

            _dirLights.push_back({
                    .directionIntensity = {direction, light.intensity},
                    .colorPad           = {color, 0.f},
                });
            _dirShadowFlags.push_back(AsShadowCaster(light.castsShadows));
        }
    }

    // The gather is linear in these, so they are what `light-gather` should be read
    // against — and what says whether a froxel-cull cost is the scene's fault.
    ASSISI_PROFILE_COUNTER("lights/point", static_cast<double>(_pointLights.size()));
    ASSISI_PROFILE_COUNTER("lights/spot", static_cast<double>(_spotLights.size()));
    ASSISI_PROFILE_COUNTER("lights/dir", static_cast<double>(_dirLights.size()));

    // Clamped for the same reason CullLights clamps its counts: Upload
    // truncates at capacity and the shader must not read past it.
    _dirLightCount = std::min(static_cast<uint32_t>(_dirLights.size()), Render::ClusterGrid::kMaxDirLights);
}

void LightingSystem::Upload(nvrhi::ICommandList *commandList, const glm::mat4 &view)
{
    ASSISI_PROFILE_GPU_PASS(commandList, "lighting");

    // CPU-side this is three buffer uploads plus a dispatch record, so it is
    // measuring the upload — the cull itself is GPU time and lands in frame/gpu-ms.
    ASSISI_PROFILE_GPU_SCOPE(commandList, "light-cull");
    _grid.CullLights(commandList, _pointLights, _spotLights, _dirLights, view);
}

} // namespace Assisi::Runtime
