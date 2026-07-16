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
#include <Assisi/Render/MeshPass.hpp>
#include <Assisi/Render/OutlinePass.hpp>
#include <Assisi/Render/RenderFrame.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/LightingSystem.hpp>
#include <Assisi/Runtime/Renderer.hpp>

#include <nvrhi/nvrhi.h>

namespace Assisi::Runtime
{

class SceneRenderer
{
  public:
    SceneRenderer() = default;

    /// @brief Bring up clustered lighting and the scene mesh pipeline against the
    /// given render-target format. Uses the engine's default scene shaders.
    /// @param framebufferInfo  Format/sample-count the mesh pipeline targets
    ///        (e.g. Application::GetSceneFramebufferInfo()).
    /// @param width,height     Viewport size in pixels, for the initial cluster grid.
    /// @param camera           Projection params (near/far/FOV) of the active camera.
    /// @param bindlessLayout / bindlessTable  The scene AssetCache's bindless
    ///        material-texture table + layout, threaded into the mesh pipeline
    ///        (stage D). Must outlive the renderer.
    /// @return false if the lighting compute shaders or the mesh pipeline failed to build.
    [[nodiscard]] bool Initialize(nvrhi::IDevice *device, const nvrhi::FramebufferInfo &framebufferInfo, int width,
                                  int height, const Camera &camera, nvrhi::IBindingLayout *bindlessLayout,
                                  nvrhi::IDescriptorTable *bindlessTable, nvrhi::IBuffer *materialTable);

    /// @brief Rebuild the cluster froxel grid for a new viewport/projection.
    /// Call from the application's resize hook. No-op until Initialize() succeeds.
    void Resize(int width, int height, const Camera &camera);

    /// @brief Rebuild just the graphics pipeline after the render-target format
    /// changes (e.g. an MSAA toggle); binding sets and shaders are reused.
    /// @return false if the pipeline failed to rebuild. No-op (true) before Initialize().
    [[nodiscard]] bool OnRenderTargetsChanged(const nvrhi::FramebufferInfo &framebufferInfo);

    /// @brief Draw `scene` from the given camera into `frame`. Propagates the
    /// scene's transforms, refreshes lighting, rebuilds the froxel grid if the
    /// projection changed since last frame, and draws every mesh entity. No-op
    /// until Initialize() succeeds.
    void Render(const Render::RenderFrame &frame, ECS::Scene &scene, const Transform &cameraTransform,
                const Camera &camera);

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
    void SetHighlightedEntity(ECS::Entity entity) { _highlightedEntity = entity; }
    [[nodiscard]] ECS::Entity HighlightedEntity() const { return _highlightedEntity; }

    /// @brief Show/hide the editor's entity icons — world-space billboards marking
    /// entities that have a Transform but no mesh. Off by default (games don't want
    /// them); an editor turns them on while authoring and off during play. Drawn
    /// each Render() after the scene when enabled.
    void SetEditorIconsVisible(bool visible) { _editorIconsVisible = visible; }
    [[nodiscard]] bool EditorIconsVisible() const { return _editorIconsVisible; }

  private:
    /// @brief Rebuild the froxel grid on its own command list (setup/resize path).
    void RebuildClusterGrid(int width, int height, const Camera &camera, const glm::mat4 &projection);

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

    nvrhi::IDevice     *_device = nullptr;
    LightingSystem      _lighting;
    Render::MeshPass    _meshPass;
    Render::OutlinePass _outlinePass;
    Render::IconPass    _iconPass;

    // The entity drawn with a selection outline this frame (NullEntity = none).
    ECS::Entity _highlightedEntity = ECS::NullEntity;

    bool _editorIconsVisible = false; // editor opts in; off during play and for games
    // Reused scratch for the per-frame icon positions, so drawing icons doesn't
    // allocate every frame.
    std::vector<glm::vec3> _iconPositions;

    // Projection the froxel grid was last built against; a mismatch in Render()
    // triggers a rebuild. Identity forces one on the first frame.
    glm::mat4 _clusterProjection{1.f};

    bool      _frustumCulling = true; // default draw path culls off-screen meshes
    bool      _sortDraws      = true; // default draw path sorts by sort key before submit
    Render::MaterialDebugView _debugView = Render::MaterialDebugView::None; // material-channel debug visualization
    DrawStats _lastDrawStats;         // drawn/culled from the last Render(), for the overlay

    // Change-detection bookmark for PropagateTransforms: the scene tick at the end
    // of the last propagation. 0 forces a full recompute on the first frame.
    uint64_t _lastPropagationTick = 0;
};

} // namespace Assisi::Runtime
