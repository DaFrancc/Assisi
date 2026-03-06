/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Runtime/LightingSystem.hpp>
#include <Assisi/Runtime/LightComponents.hpp>
#include <Assisi/Runtime/Components.hpp>

namespace Assisi::Runtime
{

bool LightingSystem::Initialize(int width, int height, float nearZ, float farZ,
                                const glm::mat4 &projection)
{
    if (!_grid.Initialize())
        return false;

    _grid.BuildClusters(width, height, nearZ, farZ, glm::inverse(projection));
    return true;
}

void LightingSystem::Resize(int width, int height, float nearZ, float farZ,
                            const glm::mat4 &projection)
{
    _grid.BuildClusters(width, height, nearZ, farZ, glm::inverse(projection));
}

void LightingSystem::Update(Assisi::ECS::Scene &scene, const glm::mat4 &view)
{
    std::vector<Assisi::Render::PointLightGPU> pointLights;
    std::vector<Assisi::Render::SpotLightGPU>  spotLights;
    std::vector<Assisi::Render::DirLightGPU>   dirLights;

    for (auto [entity, transform, light] : scene.Query<TransformComponent, PointLightComponent>())
    {
        pointLights.push_back({
            .positionRadius = {transform.position, light.radius},
            .colorIntensity = {light.color, light.intensity},
        });
    }

    for (auto [entity, transform, light] : scene.Query<TransformComponent, SpotLightComponent>())
    {
        const float innerCos = glm::cos(glm::radians(light.innerAngle));
        const float outerCos = glm::cos(glm::radians(light.outerAngle));
        spotLights.push_back({
            .positionRadius = {transform.position, light.radius},
            .directionInner = {glm::normalize(light.direction), innerCos},
            .colorIntensity = {light.color, light.intensity},
            .outerCutoff    = outerCos,
        });
    }

    for (auto [entity, light] : scene.Query<DirectionalLightComponent>())
    {
        dirLights.push_back({
            .directionIntensity = {glm::normalize(light.direction), light.intensity},
            .colorPad           = {light.color, 0.f},
        });
    }

    _dirLightCount = static_cast<uint32_t>(dirLights.size());
    _grid.CullLights(pointLights, spotLights, dirLights, view);
}

void LightingSystem::SetupMeshShader(Assisi::Render::Shader &shader) const
{
    shader.Use();
    shader.SetUVec3("uGridDim",    Assisi::Render::ClusterGrid::kNumX,
                                   Assisi::Render::ClusterGrid::kNumY,
                                   Assisi::Render::ClusterGrid::kNumZ);
    shader.SetVec2("uScreenSize",  static_cast<float>(_grid.Width()),
                                   static_cast<float>(_grid.Height()));
    shader.SetFloat("uNearZ",  _grid.NearZ());
    shader.SetFloat("uFarZ",   _grid.FarZ());
}

} // namespace Assisi::Runtime