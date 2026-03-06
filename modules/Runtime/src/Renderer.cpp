/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <glad/glad.h>

#include <Assisi/Render/DefaultResources.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Renderer.hpp>

namespace Assisi::Runtime
{

void DrawScene(Assisi::ECS::Scene &scene, const Camera &camera, const glm::mat4 &projection,
               Assisi::Render::Shader &shader)
{
    shader.Use();
    shader.SetMat4("uView", camera.ViewMatrix());
    shader.SetMat4("uProjection", projection);

    // Bind sampler uniforms to their fixed texture units
    shader.SetInt("uAlbedo",    0);
    shader.SetInt("uNormal",    1);
    shader.SetInt("uMetallic",  2);
    shader.SetInt("uRoughness", 3);

    for (auto [entity, transform, meshRenderer] : scene.Query<TransformComponent, MeshRendererComponent>())
    {
        if (meshRenderer.mesh == nullptr)
        {
            continue;
        }

        const glm::mat4 model = glm::translate(glm::mat4(1.f), transform.position) *
                                glm::mat4_cast(transform.rotation) * glm::scale(glm::mat4(1.f), transform.scale);
        shader.SetMat4("uModel", model);

        const unsigned int albedoId =
            meshRenderer.albedoTextureId != 0u
                ? meshRenderer.albedoTextureId
                : Assisi::Render::DefaultResources::WhiteTextureId();

        const unsigned int normalId =
            meshRenderer.normalTextureId != 0u
                ? meshRenderer.normalTextureId
                : Assisi::Render::DefaultResources::FlatNormalTextureId();

        const unsigned int metallicId =
            meshRenderer.metallicTextureId != 0u
                ? meshRenderer.metallicTextureId
                : Assisi::Render::DefaultResources::BlackTextureId();

        const unsigned int roughnessId =
            meshRenderer.roughnessTextureId != 0u
                ? meshRenderer.roughnessTextureId
                : Assisi::Render::DefaultResources::GreyTextureId();

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, albedoId);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, normalId);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, metallicId);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, roughnessId);

        meshRenderer.mesh->Bind();
        glDrawElements(GL_TRIANGLES, static_cast<int>(meshRenderer.mesh->IndexCount()), GL_UNSIGNED_INT, nullptr);
    }
}

} // namespace Assisi::Runtime