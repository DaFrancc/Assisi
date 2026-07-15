/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Renderer.hpp
/// @brief ECS-driven draw pass: iterates Transform + MeshRenderer.

#include <cstdint>

#include <nvrhi/nvrhi.h>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/MeshPass.hpp>
#include <Assisi/Render/RenderFrame.hpp>

namespace Assisi::Runtime
{

/// @brief What one DrawScene call produced and consumed: how much geometry
/// survived culling and how many GPU state changes the sorted submission
/// reduced to. A live overlay readout — and the seam's measurable payoff: with
/// sorting on, materialBinds/meshBinds fall toward the count of distinct
/// materials/meshes; with it off (A/B toggle) they climb toward drawnItems.
struct DrawStats
{
    uint32_t drawnItems    = 0; ///< DrawItems (visible submeshes) submitted.
    uint32_t culledMeshes  = 0; ///< Whole mesh entities skipped by frustum culling.
    uint32_t materialBinds = 0; ///< Distinct binding-set runs the sort reduced to.
    uint32_t meshBinds     = 0; ///< Distinct vertex/index-buffer runs the sort reduced to.
};

/// @brief Everything one DrawScene call needs, grouped so the call site reads as
/// named fields rather than a dozen positional arguments. The three references
/// (scene, meshPass, frame) are required and must outlive the call; the rest have
/// sensible defaults. Built at the call site with designated initializers.
struct DrawSceneParams
{
    Assisi::ECS::Scene        &scene;    ///< ECS scene to draw.
    const Assisi::Render::MeshPass &meshPass; ///< Shared pipeline; must be initialized.
    const Assisi::Render::RenderFrame &frame; ///< Command list + framebuffer + viewport size.

    glm::mat4 view{1.f};       ///< View matrix (e.g. Runtime::ViewMatrix).
    glm::mat4 projection{1.f}; ///< Projection matrix (e.g. Runtime::ProjectionMatrix).
    float     nearZ = 0.f;     ///< Camera near plane, for the sort key's depth quantization.
    float     farZ  = 0.f;     ///< Camera far plane.

    bool frustumCulling = true; ///< Skip meshes outside the view frustum.
    bool sortDraws      = true; ///< Sort the draw list by sort key before submitting.
};

/// @brief Extract, sort, and submit a draw list for every Transform+MeshRenderer
///        entity in the scene, through the shared mesh pass.
///
/// The producer half: each entity whose MeshRenderer is resolved is whole-mesh
/// frustum-culled (conservative sphere test — nothing visible is ever culled),
/// its LOD0 submeshes emitted as one DrawItem each (skipping slots with no
/// resolved material), and — when `sortDraws` is true — the list is sorted by
/// DrawItem::sortKey so MeshPass::Submit records it in material/mesh-major,
/// front-to-back order. `frustumCulling` false submits every mesh; `sortDraws`
/// false submits in query order — both for A/B comparing the seam (the image is
/// identical either way, only the bind counts change).
///
/// @return Drawn/culled counts and the submission's state-change tally.
DrawStats DrawScene(const DrawSceneParams &params);

} // namespace Assisi::Runtime
