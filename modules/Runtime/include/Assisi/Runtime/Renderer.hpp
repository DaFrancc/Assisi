#pragma once

/// @file Renderer.hpp
/// @brief ECS-driven draw pass: iterates TransformComponent + MeshRendererComponent.

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Shader.hpp>

namespace Assisi::Runtime
{

/// @brief Draws all entities in `scene` that have both a TransformComponent and a
///        MeshRendererComponent.
///
/// The shader must expose:
///   - uniform mat4       uModel, uView, uProjection
///   - uniform sampler2D  uAlbedo    (unit 0)
///   - uniform sampler2D  uNormal    (unit 1)
///   - uniform sampler2D  uMetallic  (unit 2)
///   - uniform sampler2D  uRoughness (unit 3)
///
/// Zero texture IDs in MeshRendererComponent fall back to engine defaults.
/// Entities whose MeshRendererComponent::mesh is null are skipped silently.
///
/// @param scene       ECS scene to query.
/// @param view        View matrix (e.g. from Runtime::ViewMatrix).
/// @param projection  Projection matrix (e.g. from Runtime::ProjectionMatrix).
/// @param shader      Shader program to use; must already be loaded.
void DrawScene(Assisi::ECS::Scene &scene, const glm::mat4 &view, const glm::mat4 &projection,
               Assisi::Render::Shader &shader);

} // namespace Assisi::Runtime