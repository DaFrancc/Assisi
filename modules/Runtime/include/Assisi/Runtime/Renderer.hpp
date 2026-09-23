/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Renderer.hpp
/// @brief ECS-driven draw pass: iterates Transform + MeshRenderer.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <nvrhi/nvrhi.h>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/LocalShadowPass.hpp>
#include <Assisi/Runtime/LodSelection.hpp>
#include <Assisi/Render/MeshPass.hpp>
#include <Assisi/Render/RenderFrame.hpp>
#include <Assisi/Render/ShadowDepthRenderer.hpp>

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
/// On the GPU-cull path the cull and LOD selection run on the GPU and their
/// tallies are read back (a few frames stale): `drawnItems` is the surviving
/// instances, `batches` is the coalesced instanced draws (identical
/// (mesh,submesh) instances collapse, so `batches` << `drawnItems`),
/// `culledMeshes` is the objects the frustum rejected, `lodInstances` is the
/// survivors per level, and `drawCalls` is the single drawIndexedIndirect over
/// all batch commands.
struct DrawStats
{
    uint32_t drawnItems = 0;   ///< DrawItems (visible submeshes) submitted == instances.
    uint32_t culledMeshes = 0; ///< Whole mesh entities skipped by frustum culling.
    uint32_t batches = 0;      ///< Instanced draw commands after coalescing same-geometry runs.
    uint32_t drawCalls = 0;    ///< drawIndexedIndirect(Count) API calls issued (~1 with one arena).

    /// Mesh instances drawn at each LOD level, LOD0 first; levels past the last
    /// bucket fold into it. Counted per instance rather than per submesh, so it
    /// reads against the entity count rather than against `drawnItems`, and a
    /// scene of single-level meshes puts everything in bucket 0.
    std::array<uint32_t, kMaxReportedLods> lodInstances{};
};

/// @brief Everything one DrawScene call needs, grouped so the call site reads as
/// named fields rather than a dozen positional arguments. The three references
/// (scene, meshPass, frame) are required and must outlive the call; the rest have
/// sensible defaults. Built at the call site with designated initializers.
struct DrawSceneParams
{
    Assisi::ECS::Scene &scene;                ///< ECS scene to draw.
    const Assisi::Render::MeshPass &meshPass; ///< Shared pipeline; must be initialized.
    const Assisi::Render::RenderFrame &frame; ///< Command list + framebuffer + viewport size.

    glm::mat4 view{1.f};       ///< View matrix (e.g. Runtime::ViewMatrix).
    glm::mat4 projection{1.f}; ///< Projection matrix (e.g. Runtime::ProjectionMatrix).
    float nearZ = 0.f;         ///< Camera near plane, for the sort key's depth quantization.
    float farZ = 0.f;          ///< Camera far plane.

    bool frustumCulling = true; ///< Skip meshes outside the view frustum.
    bool sortDraws = true;      ///< Sort the draw list by sort key before submitting.

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

    /// Where the LOD level of each instance is decided and remembered. Null
    /// draws every instance at LOD0 — what the path did before selection
    /// existed. The camera it measures from is the one it was given at
    /// LodSelector::BeginFrame. The GPU path hands the cull pass its view, its
    /// bias and each instance's named level, and remembers nothing.
    LodSelector *lodSelector = nullptr;

    /// Which of the mesh pass's pipeline sets draws the list. A DepthPrepass
    /// draws depth alone and leaves the list prepared for MeshPass::Redraw to
    /// shade; it is timed as `depth-prepass` rather than `draw-scene`, so the
    /// lit pass that follows keeps the name it has always been measured under.
    Assisi::Render::MeshPassStage stage = Assisi::Render::MeshPassStage::Lit;
};

/// @brief The GPU timer DrawScene opens around its draws for @p stage.
[[nodiscard]] constexpr const char *DrawSceneTimerName(Assisi::Render::MeshPassStage stage)
{
    return stage == Assisi::Render::MeshPassStage::DepthPrepass ? "depth-prepass" : "draw-scene";
}

/// @brief Extract, sort, and submit a draw list for every Transform+MeshRenderer
///        entity in the scene, through the shared mesh pass.
///
/// The producer half: each entity whose MeshRenderer is resolved is whole-mesh
/// frustum-culled (a cheap sphere reject then an AABB refine, both conservative —
/// nothing visible is ever culled), the submeshes of its selected LOD emitted as
/// one DrawItem each (skipping slots with no resolved material), and — when
/// `sortDraws` is true — the list is sorted by
/// DrawItem::sortKey so MeshPass::Submit records it in material/mesh-major,
/// front-to-back order. `frustumCulling` false submits every mesh; `sortDraws`
/// false submits in query order — both for A/B comparing the seam (the image is
/// identical either way, only the bind counts change).
///
/// @return Drawn/culled counts and the submission's state-change tally.
DrawStats DrawScene(const DrawSceneParams &params);

/// @brief The submesh run @p mesh draws at level @p level.
///
/// No LOD table at all is the whole submesh list — what a factory primitive
/// looks like. A level past the end clamps, so a stale remembered level can
/// never index off the table. Shared by the draw path and the shadow gathers.
[[nodiscard]] Assisi::Geometry::LodRange LodRangeFor(const Assisi::Render::MeshBuffer &mesh, uint32_t level);

} // namespace Assisi::Runtime
