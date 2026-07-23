/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SceneRenderer.hpp
/// @brief Default scene-render path: the "just draw my scene" entry point.
///
/// SceneRenderer bundles the standard opaque-forward setup — clustered lighting
/// (LightingSystem + its froxel grid) and the shared MeshPass pipeline — behind
/// one call, so a game's OnRender is a single line rather than a hand-rolled
/// pipeline dance. It owns the render-side machinery; the game owns the scene
/// and its per-entity meshes/textures.
///
/// Typical lifetime, from an application:
///   Initialize(device, GetSceneFramebufferInfo(), w, h, camera);   // once
///   Resize(w, h, camera);                                          // on window resize
///   OnRenderTargetsChanged(framebufferInfo);                       // on AA/MSAA change
///   Render(frame, scene, cameraTransform, camera);                 // every frame
///
/// The camera is passed as its two components (world transform + projection
/// params) rather than pulled from the scene, so the camera may live in the
/// game scene or a separate one. Projection is derived internally from the
/// camera and the frame's dimensions — callers never build a projection matrix.

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/IconPass.hpp>
#include <Assisi/Render/LinePass.hpp>
#include <Assisi/Render/MeshCuller.hpp>
#include <Assisi/Render/MeshPass.hpp>
#include <Assisi/Render/OutlinePass.hpp>
#include <Assisi/Render/RenderFrame.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/LightingSystem.hpp>
#include <Assisi/Runtime/Renderer.hpp>

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <span>
#include <vector>

namespace Assisi::Runtime
{

class SceneRenderer
{
  public:
    SceneRenderer() = default;

    /// @brief Everything Initialize() needs, gathered so callers name what they
    /// set (the positional list had reached eight parameters).
    struct InitParams
    {
        nvrhi::IDevice        *device = nullptr;
        /// Format/sample-count the mesh pipeline targets
        /// (e.g. Application::GetSceneFramebufferInfo()).
        nvrhi::FramebufferInfo framebufferInfo;
        /// Viewport size in pixels, for the initial cluster grid.
        int32_t                width  = 0;
        int32_t                height = 0;
        /// Projection params (near/far/FOV) of the active camera.
        Camera                 camera;
        /// The scene AssetCache's bindless material-texture table + layout,
        /// threaded into the mesh pipeline (stage D). Must outlive the renderer.
        nvrhi::IBindingLayout   *bindlessLayout = nullptr;
        nvrhi::IDescriptorTable *bindlessTable  = nullptr;
        nvrhi::IBuffer          *materialTable  = nullptr;
        /// Editor overlay passes: selection outline, entity icons, overlay
        /// lines. Off by default — they cost three extra pipelines and load
        /// editor-only assets (assets/editor/**), and a game has no use for
        /// them; the editor opts in. When off, the passes are never built and
        /// the overlay entry points (SetHighlightedEntity, SubmitOverlayLines,
        /// SubmitOutline*, SetEditorIconsVisible, ...) are no-ops.
        bool enableEditorVisuals = false;
    };

    /// @brief Bring up clustered lighting and the scene mesh pipeline against the
    /// given render-target format. Uses the engine's default scene shaders.
    /// @return false if the lighting compute shaders or the mesh pipeline failed
    /// to build. (The editor overlay passes are non-fatal: each failure warns and
    /// that overlay is dropped.)
    [[nodiscard]] bool Initialize(const InitParams &params);

    /// @brief Rebuild the cluster froxel grid for a new viewport/projection.
    /// Call from the application's resize hook. No-op until Initialize() succeeds.
    void Resize(int32_t width, int32_t height, const Camera &camera);

    /// @brief Rebuild just the graphics pipeline after the render-target format
    /// changes (e.g. an MSAA toggle); binding sets and shaders are reused.
    /// @return false if the pipeline failed to rebuild. No-op (true) before Initialize().
    [[nodiscard]] bool OnRenderTargetsChanged(const nvrhi::FramebufferInfo &framebufferInfo);

    /// @brief Draw `scene` from the given camera into `frame`. Propagates the
    /// scene's transforms, refreshes lighting, rebuilds the froxel grid if the
    /// projection changed since last frame, and draws every mesh entity. No-op
    /// until Initialize() succeeds.
    void Render(const Render::RenderFrame &frame, ECS::Scene &scene, const Transform &cameraTransform,
                const Camera &camera)
    {
        Render(frame, scene, cameraTransform, camera, _lastPropagationTick);
    }

    /// @brief As above, with the transform-propagation bookmark supplied by the
    /// caller instead of kept in the renderer.
    ///
    /// The bookmark is the scene tick at the end of that scene's last
    /// propagation, and it is only meaningful *per scene*: one renderer drawing
    /// two scenes with a single bookmark would compare scene B's tick against
    /// scene A's and skip propagating whichever it drew second. An app holding
    /// several worlds resident stores the bookmark in the world
    /// (App::World::propagationTick) and passes it here; the overload above
    /// keeps the renderer's own for the single-scene case.
    void Render(const Render::RenderFrame &frame, ECS::Scene &scene, const Transform &cameraTransform,
                const Camera &camera, uint64_t &propagationTick);

