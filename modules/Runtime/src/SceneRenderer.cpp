/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Runtime/SceneRenderer.hpp>

#include <algorithm>
#include <utility>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/Renderer.hpp>

namespace Assisi::Runtime
{
namespace
{
// Engine default scene shaders (opaque lit geometry, clustered forward). Compiled
// under the asset root by the build; resolved through Core::AssetSystem.
constexpr const char *kSceneVertexShader = "shaders/cube_min.vert.spv";
constexpr const char *kScenePixelShader = "shaders/cube_min.frag.spv";

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

// Selection-highlight outline colour (the always-on-top border around a selected
// entity's mesh/icon). Matches the "selected" collider colour the editor uses.
constexpr glm::vec3 kSelectionOutlineColor{1.0f, 0.45f, 0.0f};

float AspectRatio(int width, int height)
{
    return height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.f;
}
} // namespace

bool SceneRenderer::Initialize(nvrhi::IDevice *device, const nvrhi::FramebufferInfo &framebufferInfo, int width,
                               int height, const Camera &camera, nvrhi::IBindingLayout *bindlessLayout,
                               nvrhi::IDescriptorTable *bindlessTable, nvrhi::IBuffer *materialTable)
{
    _device = device;

    const glm::mat4 projection = ProjectionMatrix(camera, AspectRatio(width, height));

    // The cluster grid's light buffers must exist before MeshPass::Initialize,
    // which binds them into every binding set it creates — so build lighting first.
    nvrhi::CommandListHandle setupCommandList = device->createCommandList();
    setupCommandList->open();
    const bool lightingOk =
        _lighting.Initialize(device, setupCommandList, width, height, camera.nearZ, camera.farZ, projection);
    setupCommandList->close();
    device->executeCommandList(setupCommandList);

    if (!lightingOk)
    {
        Core::Log::Error("SceneRenderer: failed to initialise the clustered lighting pipeline.");
        return false;
    }
    _clusterProjection = projection;

    if (!_meshPass.Initialize(Render::MeshPass::InitParams{.device = device,
                                                           .framebufferInfo = framebufferInfo,
                                                           .vertexShaderSpvPath = kSceneVertexShader,
                                                           .pixelShaderSpvPath = kScenePixelShader,
                                                           .clusterGrid = &_lighting.Grid(),
                                                           .bindlessLayout = bindlessLayout,
                                                           .bindlessTable = bindlessTable,
                                                           .materialTable = materialTable}))
    {
        Core::Log::Error("SceneRenderer: failed to initialise the scene mesh pass.");
        return false;
    }

    // GPU-driven cull (stage F1). Non-fatal: if the compute pipeline fails to
    // build, the "GPU Cull" toggle stays a no-op and the CPU draw path runs.
    if (!_meshCuller.Initialize(device))
    {
        Core::Log::Warn("SceneRenderer: GPU cull unavailable (mesh_cull compute pipeline failed to build).");
    }

    // The selection outline is an editor/gameplay convenience, not core to drawing
    // a scene — if its pipelines fail to build, log and carry on without it rather
    // than failing the whole renderer.
    if (!_outlinePass.Initialize(device, framebufferInfo, static_cast<uint32_t>(width),
                                 static_cast<uint32_t>(height), kOutlineMaskVertexShader, kOutlineMaskPixelShader,
                                 kOutlineEdgeVertexShader, kOutlineEdgePixelShader, kIconVertexShader,
                                 kIconMaskPixelShader))
    {
        Core::Log::Warn("SceneRenderer: selection outline unavailable (outline pass failed to initialise).");
    }

    // Editor entity icons are likewise a convenience overlay; a failure here just
    // drops the icons, it must not fail the whole renderer.
    if (!_iconPass.Initialize(device, framebufferInfo, kIconVertexShader, kIconPixelShader, kEntityIconTexture))
    {
        Core::Log::Warn("SceneRenderer: entity icons unavailable (icon pass failed to initialise).");
    }

    // The overlay line renderer (collider wireframes) is editor chrome too; a
    // failure just drops the lines rather than failing the renderer.
    if (!_linePass.Initialize(device, framebufferInfo, kLineVertexShader, kLinePixelShader))
    {
        Core::Log::Warn("SceneRenderer: overlay lines unavailable (line pass failed to initialise).");
    }

    return true;
}

void SceneRenderer::RebuildClusterGrid(int width, int height, const Camera &camera,
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

void SceneRenderer::Resize(int width, int height, const Camera &camera)
{
    RebuildClusterGrid(width, height, camera, ProjectionMatrix(camera, AspectRatio(width, height)));
}

bool SceneRenderer::OnRenderTargetsChanged(const nvrhi::FramebufferInfo &framebufferInfo)
{
    if (!_meshPass.IsValid())
    {
        return true; // nothing built yet — nothing to rebuild
    }
    // Rebuild the outline pipelines against the new format too; a failure there
    // only drops the highlight, so it doesn't fail the render-target change.
    if (!_outlinePass.RebuildPipeline(framebufferInfo))
    {
        Core::Log::Warn("SceneRenderer: selection outline pipeline rebuild failed; highlight disabled.");
    }
    if (!_iconPass.RebuildPipeline(framebufferInfo))
    {
        Core::Log::Warn("SceneRenderer: entity-icon pipeline rebuild failed; icons disabled.");
    }
    if (!_linePass.RebuildPipeline(framebufferInfo))
    {
        Core::Log::Warn("SceneRenderer: overlay-line pipeline rebuild failed; collider wireframes disabled.");
    }
    return _meshPass.RebuildPipeline(framebufferInfo);
}

void SceneRenderer::Render(const Render::RenderFrame &frame, ECS::Scene &scene,
                           const Transform &cameraTransform, const Camera &camera)
{
    if (!_meshPass.IsValid())
    {
        return;
    }

    // Refresh world matrices before anything reads them (view matrix, draw). Only
    // entities whose transform changed since last frame are recomputed; the tick
    // bookmark carries that across frames.
    _lastPropagationTick = PropagateTransforms(scene, _lastPropagationTick);

    const glm::mat4 projection = ProjectionMatrix(camera, AspectRatio(static_cast<int>(frame.width),
                                                                      static_cast<int>(frame.height)));
    const glm::mat4 view = ViewMatrix(cameraTransform);

    // Keep the froxel grid aligned with the render projection; a drift (window
    // resize, runtime FOV/near/far edit) makes peripheral froxels stop matching
    // it and shows rectangular lighting artifacts.
    if (projection != _clusterProjection)
    {
        RebuildClusterGrid(static_cast<int>(frame.width), static_cast<int>(frame.height), camera, projection);
    }

    _lighting.Update(frame.commandList, scene, view);
    _meshPass.UpdateFrameConstants(frame.commandList, projection * view, view, frame.width, frame.height, camera.nearZ,
                                   camera.farZ, _lighting.DirLightCount(), _debugView);
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

    DrawEditorIcons(frame, projection * view, view, cameraTransform.position, scene);

    // Submitted silhouette outlines (the selected object's collider + mesh). Each
    // group is its own edge-detect pass, so a collider and the mesh it wraps outline
    // independently rather than merging into one border. Drawn on top of the scene.
    if (_outlinePass.IsValid())
    {
        for (const OutlineGroup &group : _outlineGroups)
        {
            _outlinePass.DrawOutlines(frame, projection * view, group.items, group.color);
        }
    }
    _outlineGroups.clear();

    DrawHighlightOutline(frame, projection * view, view, scene);

    // Overlay lines (collider wireframes) sit on top of everything else: the
    // depth-tested batch first (occluded by the scene), then the on-top batch
    // (x-ray). Both are cleared afterwards so the caller re-submits each frame.
    if (_linePass.IsValid())
    {
        _linePass.Draw(frame, projection * view, _overlayLinesDepthTested, /*onTop=*/false);
        _linePass.Draw(frame, projection * view, _overlayLinesOnTop, /*onTop=*/true);
    }
    _overlayLinesDepthTested.clear();
    _overlayLinesOnTop.clear();
    _iconSuppressed.clear();
}

void SceneRenderer::SubmitOverlayLines(std::span<const Render::LineVertex> vertices, bool onTop)
{
    std::vector<Render::LineVertex> &sink = onTop ? _overlayLinesOnTop : _overlayLinesDepthTested;
    sink.insert(sink.end(), vertices.begin(), vertices.end());
}

void SceneRenderer::SetIconSuppressedEntities(std::span<const ECS::Entity> entities)
{
    _iconSuppressed.assign(entities.begin(), entities.end());
}

void SceneRenderer::SubmitOutlineGroup(std::span<const Render::OutlinePass::OutlineItem> items,
                                       const glm::vec3 &color)
{
    if (items.empty())
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
    if (!_outlinePass.IsValid() || _highlightedEntity == ECS::NullEntity || !scene.IsAlive(_highlightedEntity))
    {
        return;
    }

    const Transform *transform = scene.Get<Transform>(_highlightedEntity);
    if (transform == nullptr)
    {
        return; // no placement — nothing to outline
    }
    const MeshRenderer *renderer = scene.Get<MeshRenderer>(_highlightedEntity);

    // A placement-only entity shows a billboard only in editor mode; a MeshRenderer
    // whose mesh is still loading shows one always (see DrawEditorIcons). Outline
    // whichever is actually on screen so selection tracks it. A suppressed entity
    // (e.g. a meshless collider, marked by its wireframe instead) has no billboard,
    // so there is nothing to outline — its selection reads from the wireframe colour.
    const bool placementIcon = renderer == nullptr && _editorIconsVisible && !IsIconSuppressed(_highlightedEntity);
    const bool loadingMesh    = renderer != nullptr && renderer->meshBuffer == nullptr &&
                             !IsIconSuppressed(_highlightedEntity);

    if (renderer != nullptr && renderer->meshBuffer != nullptr)
    {
        _outlinePass.Draw(frame, viewProjection, *renderer->meshBuffer, transform->worldMatrix,
                          kSelectionOutlineColor);
    }
    else if (placementIcon || loadingMesh)
    {
        // Outline the billboard quad so its selection matches a mesh's.
        const glm::vec3 center(transform->worldMatrix[3]);
        const glm::vec3 cameraRight(view[0][0], view[1][0], view[2][0]);
        const glm::vec3 cameraUp(view[0][1], view[1][1], view[2][1]);
        _outlinePass.DrawBillboard(frame, viewProjection, center, cameraRight, cameraUp,
                                   0.5f * Render::kEntityIconWorldSize, _iconPass.IconTexture(),
                                   kSelectionOutlineColor);
    }
}

} // namespace Assisi::Runtime
