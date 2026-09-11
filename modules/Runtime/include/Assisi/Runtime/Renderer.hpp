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

/// @brief The identity the shadow atlas's cache remembers a caster by.
///
/// The entity's index and generation together, so a slot reused by a new entity
/// is a different caster rather than the old one having teleported. The cache
/// has to recognise a caster across frames in which it was not gathered at all,
/// which is why this is a durable handle rather than a position in a span.
[[nodiscard]] constexpr std::uint64_t ShadowCasterId(Assisi::ECS::Entity entity)
{
    return (static_cast<std::uint64_t>(entity.generation) << 32) | entity.index;
}

/// @brief One frame's shadow casters, and how far up-light they reach.
///
/// Reused across frames by the caller — the vector's capacity survives a
/// re-gather, so a steady-state scene allocates nothing here.
struct ShadowCasterGather
{
    /// Sorted opaque-first and by geometry key within each half, which is what
    /// lets consecutive entries coalesce into one instanced draw in the shadow
    /// pass. The alpha-tested half sorts last because it draws through its own
    /// pipeline, and a run that straddled the two could not coalesce anyway.
    std::vector<Assisi::Render::ShadowCaster> casters;

    /// The smallest `dot(p, lightDirection)` any caster's bounding sphere
    /// reaches — the point up-light past which nothing can cast into the view.
    /// Absent when nothing casts. Measured over the casters that survived the
    /// distance cull, since the ones that did not are no longer drawn.
    std::optional<float> nearAlongLight;

    /// Entities dropped for reaching no view's volume at all. Reported so the
    /// shadow distance's effect is a number rather than an impression: walking
    /// content past it should move this and nothing else.
    std::uint32_t culledEntities = 0;

    /// Entity-view pairs drawn one level coarser than the camera draws that
    /// entity. Zero with shadow LOD off, and in a scene with no LOD chains.
    std::uint32_t coarserViews = 0;
};

/// @brief Collect every shadow-casting submesh in the scene, for the sun.
///
/// A caster is any resolved mesh whose MeshRenderer has `castsShadows` set.
/// Each submesh's material is consulted for one thing only: whether it
/// alpha-tests, and which table row the test reads. A submesh with no material
/// resolved still casts, opaquely — a shadow is a property of the geometry, and
/// dropping it would be a worse answer than a solid one.
///
/// Nothing is frustum-culled here — that is per view, and the frustum is the
/// view's own. What happens here is the classification every view would
/// otherwise repeat: @p viewVolumes is one sphere per view, containing that
/// view's whole ortho box, and a caster's sweep down-light against each of them
/// is the mask the draw list walks. A caster that reaches none of them belongs
/// in no list at all, and is dropped rather than gathered.
///
/// That is where the sun's shadow distance is spent — the fit clamps its own far
/// plane to it, and without this the gather still walked, sorted and per-view
/// tested a caster kilometres past the last cascade. An empty span, or volumes
/// of radius zero, is the unfitted case and gathers nothing.
///
/// Each caster's geometry is resolved to its arena offsets here rather than at
/// draw time: a caster is drawn once per view it survives into, and resolving
/// per view would repeat the lookup for every one of them.
///
/// Each view draws a caster at the level its own texels ask for, held to the
/// camera's level or one coarser (see LodSelector::CoarserShadowViews, whose
/// views must be these, in this order): an entity the views disagree about is
/// emitted once per level, each copy masked to the views that draw it. In a far
/// cascade a coarse silhouette differs from the one on screen by less than a
/// texel, and the depth pass pays for vertices once per view. Null selects LOD0
/// for everything.
///
/// @p out is cleared and refilled; pass the same object every frame.
void GatherShadowCasters(Assisi::ECS::Scene &scene, const glm::vec3 &lightDirection,
                         std::span<const Assisi::Geometry::BoundingSphere> viewVolumes, LodSelector *lodSelector,
                         ShadowCasterGather &out);

/// @brief One frame's local-light shadow casters, the lights each one reaches,
/// and the per-light rows the atlas reads.
///
/// Owns its buffers and is refilled rather than rebuilt: a scene changes little
/// between frames, so a steady state reuses the capacity instead of freeing and
/// regrowing it.
class LocalShadowCasterGather
{
public:
    /// @brief Every shadow-casting submesh in the scene, and which of
    /// @p lightVolumes each one reaches. A caster reaching none is dropped.
    ///
    /// A plain sphere-sphere test, and no sweep: a local light is a point with a
    /// range, so what can occlude for it is what stands inside it. The sun's
    /// gather sweeps down-light because the sun has no position to be inside of.
    ///
    /// @p mobility decides which half of a cached tile each caster belongs to,
    /// and is told where every still caster stands — the pose a kept layer holds
    /// it at, and what a later demotion has to invalidate.
    /// @p lodSelector picks each caster's level, as the sun's gather and the draw
    /// path do; null gathers everything at LOD0.
    void Gather(Assisi::ECS::Scene &scene, std::span<const Assisi::Geometry::BoundingSphere> lightVolumes,
                Assisi::Render::ShadowCasterMobility &mobility, LodSelector *lodSelector);

    /// @brief Invert the membership into the per-light rows, and sort the casters
    /// those rows name.
    ///
    /// The sort lives here because the rows name casters by position in the
    /// sorted span; splitting the two would mean carrying a permutation between
    /// them. Opaque-first and geometry-major, so consecutive entries coalesce.
    void BuildIndex();

    /// @brief No casters, and an empty row for each of @p lightCount lights.
    ///
    /// A frame that skips the gather still needs every row: the composite walks
    /// one per served face whether or not it finds anything in it.
    void Reset(std::uint32_t lightCount);

    /// In gather order until BuildIndex sorts them.
    [[nodiscard]] std::span<const Assisi::Render::ShadowCaster> Casters() const { return _casters; }
    [[nodiscard]] const Assisi::Render::LocalShadowCasterIndex &Index() const { return _index; }
    [[nodiscard]] std::uint32_t CulledEntities() const { return _culledEntities; }

private:
    std::vector<Assisi::Render::ShadowCaster> _casters;

    // Light indices, concatenated: `_casterStart[i]` to `_casterStart[i + 1]` is
    // caster i's row. `_index`'s rows are this, inverted.
    std::vector<std::uint32_t> _reachedLights;
    std::vector<std::uint32_t> _casterStart;

    Assisi::Render::LocalShadowCasterIndex _index;

    // Scratch, kept between frames for the capacity.
    std::vector<std::uint32_t> _order;
    std::vector<std::uint32_t> _cursor;
    std::vector<Assisi::Render::ShadowCaster> _sorted;

    std::uint32_t _lightCount = 0;
    std::uint32_t _culledEntities = 0;
};

/// @brief The shadow casters among @p changed, with where they now stand.
///
/// The invalidation input, and the reason it is cheap: @p changed comes from the
/// Transform pool's change-tick lane, so it names what moved rather than what
/// exists, and a frame in which nothing moved produces nothing here and skips
/// the caster gather entirely.
void GatherShadowMovers(Assisi::ECS::Scene &scene, std::span<const Assisi::ECS::Entity> changed,
                        std::vector<Assisi::Render::ShadowMover> &out);

} // namespace Assisi::Runtime