    [[nodiscard]] bool IsValid() const { return _meshPass.IsValid(); }

    /// @brief The underlying mesh pipeline, for games that need direct draw control
    /// beyond the default Render() path.
    [[nodiscard]] Render::MeshPass &MeshPass() { return _meshPass; }

    /// @brief Evict the mesh pass's cached material binding sets. Call after the
    /// asset set backing the scene's materials changes (level unload / asset-cache
    /// clear) so freed GPU resources aren't reused. No-op before Initialize().
    void InvalidateAssetBindings() { _meshPass.InvalidateBindingSets(); }

    /// @brief Enable/disable view-frustum culling in the default draw path (on by
    /// default). Turning it off submits every mesh, for A/B comparing the cull.
    void SetFrustumCulling(bool enabled) { _frustumCulling = enabled; }
    [[nodiscard]] bool FrustumCulling() const { return _frustumCulling; }

    /// @brief Enable/disable draw-list sorting by sort key (on by default). Off
    /// submits in query order — the image is identical, but LastDrawStats().batches
    /// climbs (identical meshes no longer land adjacent to coalesce), which is how
    /// the sort's instancing payoff is measured.
    void SetSortDraws(bool enabled) { _sortDraws = enabled; }
    [[nodiscard]] bool SortDraws() const { return _sortDraws; }

    /// @brief Take the GPU-driven cull path (stage F1) instead of the CPU
    /// extract/sort path (off by default). A compute pass frustum-culls every
    /// object and builds the indirect draw commands on the GPU; the CPU issues one
    /// drawIndexedIndirectCount. An A/B toggle against the CPU path — the opaque
    /// image is identical. No-op if the culler failed to initialize (falls back to
    /// the CPU path). `SortDraws` doesn't affect this path; `FrustumCulling` gates
    /// the GPU frustum test.
    void SetGpuCulling(bool enabled) { _gpuCulling = enabled; }
    [[nodiscard]] bool GpuCulling() const { return _gpuCulling; }

    /// @brief Select a material-channel debug view (None = normal lit render).
    /// The mesh pass short-circuits its shader to that channel — for inspecting
    /// base colour / metallic / roughness / normal / occlusion / emissive.
    void SetDebugView(Render::MaterialDebugView view) { _debugView = view; }
    [[nodiscard]] Render::MaterialDebugView DebugView() const { return _debugView; }

    /// @brief Drawn/culled counts from the most recent Render(); zero before the
    /// first frame. Reflects whether culling is actually removing anything.
    [[nodiscard]] DrawStats LastDrawStats() const { return _lastDrawStats; }

    /// @brief Mark one entity to receive an always-on-top orange silhouette
    /// outline (a selection highlight). Pass ECS::NullEntity to clear it. The
    /// entity must carry a Transform and a MeshRenderer with a resolved mesh, or
    /// the outline is silently skipped. Drawn each Render() after the scene.
    /// No-op unless InitParams::enableEditorVisuals was set.
    void SetHighlightedEntity(ECS::Entity entity)
    {
        _highlightedEntity = _editorVisuals ? entity : ECS::NullEntity;
    }
    [[nodiscard]] ECS::Entity HighlightedEntity() const { return _highlightedEntity; }

    /// @brief Show/hide the editor's entity icons — world-space billboards marking
    /// entities that have a Transform but no mesh. Off by default (games don't want
    /// them); an editor turns them on while authoring and off during play. Drawn
    /// each Render() after the scene when enabled. No-op unless
    /// InitParams::enableEditorVisuals was set.
    void SetEditorIconsVisible(bool visible) { _editorIconsVisible = _editorVisuals && visible; }
    [[nodiscard]] bool EditorIconsVisible() const { return _editorIconsVisible; }

    /// @brief Queue a batch of coloured world-space line segments (a LineList —
    /// consecutive vertex pairs) to draw over the scene this frame. @p onTop routes
    /// them to the x-ray pipeline (visible through geometry); otherwise they are
    /// depth-tested. Queued lines are drawn at the end of the next Render() and then
    /// cleared, so callers re-submit each frame. This is a generic overlay facility
    /// (the editor draws collider wireframes with it); it carries no scene meaning.
    void SubmitOverlayLines(std::span<const Render::LineVertex> vertices, bool onTop);

    /// @brief Suppress the editor billboard/icon for these entities for the next
    /// Render() (something else already marks them in the world — e.g. an editor
    /// collider wireframe). The list is consumed and cleared each Render(), so the
    /// caller re-supplies it every frame; pass an empty span (or don't call) to
    /// suppress nothing.
    void SetIconSuppressedEntities(std::span<const ECS::Entity> entities);

