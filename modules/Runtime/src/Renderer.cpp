/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Renderer.hpp>

namespace Assisi::Runtime
{

void DrawScene(Assisi::ECS::Scene &scene, const glm::mat4 &view, const glm::mat4 &projection,
               nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *framebuffer, uint32_t viewportWidth,
               uint32_t viewportHeight, const Assisi::Render::MeshPass &meshPass)
{
    const glm::mat4 viewProjection = projection * view;

    for (auto [entity, transform, meshRenderer] : scene.Query<Transform, MeshRenderer>())
    {
        if (meshRenderer.mesh == nullptr)
        {
            continue;
        }

        const glm::mat4 modelViewProjection = viewProjection * transform.worldMatrix;
        nvrhi::ITexture *albedoTexture =
            meshRenderer.albedoTexture != nullptr ? meshRenderer.albedoTexture->NativeTexture() : nullptr;
        meshPass.Draw(commandList, framebuffer, viewportWidth, viewportHeight, modelViewProjection,
                      transform.worldMatrix, *meshRenderer.mesh, albedoTexture);
    }
}

} // namespace Assisi::Runtime
