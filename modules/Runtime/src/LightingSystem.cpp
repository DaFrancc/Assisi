/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Runtime/LightingSystem.hpp>
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

void LightingSystem::Update(nvrhi::ICommandList *commandList, Assisi::ECS::Scene &scene, const glm::mat4 &view)
{
    ASSISI_PROFILE_GPU_PASS(commandList, "lighting");

    // Reuse the staging buffers' capacity across frames; clear() keeps storage.
    _pointLights.clear();
    _spotLights.clear();
    _dirLights.clear();

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
            _pointLights.push_back({
                    .positionRadius = {glm::vec3(transform.worldMatrix[3]), light.radius},
                    .colorIntensity = {light.color, light.intensity},
                });
        }

        for (auto [entity, transform, light] : scene.Query<Transform, SpotLight>())
        {
            const float innerCos = glm::cos(glm::radians(light.innerAngle));
            const float outerCos = glm::cos(glm::radians(light.outerAngle));
            _spotLights.push_back({
                    .positionRadius = {glm::vec3(transform.worldMatrix[3]), light.radius},
                    .directionInner = {WorldSpotDirection(transform.worldMatrix, light.direction), innerCos},
                    .colorIntensity = {light.color, light.intensity},
                    .outerCutoff    = outerCos,
                });
        }

        for (auto [entity, light] : scene.Query<DirectionalLight>())
        {
            _dirLights.push_back({
                    .directionIntensity = {SafeDirection(light.direction), light.intensity},
                    .colorPad           = {light.color, 0.f},
                });
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
    {
        // CPU-side this is three buffer uploads plus a dispatch record, so it is
        // measuring the upload — the cull itself is GPU time and lands in frame/gpu-ms.
        ASSISI_PROFILE_GPU_SCOPE(commandList, "light-cull");
        _grid.CullLights(commandList, _pointLights, _spotLights, _dirLights, view);
    }
}

} // namespace Assisi::Runtime
