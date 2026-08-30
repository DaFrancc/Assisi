/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Runtime/SceneRenderer.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>

#include <Assisi/Chiara/Profile.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/GpuMarker.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/Renderer.hpp>

namespace Assisi::Runtime
{
namespace
{
// Engine default scene shaders (opaque lit geometry, clustered forward). Compiled
// under the asset root by the build; resolved through Core::AssetSystem.
constexpr const char *kSceneVertexShader = "shaders/mesh.vert.spv";
constexpr const char *kScenePixelShader = "shaders/mesh.frag.spv";
// The same pixel shader built with its alpha-test discard enabled, for the mesh
// pass's masked pipeline (see MeshPass::InitParams).
constexpr const char *kSceneMaskedPixelShader = "shaders/mesh.frag.masked.spv";

// The sun's cascade depth pass — a vertex stage and nothing else, since the
// pipeline writes depth and no colour (see Render::ShadowPass).
constexpr const char *kShadowVertexShader = "shaders/shadow_depth.vert.spv";
// The alpha-tested caster variant: the same vertex stage built to carry the UV
// and material row, and the fragment stage that discards on them, so a cutout
// material casts a shadow with its hole in it.
constexpr const char *kShadowMaskedVertexShader = "shaders/shadow_depth.vert.masked.spv";
constexpr const char *kShadowMaskedPixelShader = "shaders/shadow_depth.frag.spv";

// Selection-outline shaders (screen-space edge detect; see Render::OutlinePass):
// a mask pass that stamps the silhouette, and a fullscreen edge pass that paints
// the orange border. Editor-only, so they live under editor/shaders/ — except the
// edge pass's vertex stage, which reuses the shared fullscreen-triangle shader.
constexpr const char *kOutlineMaskVertexShader = "editor/shaders/outline_mask.vert.spv";
constexpr const char *kOutlineMaskPixelShader  = "editor/shaders/outline_mask.frag.spv";
constexpr const char *kOutlineEdgeVertexShader = "shaders/fullscreen.vert.spv";
constexpr const char *kOutlineEdgePixelShader  = "editor/shaders/outline_edge.frag.spv";

// Generic overlay-line renderer (see Render::LinePass). Editor-only in practice
// (collider wireframes), so the shaders live under editor/shaders/.
constexpr const char *kLineVertexShader = "editor/shaders/line.vert.spv";
constexpr const char *kLinePixelShader  = "editor/shaders/line.frag.spv";

// Editor entity-icon billboard (see Render::IconPass), editor-only. The icon image
// is authored content dropped at this virtual path; until it exists the pass shows
// a magenta placeholder.
constexpr const char *kIconVertexShader = "editor/shaders/icon_billboard.vert.spv";
constexpr const char *kIconPixelShader  = "editor/shaders/icon_billboard.frag.spv";
constexpr const char *kEntityIconTexture = "editor/entity_icon.png";
// Outline mask for a selected icon: samples the icon so the border traces its
// artwork. Reuses the icon billboard vertex stage (kIconVertexShader).
constexpr const char *kIconMaskPixelShader = "editor/shaders/icon_mask.frag.spv";

// Entity icons past this distance from the camera are not drawn — a simple
// render/don't LOD so a large scene isn't peppered with distant icons.
constexpr float kMaxIconDistance = 100.f;


float AspectRatio(int32_t width, int32_t height)
{
    return height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.f;
}
} // namespace

bool SceneRenderer::Initialize(const InitParams &params)
{
    _device        = params.device;
    _editorVisuals = params.enableEditorVisuals;

    const glm::mat4 projection = ProjectionMatrix(params.camera, AspectRatio(params.width, params.height));

    // The cluster grid's light buffers must exist before MeshPass::Initialize,
    // which binds them into every binding set it creates — so build lighting first.
    nvrhi::CommandListHandle setupCommandList = _device->createCommandList();
    setupCommandList->open();
    const bool lightingOk = _lighting.Initialize(_device, setupCommandList, params.width, params.height,
                                                 params.camera.nearZ, params.camera.farZ, projection);
    setupCommandList->close();
    _device->executeCommandList(setupCommandList);

    if (!lightingOk)
    {
        Core::Log::Error("SceneRenderer: failed to initialise the clustered lighting pipeline.");
        return false;
    }
    _clusterProjection = projection;

    // Before the mesh pass: it binds the cascade array into every binding set it
    // builds, and the pass keeps a placeholder there until a sun turns up. A
    // failure is non-fatal — the scene renders unshadowed rather than not at all.
    if (!_shadowDepthRenderer.Initialize(
            Render::ShadowDepthRenderer::InitParams{.device = _device,
                                                    .vertexShaderSpvPath = kShadowVertexShader,
                                                    .maskedVertexShaderSpvPath = kShadowMaskedVertexShader,
                                                    .maskedPixelShaderSpvPath = kShadowMaskedPixelShader,
                                                    .materialTable = params.materialTable,
                                                    .bindlessLayout = params.bindlessLayout,
                                                    .bindlessTable = params.bindlessTable}) ||
        !_shadowPass.Initialize(
            Render::ShadowPass::InitParams{.device = _device, .depthRenderer = &_shadowDepthRenderer}))
    {
        Core::Log::Warn("SceneRenderer: sun shadows unavailable (the depth pass failed to initialise).");
    }

    if (!_meshPass.Initialize(Render::MeshPass::InitParams{.device = _device,
                                                           .framebufferInfo = params.framebufferInfo,
                                                           .vertexShaderSpvPath = kSceneVertexShader,
                                                           .pixelShaderSpvPath = kScenePixelShader,
                                                           .maskedPixelShaderSpvPath = kSceneMaskedPixelShader,
                                                           .clusterGrid = &_lighting.Grid(),
                                                           .bindlessLayout = params.bindlessLayout,
                                                           .bindlessTable = params.bindlessTable,
                                                           .materialTable = params.materialTable}))
    {
        Core::Log::Error("SceneRenderer: failed to initialise the scene mesh pass.");
        return false;
    }
    _meshPass.SetShadowMap(_shadowPass.CascadeTexture());

    // GPU-driven cull (stage F1). Non-fatal: if the compute pipeline fails to
    // build, the "GPU Cull" toggle stays a no-op and the CPU draw path runs.
    if (!_meshCuller.Initialize(_device))
    {
        Core::Log::Warn("SceneRenderer: GPU cull unavailable (mesh_cull compute pipeline failed to build).");
    }

    // Editor overlay passes (selection outline, entity icons, overlay lines).
    // Opt-in: a game never builds these pipelines or touches assets/editor/**;
    // the editor asks for them. Failures inside the opted-in path stay
    // non-fatal — each overlay warns and is dropped, never the renderer.
    //
    // They target the overlay framebuffer, not the scene's: overlay colours are
    // display values and are drawn after the tone map.
    if (_editorVisuals)
    {
        if (!_outlinePass.Initialize(_device, params.overlayFramebufferInfo, static_cast<uint32_t>(params.width),
                                     static_cast<uint32_t>(params.height), kOutlineMaskVertexShader,
                                     kOutlineMaskPixelShader, kOutlineEdgeVertexShader, kOutlineEdgePixelShader,
                                     kIconVertexShader, kIconMaskPixelShader))
        {
            Core::Log::Warn("SceneRenderer: selection outline unavailable (outline pass failed to initialise).");
        }

        if (!_iconPass.Initialize(_device, params.overlayFramebufferInfo, kIconVertexShader, kIconPixelShader,
                                  kEntityIconTexture))
        {
            Core::Log::Warn("SceneRenderer: entity icons unavailable (icon pass failed to initialise).");
        }

        if (!_linePass.Initialize(_device, params.overlayFramebufferInfo, kLineVertexShader, kLinePixelShader))
        {
            Core::Log::Warn("SceneRenderer: overlay lines unavailable (line pass failed to initialise).");
        }
    }

    return true;
}

void SceneRenderer::RebuildClusterGrid(int32_t width, int32_t height, const Camera &camera,
                                       const glm::mat4 &projection)
{
    if (_device == nullptr || !_meshPass.IsValid())
    {
        return;
    }

    nvrhi::CommandListHandle commandList = _device->createCommandList();
    commandList->open();
    _lighting.Resize(commandList, width, height, camera.nearZ, camera.farZ, projection);
    commandList->close();
    _device->executeCommandList(commandList);

    _clusterProjection = projection;
}

void SceneRenderer::Resize(int32_t width, int32_t height, const Camera &camera)
{
    RebuildClusterGrid(width, height, camera, ProjectionMatrix(camera, AspectRatio(width, height)));
}

bool SceneRenderer::OnRenderTargetsChanged(const nvrhi::FramebufferInfo &framebufferInfo,
                                           const nvrhi::FramebufferInfo &overlayFramebufferInfo)
{
    if (!_meshPass.IsValid())
    {
        return true; // nothing built yet — nothing to rebuild
    }
    // Rebuild the overlay pipelines against the new format too; a failure there
    // only drops the overlay, so it doesn't fail the render-target change.
    if (!_outlinePass.RebuildPipeline(overlayFramebufferInfo))
    {
        Core::Log::Warn("SceneRenderer: selection outline pipeline rebuild failed; highlight disabled.");
    }
    if (!_iconPass.RebuildPipeline(overlayFramebufferInfo))
    {
        Core::Log::Warn("SceneRenderer: entity-icon pipeline rebuild failed; icons disabled.");
    }
    if (!_linePass.RebuildPipeline(overlayFramebufferInfo))
    {
        Core::Log::Warn("SceneRenderer: overlay-line pipeline rebuild failed; collider wireframes disabled.");
    }
    return _meshPass.RebuildPipeline(framebufferInfo);
}

void SceneRenderer::Render(const Render::RenderFrame &frame, ECS::Scene &scene,
                           const Transform &cameraTransform, const Camera &camera, uint64_t &propagationTick)
{
    if (!_meshPass.IsValid())
    {
        return;
    }

    // Refresh world matrices before anything reads them (view matrix, draw). Only
    // entities whose transform changed since last frame are recomputed; the tick
    // bookmark carries that across frames — and belongs to the scene, not to us
    // (see the header).
    {
        ASSISI_PROFILE_SCOPE("propagate-transforms");
        propagationTick = PropagateTransforms(scene, propagationTick);
    }

    const glm::mat4 projection = ProjectionMatrix(camera, AspectRatio(static_cast<int32_t>(frame.width),
                                                                      static_cast<int32_t>(frame.height)));
    const glm::mat4 view = ViewMatrix(cameraTransform);

    // Keep the froxel grid aligned with the render projection; a drift (window
    // resize, runtime FOV/near/far edit) makes peripheral froxels stop matching
    // it and shows rectangular lighting artifacts.
    if (projection != _clusterProjection)
    {
        // Scoped even though it is rare: it is a full grid rebuild, so the one
        // frame that pays it should say so rather than look like a random spike.
        ASSISI_PROFILE_GPU_PASS(frame.commandList, "cluster-rebuild");
        RebuildClusterGrid(static_cast<int32_t>(frame.width), static_cast<int32_t>(frame.height), camera, projection);
    }

    _lighting.Update(frame.commandList, scene, view);

    const Render::MeshPass::ShadowFrameData shadows = RenderSunShadows(frame, scene, camera, view);

    {
        ASSISI_PROFILE_GPU_SCOPE(frame.commandList, "mesh-constants");
        const Render::MeshPass::FrameConstantsParams frameConstants{
            .viewProjection   = projection * view,
            .view             = view,
            .screenWidth      = frame.width,
            .screenHeight     = frame.height,
            .nearZ            = camera.nearZ,
            .farZ             = camera.farZ,
            .dirLightCount    = _lighting.DirLightCount(),
            .debugView        = _debugView,
            .ambientColor     = _ambientColor,
            .ambientIntensity = _ambientIntensity,
            .shadows          = shadows};
        _meshPass.UpdateFrameConstants(frame.commandList, frameConstants);
    }
    _lastDrawStats = DrawScene(DrawSceneParams{.scene          = scene,
                                               .meshPass       = _meshPass,
                                               .frame          = frame,
                                               .view           = view,
                                               .projection     = projection,
                                               .nearZ          = camera.nearZ,
                                               .farZ           = camera.farZ,
                                               .frustumCulling = _frustumCulling,
                                               .sortDraws      = _sortDraws,
                                               .gpuCulling     = _gpuCulling,
                                               .culler         = &_meshCuller,
                                               .cullBuilder    = &_cullBuilder});

    // What the frame actually drew, on their own tracks. These are the numbers you
    // reach for the moment `draw-scene` moves: a jump in batches or draw calls says
    // the scene grew, a jump with flat counts says the cost is elsewhere.
    ASSISI_PROFILE_COUNTER("render/draw-calls", static_cast<double>(_lastDrawStats.drawCalls));
    ASSISI_PROFILE_COUNTER("render/batches", static_cast<double>(_lastDrawStats.batches));
    ASSISI_PROFILE_COUNTER("render/drawn-items", static_cast<double>(_lastDrawStats.drawnItems));
    ASSISI_PROFILE_COUNTER("render/culled-meshes", static_cast<double>(_lastDrawStats.culledMeshes));

    // The shadow pass on its own tracks, for the same reason: a jump in
    // `shadow-cascades` is explained by one of these or by none of them, and
    // "none of them" is the interesting answer.
    ASSISI_PROFILE_COUNTER("shadows/cascades", static_cast<double>(_lastShadowStats.cascades));
    ASSISI_PROFILE_COUNTER("shadows/instances", static_cast<double>(_lastShadowStats.instances));
    ASSISI_PROFILE_COUNTER("shadows/batches", static_cast<double>(_lastShadowStats.batches));
    // Reads zero for a scene with no cutout caster in it, which is what turns
    // "the alpha-tested variant costs nothing here" into something visible.
    ASSISI_PROFILE_COUNTER("shadows/masked-batches", static_cast<double>(_lastShadowStats.maskedBatches));
    ASSISI_PROFILE_COUNTER("shadows/culled", static_cast<double>(_lastShadowStats.culled));
    // Casters the gather never handed to a view at all, because nothing they
    // cast can reach the shadow distance. Walking content out past it moves this
    // and leaves every other shadow counter where it was.
    ASSISI_PROFILE_COUNTER("shadows/gather-culled", static_cast<double>(_shadowCasters.culledEntities));
}

Render::MeshPass::ShadowFrameData SceneRenderer::RenderSunShadows(const Render::RenderFrame &frame,
                                                                  ECS::Scene &scene, const Camera &camera,
                                                                  const glm::mat4 &view)
{
    Render::MeshPass::ShadowFrameData shadows;
    _lastShadowStats = Render::ShadowPass::Stats{};
    _cascadeFit = Render::CascadeFit{};

    // No shadow-casting sun means no allocation and no pass — which is the
    // blank-scene rule, and the reason `active` is a scene fact rather than a
    // settings one.
    const std::optional<LightingSystem::ShadowSun> sun = _lighting.ShadowCastingSun();
    const bool active = _shadowSettings.sun.enabled && sun.has_value();
    if (!_shadowPass.Configure(_shadowSettings.sun, active))
    {
        Core::Log::Warn("SceneRenderer: sun shadows disabled (the cascade targets failed to rebuild).");
        _shadowSettings.sun.enabled = false;
    }
    // After Configure: a reallocation swaps the texture handle, and the mesh
    // pass rebuilds its binding set when it notices.
    _meshPass.SetShadowMap(_shadowPass.CascadeTexture());

    if (!_shadowPass.IsActive() || !sun.has_value())
    {
        return shadows;
    }

    // Opens the frame's shadow view table. Every kind of shadow map appends to
    // one table, so this belongs here rather than inside any one of them.
    _shadowDepthRenderer.BeginFrame();

    Render::CascadeFitParams fitParams;
    fitParams.cameraView = view;
    fitParams.tanHalfFovY = std::tan(glm::radians(camera.fovDegrees) * 0.5f);
    fitParams.aspectRatio = AspectRatio(static_cast<int32_t>(frame.width), static_cast<int32_t>(frame.height));
    fitParams.nearZ = camera.nearZ;
    fitParams.farZ = camera.farZ;
    fitParams.lightDirection = sun->direction;
    // The pass's own copy, not ours: Configure sanitized it, and the fit has to
    // agree with the array that was actually allocated.
    fitParams.settings = _shadowPass.Settings();
    _cascadeFit = Render::FitCascades(fitParams);

    // After the fit, because the gather culls against it: the sun's shadow
    // distance lives in the cascades' own extent, and a caster that cannot reach
    // it is one no view wants. The fit reads nothing from the gather, so this is
    // the order it always could have been in.
    GatherShadowCasters(scene, sun->direction, Render::ShadowedVolumeBounds(_cascadeFit), _shadowCasters);

    _lastShadowStats = _shadowPass.Render(frame.commandList, _cascadeFit, _shadowCasters.casters);

    shadows.fit = &_cascadeFit;
    shadows.settings = _shadowPass.Settings();
    shadows.sunLightIndex = sun->index;
    shadows.debugView = _shadowDebugView;
    return shadows;
}

void SceneRenderer::RenderOverlays(const Render::RenderFrame &frame, ECS::Scene &scene,
                                   const Transform &cameraTransform, const Camera &camera)
{
    if (!_meshPass.IsValid())
    {
        return;
    }

    // Recomputed rather than carried over from Render(): both are two matrix
    // builds from state neither of them changes, and a cached pair is one more
    // thing that can go stale between the two calls.
    const glm::mat4 projection = ProjectionMatrix(camera, AspectRatio(static_cast<int32_t>(frame.width),
                                                                      static_cast<int32_t>(frame.height)));
    const glm::mat4 view = ViewMatrix(cameraTransform);
    const glm::mat4 viewProjection = projection * view;

    {
        ASSISI_PROFILE_GPU_PASS(frame.commandList, "editor-icons");
        DrawEditorIcons(frame, viewProjection, view, cameraTransform.position, scene);
    }

    // Submitted silhouette outlines (the selected object's collider + mesh). Each
    // group is its own edge-detect pass, so a collider and the mesh it wraps outline
    // independently rather than merging into one border. Drawn on top of the scene.
    {
        // One scope for both outline sources (submitted groups + the highlight):
        // same pass, same per-group cost, so a split would name the caller rather
        // than the cost.
        ASSISI_PROFILE_GPU_PASS(frame.commandList, "outlines");
        if (_outlinePass.IsValid())
        {
            for (const OutlineGroup &group : _outlineGroups)
            {
                _outlinePass.DrawOutlines(frame, viewProjection, group.items, group.color);
            }
        }
        _outlineGroups.clear();

        DrawHighlightOutline(frame, viewProjection, view, scene);
    }

    // Overlay lines (collider wireframes) sit on top of everything else: the
    // depth-tested batch first (occluded by the scene), then the on-top batch
    // (x-ray). Both are cleared afterwards so the caller re-submits each frame.
    {
        ASSISI_PROFILE_GPU_PASS(frame.commandList, "overlay-lines");
        if (_linePass.IsValid())
        {
            _linePass.Draw(frame, viewProjection, _overlayLinesDepthTested, /*onTop=*/ false);
            _linePass.Draw(frame, viewProjection, _overlayLinesOnTop, /*onTop=*/ true);
        }
        _overlayLinesDepthTested.clear();
        _overlayLinesOnTop.clear();
    }
    _iconSuppressed.clear();
}

void SceneRenderer::SubmitOverlayLines(std::span<const Render::LineVertex> vertices, bool onTop)
{
    if (!_editorVisuals)
    {
        return; // overlay passes were never built (InitParams::enableEditorVisuals off)
    }
    std::vector<Render::LineVertex> &sink = onTop ? _overlayLinesOnTop : _overlayLinesDepthTested;
    sink.insert(sink.end(), vertices.begin(), vertices.end());
}

void SceneRenderer::SetIconSuppressedEntities(std::span<const ECS::Entity> entities)
{
    if (!_editorVisuals)
    {
        return;
    }
    _iconSuppressed.assign(entities.begin(), entities.end());
}

void SceneRenderer::SubmitEditorIcons(std::span<const glm::vec3> positions)
{
    if (!_editorVisuals)
    {
        return; // the icon pass was never built (InitParams::enableEditorVisuals off)
    }
    _submittedIcons.insert(_submittedIcons.end(), positions.begin(), positions.end());
}

void SceneRenderer::SubmitIconOutline(const glm::vec3 &position)
{
    if (!_editorVisuals)
    {
        return;
    }
    _submittedIconOutlines.push_back(position);
}

void SceneRenderer::SubmitOutlineGroup(std::span<const Render::OutlinePass::OutlineItem> items,
                                       const glm::vec3 &color)
{
    if (!_editorVisuals || items.empty())
    {
        return;
    }
    OutlineGroup group;
    group.items.assign(items.begin(), items.end());
    group.color = color;
    _outlineGroups.push_back(std::move(group));
}

void SceneRenderer::SubmitOutline(const Render::MeshBuffer *mesh, const glm::mat4 &model, const glm::vec3 &color)
{
    if (mesh == nullptr)
    {
        return;
    }
    const Render::OutlinePass::OutlineItem item{mesh, model};
    SubmitOutlineGroup(std::span<const Render::OutlinePass::OutlineItem>(&item, 1), color);
}

void SceneRenderer::DrawEditorIcons(const Render::RenderFrame &frame, const glm::mat4 &viewProjection,
                                    const glm::mat4 &view, const glm::vec3 &cameraPosition, ECS::Scene &scene)
{
    if (!_iconPass.IsValid())
    {
        return;
    }

    // Camera world-space basis is the first two rows of the view matrix (the view
    // rotation is the transpose of the camera's world rotation).
    const glm::vec3 cameraRight(view[0][0], view[1][0], view[2][0]);
    const glm::vec3 cameraUp(view[0][1], view[1][1], view[2][1]);

    constexpr float maxDistanceSq = kMaxIconDistance * kMaxIconDistance;
    _iconPositions.clear();

    // One icon per placement-only entity (has a Transform, no mesh to draw), unless
    // it is beyond the LOD distance from the camera — editor decoration, shown only
    // when editor icons are enabled.
    if (_editorIconsVisible)
    {
        for (auto [entity, transform] : scene.Query<Transform>(ECS::Without<MeshRenderer>{}))
        {
            if (IsIconSuppressed(entity))
            {
                continue;
            }
            const glm::vec3 position(transform.worldMatrix[3]);
            const glm::vec3 offset = position - cameraPosition;
            if (glm::dot(offset, offset) <= maxDistanceSq)
            {
                _iconPositions.push_back(position);
            }
        }
    }

    // A MeshRenderer whose mesh is still streaming in (meshBuffer == null) shows the
    // same billboard as a placeholder until the mesh pops in — regardless of the
    // editor-icon toggle, since this reflects real load state, not editor chrome.
    // Not distance-culled: a loading entity should never be invisibly absent.
    for (auto [entity, transform, meshRenderer] : scene.Query<Transform, MeshRenderer>())
    {
        if (meshRenderer.meshBuffer == nullptr && !IsIconSuppressed(entity))
        {
            _iconPositions.emplace_back(transform.worldMatrix[3]);
        }
    }

    // Billboards the caller placed by hand, for things that are not entities — a
    // blueprint instance's root, which has no Transform for the queries above to
    // find. Editor chrome, so gated on the same toggle, but not distance-culled:
    // there are a handful of them and losing the only mark an instance has is worse
    // than drawing one far away.
    if (_editorIconsVisible)
    {
        _iconPositions.insert(_iconPositions.end(), _submittedIcons.begin(), _submittedIcons.end());
    }
    _submittedIcons.clear();

    if (_iconPositions.empty())
    {
        return;
    }
    _iconPass.Draw(frame, viewProjection, cameraRight, cameraUp, _iconPositions);
}

bool SceneRenderer::IsIconSuppressed(ECS::Entity entity) const
{
    return std::find(_iconSuppressed.begin(), _iconSuppressed.end(), entity) != _iconSuppressed.end();
}

void SceneRenderer::DrawHighlightOutline(const Render::RenderFrame &frame, const glm::mat4 &viewProjection,
                                         const glm::mat4 &view, ECS::Scene &scene)
{
    if (!_outlinePass.IsValid())
    {
        // Dropped rather than kept: submissions are per-frame and nothing downstream
        // will ever read these, so holding them grows the vector for the life of the
        // renderer on every frame a selected instance is on screen.
        _submittedIconOutlines.clear();
        return;
    }
    for (const ECS::Entity entity : _highlightedEntities)
    {
        DrawHighlightOutlineFor(entity, frame, viewProjection, view, scene);
    }

    // The same treatment for a submitted billboard that belongs to no entity, so a
    // selected instance reads exactly like a selected entity rather than being the
    // one selection in the editor with no visible border.
    if (!_submittedIconOutlines.empty() && _editorIconsVisible)
    {
        const glm::vec3 cameraRight(view[0][0], view[1][0], view[2][0]);
        const glm::vec3 cameraUp(view[0][1], view[1][1], view[2][1]);
        for (const glm::vec3 &center : _submittedIconOutlines)
        {
            // Always the active colour: a submitted outline is only ever asked for by
            // a selection of exactly one thing (an instance), and that thing is what
            // the inspector is showing.
            _outlinePass.DrawBillboard(frame, viewProjection, center, cameraRight, cameraUp,
                                       0.5f * Render::kEntityIconWorldSize, _iconPass.IconTexture(),
                                       kActiveSelectionOutline);
        }
    }
    _submittedIconOutlines.clear();
}

void SceneRenderer::DrawHighlightOutlineFor(ECS::Entity entity, const Render::RenderFrame &frame,
                                            const glm::mat4 &viewProjection, const glm::mat4 &view,
                                            ECS::Scene &scene)
{
    if (entity == ECS::NullEntity || !scene.IsAlive(entity))
    {
        return;
    }

    const Transform *transform = scene.Get<Transform>(entity);
    if (transform == nullptr)
    {
        return; // no placement — nothing to outline
    }
    const MeshRenderer *renderer = scene.Get<MeshRenderer>(entity);

    // A placement-only entity shows a billboard only in editor mode; a MeshRenderer
    // whose mesh is still loading shows one always (see DrawEditorIcons). Outline
    // whichever is actually on screen so selection tracks it. A suppressed entity
    // (e.g. a meshless collider, marked by its wireframe instead) has no billboard,
    // so there is nothing to outline — its selection reads from the wireframe colour.
    const bool placementIcon = renderer == nullptr && _editorIconsVisible && !IsIconSuppressed(entity);
    const bool loadingMesh =
        renderer != nullptr && renderer->meshBuffer == nullptr && !IsIconSuppressed(entity);

    // The one the inspector is talking about reads redder than the rest. With one
    // thing selected it is that thing, so an ordinary click gets the active colour.
    const glm::vec3 color = entity == _activeHighlight ? kActiveSelectionOutline : kSelectionOutline;

    if (renderer != nullptr && renderer->meshBuffer != nullptr)
    {
        _outlinePass.Draw(frame, viewProjection, *renderer->meshBuffer, transform->worldMatrix, color);
    }
    else if (placementIcon || loadingMesh)
    {
        // Outline the billboard quad so its selection matches a mesh's.
        const glm::vec3 center(transform->worldMatrix[3]);
        const glm::vec3 cameraRight(view[0][0], view[1][0], view[2][0]);
        const glm::vec3 cameraUp(view[0][1], view[1][1], view[2][1]);
        _outlinePass.DrawBillboard(frame, viewProjection, center, cameraRight, cameraUp,
                                   0.5f * Render::kEntityIconWorldSize, _iconPass.IconTexture(), color);
    }
}

} // namespace Assisi::Runtime
