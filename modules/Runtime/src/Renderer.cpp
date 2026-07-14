/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Render/Frustum.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Renderer.hpp>

namespace Assisi::Runtime
{

DrawStats DrawScene(Assisi::ECS::Scene &scene, const glm::mat4 &view, const glm::mat4 &projection,
                    nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *framebuffer, uint32_t viewportWidth,
                    uint32_t viewportHeight, const Assisi::Render::MeshPass &meshPass, bool frustumCulling)
{
    const glm::mat4 viewProjection = projection * view;

    // Cull each mesh's world-space bounding sphere against the view frustum before
    // recording its draw, so off-screen geometry costs a matrix-times-point and six
    // dot products instead of a full pipeline-state set + draw call.
    const Assisi::Render::Frustum frustum = Assisi::Render::Frustum::FromViewProjection(viewProjection);

    DrawStats stats;
    for (auto [entity, transform, meshRenderer] : scene.Query<Transform, MeshRenderer>())
    {
        if (meshRenderer.mesh == nullptr)
        {
            continue;
        }

        if (frustumCulling)
        {
            const Assisi::Geometry::BoundingSphere worldBounds =
                Assisi::Geometry::TransformedBoundingSphere(meshRenderer.mesh->LocalBounds(), transform.worldMatrix);
            if (!frustum.IntersectsSphere(worldBounds))
            {
                ++stats.culled;
                continue;
            }
        }

        const glm::mat4 modelViewProjection = viewProjection * transform.worldMatrix;
        nvrhi::ITexture *albedoTexture =
            meshRenderer.albedoTexture != nullptr ? meshRenderer.albedoTexture->NativeTexture() : nullptr;
        meshPass.Draw(commandList, framebuffer, viewportWidth, viewportHeight, modelViewProjection,
                      transform.worldMatrix, *meshRenderer.mesh, albedoTexture);
        ++stats.drawn;
    }
    return stats;
}

} // namespace Assisi::Runtime
