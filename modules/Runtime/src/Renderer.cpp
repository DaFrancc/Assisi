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
    shader.SetInt("uDiffuse", 0);

    for (auto [entity, transform, meshRenderer] : scene.Query<TransformComponent, MeshRendererComponent>())
    {
        if (meshRenderer.mesh == nullptr)
        {
            continue;
        }

        const glm::mat4 model = glm::translate(glm::mat4(1.f), transform.position) *
                                glm::mat4_cast(transform.rotation) * glm::scale(glm::mat4(1.f), transform.scale);
        shader.SetMat4("uModel", model);

        const unsigned int texId =
            meshRenderer.textureId != 0u ? meshRenderer.textureId : Assisi::Render::DefaultResources::WhiteTextureId();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texId);

        meshRenderer.mesh->Bind();
        glDrawElements(GL_TRIANGLES, static_cast<int>(meshRenderer.mesh->IndexCount()), GL_UNSIGNED_INT, nullptr);
    }
}

} // namespace Assisi::Runtime