    /// @brief Queue one independent silhouette outline for the next Render(): the
    /// @p items' silhouettes union into a SINGLE border of @p color (e.g. a capsule
    /// collider is a cylinder + two spheres that must merge). Each call is its own
    /// edge-detect pass, so separate groups never combine — a collider and the mesh
    /// it wraps outline independently. Drawn on top of the scene (a selection
    /// highlight). Cleared each Render(), so re-submit every frame. Editor overlay;
    /// carries no scene meaning.
    void SubmitOutlineGroup(std::span<const Render::OutlinePass::OutlineItem> items, const glm::vec3 &color);

    /// @brief Convenience for a single-mesh outline group. See SubmitOutlineGroup.
    void SubmitOutline(const Render::MeshBuffer *mesh, const glm::mat4 &model, const glm::vec3 &color);

  private:
    /// @brief Rebuild the froxel grid on its own command list (setup/resize path).
    void RebuildClusterGrid(int32_t width, int32_t height, const Camera &camera, const glm::mat4 &projection);

    /// @brief Draw the selection outline for _highlightedEntity (if any) as an
    /// always-on-top overlay after the scene: a mesh silhouette, or — for a
    /// placement-only entity shown as an icon — its billboard quad (editor only).
    /// No-op when nothing is highlighted or the outline pass is unavailable.
    void DrawHighlightOutline(const Render::RenderFrame &frame, const glm::mat4 &viewProjection,
                              const glm::mat4 &view, ECS::Scene &scene);

    /// @brief Draw a world-space billboard for every entity with a Transform but no
    /// MeshRenderer, using the camera basis from @p view to face them. Icons beyond
    /// a fixed distance from @p cameraPosition are skipped (a simple render/don't
    /// LOD). No-op when icons are hidden or the icon pass is unavailable.
    void DrawEditorIcons(const Render::RenderFrame &frame, const glm::mat4 &viewProjection, const glm::mat4 &view,
                         const glm::vec3 &cameraPosition, ECS::Scene &scene);

    /// @brief Whether @p entity's editor icon is suppressed this frame (see
    /// SetIconSuppressedEntities) — it is marked in the world some other way.
    [[nodiscard]] bool IsIconSuppressed(ECS::Entity entity) const;

    nvrhi::IDevice     *_device = nullptr;
    LightingSystem      _lighting;
    Render::MeshPass    _meshPass;
    // GPU-driven cull (stage F1): the compute cull pass + the reused host-side
    // table builder it uploads from. Initialized alongside the mesh pass; the draw
    // path uses them only when _gpuCulling is on (else the CPU path runs).
    Render::MeshCuller       _meshCuller;
    Render::CullTableBuilder _cullBuilder;
    Render::OutlinePass _outlinePass;
    Render::IconPass    _iconPass;
    Render::LinePass    _linePass;

    // The entity drawn with a selection outline this frame (NullEntity = none).
    ECS::Entity _highlightedEntity = ECS::NullEntity;

    // Editor overlay passes were requested at Initialize (InitParams::
    // enableEditorVisuals). When false the passes were never built and every
    // overlay entry point no-ops.
    bool _editorVisuals = false;

    bool _editorIconsVisible = false; // editor opts in; off during play and for games
    // Reused scratch for the per-frame icon positions, so drawing icons doesn't
    // allocate every frame.
    std::vector<glm::vec3> _iconPositions;

    // Overlay line segments queued this frame, split by depth mode; drawn at the
    // end of Render() and cleared. Refilled each frame by the caller.
    std::vector<Render::LineVertex> _overlayLinesDepthTested;
    std::vector<Render::LineVertex> _overlayLinesOnTop;

    // Independent silhouette-outline groups queued this frame; each is drawn as its
    // own edge-detect pass (so a collider and its mesh never merge), then cleared.
    struct OutlineGroup
    {
        std::vector<Render::OutlinePass::OutlineItem> items;
        glm::vec3                                     color;
    };
    std::vector<OutlineGroup> _outlineGroups;
    // Entities whose editor icon is suppressed this frame (drawn some other way).
    // Consumed and cleared each Render().
    std::vector<ECS::Entity> _iconSuppressed;

    // Projection the froxel grid was last built against; a mismatch in Render()
    // triggers a rebuild. Identity forces one on the first frame.
    glm::mat4 _clusterProjection{1.f};

    bool      _frustumCulling = true; // default draw path culls off-screen meshes
    bool      _sortDraws      = true; // default draw path sorts by sort key before submit
    bool      _gpuCulling     = false; // GPU-driven cull path (stage F1); CPU path is the default reference
    Render::MaterialDebugView _debugView = Render::MaterialDebugView::None; // material-channel debug visualization
    DrawStats _lastDrawStats;         // drawn/culled from the last Render(), for the overlay

    // Change-detection bookmark for PropagateTransforms used by the single-scene
    // Render() overload: the scene tick at the end of the last propagation. 0
    // forces a full recompute on the first frame. Apps with several worlds
    // resident pass their own per-world bookmark instead.
    uint64_t _lastPropagationTick = 0;
};

} // namespace Assisi::Runtime
