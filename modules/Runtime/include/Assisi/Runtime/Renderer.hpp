#pragma once

/// @file Renderer.hpp
/// @brief ECS-driven draw pass: iterates TransformComponent + MeshRendererComponent.

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Shader.hpp>
#include <Assisi/Runtime/Camera.hpp>

namespace Assisi::Runtime
{

/// @brief Draws all entities in `scene` that have both a TransformComponent and a
///        MeshRendererComponent.
///
/// The shader must expose:
///   - uniform mat4  uModel
///   - uniform mat4  uView
///   - uniform mat4  uProjection
///   - uniform sampler2D uTexture  (bound to texture unit 0)
///
/// Entities whose MeshRendererComponent::textureId is 0 fall back to a 1×1 white texture.
/// Entities whose MeshRendererComponent::mesh is null are skipped silently.
///
/// @param scene       ECS scene to query.
/// @param camera      Provides the view matrix.
/// @param projection  Projection matrix (e.g. glm::perspective).
/// @param shader      Shader program to use; must already be loaded.
void DrawScene(Assisi::ECS::Scene &scene, const Camera &camera, const glm::mat4 &projection,
               Assisi::Render::Shader &shader);

} // namespace Assisi::Runtime