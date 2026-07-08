/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Renderer.hpp
/// @brief ECS-driven draw pass: iterates TransformComponent + MeshRendererComponent.

#include <cstdint>

#include <nvrhi/nvrhi.h>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/MeshPass.hpp>

namespace Assisi::Runtime
{

/// @brief Draws all entities in `scene` that have both a TransformComponent and a
///        MeshRendererComponent, using the shared `meshPass` pipeline.
///
/// Entities whose MeshRendererComponent::mesh is null are skipped silently.
///
/// @param scene       ECS scene to query.
/// @param view        View matrix (e.g. from Runtime::ViewMatrix).
/// @param projection  Projection matrix (e.g. from Runtime::ProjectionMatrix).
/// @param commandList Open command list to record draws into.
/// @param framebuffer Target framebuffer for this frame.
/// @param viewportWidth,viewportHeight  Framebuffer size in pixels.
/// @param meshPass    Shared pipeline; must already be initialized.
void DrawScene(Assisi::ECS::Scene &scene, const glm::mat4 &view, const glm::mat4 &projection,
               nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *framebuffer, uint32_t viewportWidth,
               uint32_t viewportHeight, const Assisi::Render::MeshPass &meshPass);

} // namespace Assisi::Runtime
