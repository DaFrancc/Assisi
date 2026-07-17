/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Runtime/LightingSystem.hpp>
#include <Assisi/Runtime/LightComponents.hpp>
#include <Assisi/Runtime/Components.hpp>

#include <algorithm>

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

bool LightingSystem::Initialize(nvrhi::IDevice *device, nvrhi::ICommandList *commandList, int width, int height,
                                float nearZ, float farZ, const glm::mat4 &projection)
{
    if (!_grid.Initialize(device))
        return false;

    _grid.BuildClusters(commandList, width, height, nearZ, farZ, glm::inverse(projection));
    return true;
}

void LightingSystem::Resize(nvrhi::ICommandList *commandList, int width, int height, float nearZ, float farZ,
                            const glm::mat4 &projection)
{
    _grid.BuildClusters(commandList, width, height, nearZ, farZ, glm::inverse(projection));
}

void LightingSystem::Update(nvrhi::ICommandList *commandList, Assisi::ECS::Scene &scene, const glm::mat4 &view)
{
    // Reuse the staging buffers' capacity across frames; clear() keeps storage.
    _pointLights.clear();
    _spotLights.clear();
    _dirLights.clear();

    // World position lives in the propagated worldMatrix, not the local
    // transform.position: a parented light's local position is relative to its
    // parent, so reading it directly placed the light at the wrong spot. For a root
    // (no parent) worldMatrix[3] equals position, so unparented lights are unchanged.
    // (Spot *direction* is still taken from light.direction as-is — whether it should
    // rotate with the parent is a separate open decision; see the round-6 review M2.)
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
            .directionInner = {SafeDirection(light.direction), innerCos},
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

    // Clamped for the same reason CullLights clamps its counts: Upload
    // truncates at capacity and the shader must not read past it.
    _dirLightCount = std::min(static_cast<uint32_t>(_dirLights.size()), Render::ClusterGrid::kMaxDirLights);
    _grid.CullLights(commandList, _pointLights, _spotLights, _dirLights, view);
}

} // namespace Assisi::Runtime
