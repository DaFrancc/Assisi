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

namespace Assisi::Render
{
class MeshCuller;
class CullTableBuilder;
} // namespace Assisi::Render

namespace Assisi::Runtime
{

/// @brief What one DrawScene call produced and consumed: how much geometry
/// survived culling and how far the indirect submission collapsed it (stage E).
/// A live overlay readout — and the seam's measurable payoff: sorting places
/// identical same-material meshes adjacent so they coalesce, so `batches` falls
/// toward the count of distinct meshes; with sorting off (A/B toggle) it climbs
/// toward drawnItems (every item its own batch).
///
/// On the GPU-cull path (stages F1/F2a) the cull runs on the GPU and its survivor
/// tallies are read back (a few frames stale): `drawnItems` is the surviving
/// instances, `batches` is the coalesced instanced draws (F2a collapses identical
/// (mesh,submesh) instances, so `batches` << `drawnItems`), `culledMeshes` is the
/// culled instances (candidates − survivors), and `drawCalls` is the single
/// drawIndexedIndirect over all batch commands.
struct DrawStats
{
    uint32_t drawnItems   = 0; ///< DrawItems (visible submeshes) submitted == instances.
    uint32_t culledMeshes = 0; ///< Whole mesh entities skipped by frustum culling.
    uint32_t batches      = 0; ///< Instanced draw commands after coalescing same-geometry runs.
    uint32_t drawCalls    = 0; ///< drawIndexedIndirect(Count) API calls issued (~1 with one arena).
};

/// @brief Everything one DrawScene call needs, grouped so the call site reads as
/// named fields rather than a dozen positional arguments. The three references
/// (scene, meshPass, frame) are required and must outlive the call; the rest have
/// sensible defaults. Built at the call site with designated initializers.
struct DrawSceneParams
{
    Assisi::ECS::Scene &scene;           ///< ECS scene to draw.
    const Assisi::Render::MeshPass &meshPass; ///< Shared pipeline; must be initialized.
    const Assisi::Render::RenderFrame &frame; ///< Command list + framebuffer + viewport size.

    glm::mat4 view{1.f};       ///< View matrix (e.g. Runtime::ViewMatrix).
    glm::mat4 projection{1.f}; ///< Projection matrix (e.g. Runtime::ProjectionMatrix).
    float nearZ = 0.f;         ///< Camera near plane, for the sort key's depth quantization.
    float farZ  = 0.f;         ///< Camera far plane.

    bool frustumCulling = true; ///< Skip meshes outside the view frustum.
    bool sortDraws      = true; ///< Sort the draw list by sort key before submitting.

    /// @brief Take the GPU-driven cull path (stage F1) instead of the CPU
    /// extract/sort path. Requires @ref culler and @ref cullBuilder; falls back to
    /// the CPU path when either is null or the culler isn't initialized. An A/B
    /// toggle against the CPU path (the opaque image is identical). `sortDraws` is
    /// ignored on this path (the GPU appends draws in atomic order); `frustumCulling`
    /// still gates the GPU frustum test.
    bool gpuCulling = false;
    /// GPU cull pass; must outlive the call when @ref gpuCulling is set.
    Assisi::Render::MeshCuller *culler = nullptr;
    /// Reused per-frame table builder for the GPU path (avoids re-allocating the
    /// host-side tables each frame); must outlive the call when @ref gpuCulling is set.
    Assisi::Render::CullTableBuilder *cullBuilder = nullptr;
};

/// @brief Extract, sort, and submit a draw list for every Transform+MeshRenderer
///        entity in the scene, through the shared mesh pass.
///
/// The producer half: each entity whose MeshRenderer is resolved is whole-mesh
/// frustum-culled (a cheap sphere reject then an AABB refine, both conservative —
/// nothing visible is ever culled), its LOD0 submeshes emitted as one DrawItem
/// each (skipping slots with no resolved material), and — when `sortDraws` is
/// true — the list is sorted by
/// DrawItem::sortKey so MeshPass::Submit records it in material/mesh-major,
/// front-to-back order. `frustumCulling` false submits every mesh; `sortDraws`
/// false submits in query order — both for A/B comparing the seam (the image is
/// identical either way, only the bind counts change).
///
/// @return Drawn/culled counts and the submission's state-change tally.
DrawStats DrawScene(const DrawSceneParams &params);

} // namespace Assisi::Runtime
