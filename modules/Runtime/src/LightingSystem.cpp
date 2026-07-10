/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Runtime/LightingSystem.hpp>
#include <Assisi/Runtime/LightComponents.hpp>
#include <Assisi/Runtime/Components.hpp>

namespace Assisi::Runtime
{

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

    for (auto [entity, transform, light] : scene.Query<Transform, PointLight>())
    {
        _pointLights.push_back({
            .positionRadius = {transform.position, light.radius},
            .colorIntensity = {light.color, light.intensity},
        });
    }

    for (auto [entity, transform, light] : scene.Query<Transform, SpotLight>())
    {
        const float innerCos = glm::cos(glm::radians(light.innerAngle));
        const float outerCos = glm::cos(glm::radians(light.outerAngle));
        _spotLights.push_back({
            .positionRadius = {transform.position, light.radius},
            .directionInner = {glm::normalize(light.direction), innerCos},
            .colorIntensity = {light.color, light.intensity},
            .outerCutoff    = outerCos,
        });
    }

    for (auto [entity, light] : scene.Query<DirectionalLight>())
    {
        _dirLights.push_back({
            .directionIntensity = {glm::normalize(light.direction), light.intensity},
            .colorPad           = {light.color, 0.f},
        });
    }

    _dirLightCount = static_cast<uint32_t>(_dirLights.size());
    _grid.CullLights(commandList, _pointLights, _spotLights, _dirLights, view);
}

} // namespace Assisi::Runtime
