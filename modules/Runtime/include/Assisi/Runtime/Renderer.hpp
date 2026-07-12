/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Renderer.hpp
/// @brief ECS-driven draw pass: iterates Transform + MeshRenderer.

#include <cstdint>

#include <nvrhi/nvrhi.h>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/MeshPass.hpp>

namespace Assisi::Runtime
{

/// @brief How many mesh entities a DrawScene call submitted versus culled.
/// Useful as a live overlay readout and to confirm culling is actually firing.
struct DrawStats
{
    uint32_t drawn  = 0; ///< Entities whose draw was recorded.
    uint32_t culled = 0; ///< Entities skipped by frustum culling this pass.
};

/// @brief Draws all entities in `scene` that have both a Transform and a
///        MeshRenderer, using the shared `meshPass` pipeline.
///
/// Entities whose MeshRenderer::mesh is null are skipped silently. When
/// `frustumCulling` is true, entities whose world-space bounding sphere falls
/// entirely outside the view frustum are also skipped (the sphere test is
/// conservative, so nothing visible is ever culled); pass false to submit every
/// mesh unconditionally — useful for A/B comparing the cull's cost/benefit.
///
/// @param scene       ECS scene to query.
/// @param view        View matrix (e.g. from Runtime::ViewMatrix).
/// @param projection  Projection matrix (e.g. from Runtime::ProjectionMatrix).
/// @param commandList Open command list to record draws into.
/// @param framebuffer Target framebuffer for this frame.
/// @param viewportWidth,viewportHeight  Framebuffer size in pixels.
/// @param meshPass    Shared pipeline; must already be initialized.
/// @param frustumCulling  Skip meshes outside the view frustum (default true).
/// @return Counts of drawn and culled entities.
DrawStats DrawScene(Assisi::ECS::Scene &scene, const glm::mat4 &view, const glm::mat4 &projection,
                    nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *framebuffer, uint32_t viewportWidth,
                    uint32_t viewportHeight, const Assisi::Render::MeshPass &meshPass, bool frustumCulling = true);

} // namespace Assisi::Runtime
