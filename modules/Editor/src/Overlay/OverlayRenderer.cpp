/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/Overlay/OverlayRenderer.hpp>

#include <algorithm>

#include <Assisi/Chiara/Profile.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Render/GpuMarker.hpp>
#include <Assisi/Render/MeshBuffer.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/SceneRenderer.hpp>

namespace Assisi::Editor
{

using Runtime::Camera;
using Runtime::MeshRenderer;
using Runtime::Transform;

namespace
{

// Selection-outline shaders (screen-space edge detect; see OutlinePass): a mask
// pass that stamps the silhouette, and a fullscreen edge pass that paints the
// border. These live under assets/editor/ because nothing but an editor loads
// them — except the edge pass's vertex stage, which reuses the shared
// fullscreen-triangle shader.
constexpr const char *kOutlineMaskVertexShader = "editor/shaders/outline_mask.vert.spv";
constexpr const char *kOutlineMaskPixelShader = "editor/shaders/outline_mask.frag.spv";
constexpr const char *kOutlineEdgeVertexShader = "shaders/fullscreen.vert.spv";
constexpr const char *kOutlineEdgePixelShader = "editor/shaders/outline_edge.frag.spv";

// Generic overlay-line renderer (see LinePass), used for collider wireframes.
constexpr const char *kLineVertexShader = "editor/shaders/line.vert.spv";
constexpr const char *kLinePixelShader = "editor/shaders/line.frag.spv";

// Entity-icon billboard (see IconPass). The icon image is authored content
// dropped at this virtual path; until it exists the pass shows a magenta
// placeholder.
constexpr const char *kIconVertexShader = "editor/shaders/icon_billboard.vert.spv";
constexpr const char *kIconPixelShader = "editor/shaders/icon_billboard.frag.spv";
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

bool OverlayRenderer::Initialize(nvrhi::IDevice *device, const nvrhi::FramebufferInfo &overlayFramebufferInfo,
                                 uint32_t width, uint32_t height)
{
    if (device == nullptr)
    {
        Core::Log::Error("OverlayRenderer: no device.");
        return false;
    }

    // Each overlay is independently optional: one that fails to build warns and
    // is dropped, because losing entity icons should not cost the editor its
    // selection outlines, still less its scene.
    if (!_outlinePass.Initialize(device, overlayFramebufferInfo, width, height, kOutlineMaskVertexShader,
                                 kOutlineMaskPixelShader, kOutlineEdgeVertexShader, kOutlineEdgePixelShader,
                                 kIconVertexShader, kIconMaskPixelShader))
    {
        Core::Log::Warn("OverlayRenderer: selection outline unavailable (outline pass failed to initialise).");
    }

    if (!_iconPass.Initialize(device, overlayFramebufferInfo, kIconVertexShader, kIconPixelShader,
                              kEntityIconTexture))
    {
        Core::Log::Warn("OverlayRenderer: entity icons unavailable (icon pass failed to initialise).");
    }

    if (!_linePass.Initialize(device, overlayFramebufferInfo, kLineVertexShader, kLinePixelShader))
    {
        Core::Log::Warn("OverlayRenderer: overlay lines unavailable (line pass failed to initialise).");
    }

    return true;
}

bool OverlayRenderer::OnRenderTargetsChanged(const nvrhi::FramebufferInfo &overlayFramebufferInfo)
{
    bool ok = _outlinePass.RebuildPipeline(overlayFramebufferInfo);
    ok      = _iconPass.RebuildPipeline(overlayFramebufferInfo) && ok;
    ok      = _linePass.RebuildPipeline(overlayFramebufferInfo) && ok;
    return ok;
}

void OverlayRenderer::SubmitOverlayLines(std::span<const LineVertex> vertices, bool onTop)
{
    std::vector<LineVertex> &sink = onTop ? _overlayLinesOnTop : _overlayLinesDepthTested;
    sink.insert(sink.end(), vertices.begin(), vertices.end());
}

void OverlayRenderer::SetIconSuppressedEntities(std::span<const ECS::Entity> entities)
{
    _iconSuppressed.assign(entities.begin(), entities.end());
}

void OverlayRenderer::SubmitEditorIcons(std::span<const glm::vec3> positions)
{
    _submittedIcons.insert(_submittedIcons.end(), positions.begin(), positions.end());
}

void OverlayRenderer::SubmitIconOutline(const glm::vec3 &position)
{
    _submittedIconOutlines.push_back(position);
}

void OverlayRenderer::SubmitOutlineGroup(std::span<const OutlinePass::OutlineItem> items, const glm::vec3 &color)
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

void OverlayRenderer::SubmitOutline(const Render::MeshBuffer *mesh, const glm::mat4 &model, const glm::vec3 &color)
{
    if (mesh == nullptr)
    {
        return;
    }
    const OutlinePass::OutlineItem item{mesh, model};
    SubmitOutlineGroup(std::span<const OutlinePass::OutlineItem>(&item, 1), color);
}

bool OverlayRenderer::IsIconSuppressed(ECS::Entity entity) const
{
    return std::find(_iconSuppressed.begin(), _iconSuppressed.end(), entity) != _iconSuppressed.end();
}

void OverlayRenderer::Render(const Render::RenderFrame &frame, ECS::Scene &scene, const Transform &cameraTransform,
                             const Camera &camera, const Runtime::SceneRenderer &sceneRenderer)
{
    // Recomputed rather than carried over from the scene draw: both are two
    // matrix builds from state neither of them changes, and a cached pair is one
    // more thing that can go stale between the two calls.
    const glm::mat4 projection = Runtime::ProjectionMatrix(
        camera, AspectRatio(static_cast<int32_t>(frame.width), static_cast<int32_t>(frame.height)));
    const glm::mat4 view = Runtime::ViewMatrix(cameraTransform);
    const glm::mat4 viewProjection = projection * view;

    {
        ASSISI_PROFILE_GPU_PASS(frame.commandList, "editor-icons");
        DrawEditorIcons(frame, viewProjection, view, cameraTransform.position, scene);
    }

    // Submitted silhouette outlines (the selected object's collider + mesh). Each
    // group is its own edge-detect pass, so a collider and the mesh it wraps
    // outline independently rather than merging into one border.
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

        DrawHighlightOutline(frame, viewProjection, view, scene, sceneRenderer);
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

void OverlayRenderer::DrawEditorIcons(const Render::RenderFrame &frame, const glm::mat4 &viewProjection,
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

    // One icon per placement-only entity (has a Transform, no mesh to draw),
    // unless it is beyond the LOD distance from the camera.
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

    // Billboards the caller placed by hand, for things that are not entities — a
    // blueprint instance's root, which has no Transform for the query above to
    // find. Not distance-culled: there are a handful of them and losing the only
    // mark an instance has is worse than drawing one far away.
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

void OverlayRenderer::DrawHighlightOutline(const Render::RenderFrame &frame, const glm::mat4 &viewProjection,
                                           const glm::mat4 &view, ECS::Scene &scene,
                                           const Runtime::SceneRenderer &sceneRenderer)
{
    if (!_outlinePass.IsValid())
    {
        // Dropped rather than kept: submissions are per-frame and nothing
        // downstream will ever read these, so holding them grows the vector for
        // the life of the renderer on every frame a selected instance is on screen.
        _submittedIconOutlines.clear();
        return;
    }
    for (const ECS::Entity entity : _highlightedEntities)
    {
        DrawHighlightOutlineFor(frame, viewProjection, view, entity, sceneRenderer, scene);
    }

    // The same treatment for a submitted billboard that belongs to no entity, so
    // a selected instance reads exactly like a selected entity rather than being
    // the one selection in the editor with no visible border.
    if (!_submittedIconOutlines.empty() && _editorIconsVisible)
    {
        const glm::vec3 cameraRight(view[0][0], view[1][0], view[2][0]);
        const glm::vec3 cameraUp(view[0][1], view[1][1], view[2][1]);
        for (const glm::vec3 &center : _submittedIconOutlines)
        {
            // Always the active colour: a submitted outline is only ever asked for
            // by a selection of exactly one thing (an instance), and that thing is
            // what the inspector is showing.
            _outlinePass.DrawBillboard(frame, viewProjection, center, cameraRight, cameraUp,
                                       0.5f * kEntityIconWorldSize, _iconPass.IconTexture(),
                                       kActiveSelectionOutline);
        }
    }
    _submittedIconOutlines.clear();
}

void OverlayRenderer::DrawHighlightOutlineFor(const Render::RenderFrame &frame, const glm::mat4 &viewProjection,
                                              const glm::mat4 &view, ECS::Entity entity,
                                              const Runtime::SceneRenderer &sceneRenderer, ECS::Scene &scene)
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

    // A placement-only entity shows a billboard only while icons are on. A
    // suppressed entity (a meshless collider, marked by its wireframe instead)
    // has no billboard, so there is nothing to outline — its selection reads from
    // the wireframe colour.
    const bool placementIcon = renderer == nullptr && _editorIconsVisible && !IsIconSuppressed(entity);

    // The one the inspector is talking about reads redder than the rest. With one
    // thing selected it is that thing, so an ordinary click gets the active colour.
    const glm::vec3 color = entity == _activeHighlight ? kActiveSelectionOutline : kSelectionOutline;

    if (renderer != nullptr && renderer->meshBuffer != nullptr)
    {
        // The level the mesh pass drew this entity at, not a second opinion: a
        // border traced around a finer silhouette than the one on screen reads as
        // a halo.
        _outlinePass.Draw(frame, viewProjection,
                          OutlinePass::OutlineItem{renderer->meshBuffer, transform->worldMatrix,
                                                   sceneRenderer.DrawnLodLevel(entity, *renderer->meshBuffer,
                                                                               transform->worldMatrix)},
                          color);
    }
    else if (placementIcon)
    {
        // Outline the billboard quad so its selection matches a mesh's.
        const glm::vec3 center(transform->worldMatrix[3]);
        const glm::vec3 cameraRight(view[0][0], view[1][0], view[2][0]);
        const glm::vec3 cameraUp(view[0][1], view[1][1], view[2][1]);
        _outlinePass.DrawBillboard(frame, viewProjection, center, cameraRight, cameraUp,
                                   0.5f * kEntityIconWorldSize, _iconPass.IconTexture(), color);
    }
}

} // namespace Assisi::Editor
