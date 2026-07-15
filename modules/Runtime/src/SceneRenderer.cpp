/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Runtime/SceneRenderer.hpp>

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
// the orange border. The edge pass reuses the shared fullscreen-triangle vertex shader.
constexpr const char *kOutlineMaskVertexShader = "shaders/outline_mask.vert.spv";
constexpr const char *kOutlineMaskPixelShader  = "shaders/outline_mask.frag.spv";
constexpr const char *kOutlineEdgeVertexShader = "shaders/fullscreen.vert.spv";
constexpr const char *kOutlineEdgePixelShader  = "shaders/outline_edge.frag.spv";

float AspectRatio(int width, int height)
{
    return height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.f;
}
} // namespace

bool SceneRenderer::Initialize(nvrhi::IDevice *device, const nvrhi::FramebufferInfo &framebufferInfo, int width,
                               int height, const Camera &camera)
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

    if (!_meshPass.Initialize(device, framebufferInfo, kSceneVertexShader, kScenePixelShader, _lighting.Grid()))
    {
        Core::Log::Error("SceneRenderer: failed to initialise the scene mesh pass.");
        return false;
    }

    // The selection outline is an editor/gameplay convenience, not core to drawing
    // a scene — if its pipelines fail to build, log and carry on without it rather
    // than failing the whole renderer.
    if (!_outlinePass.Initialize(device, framebufferInfo, static_cast<uint32_t>(width),
                                 static_cast<uint32_t>(height), kOutlineMaskVertexShader, kOutlineMaskPixelShader,
                                 kOutlineEdgeVertexShader, kOutlineEdgePixelShader))
    {
        Core::Log::Warn("SceneRenderer: selection outline unavailable (outline pass failed to initialise).");
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
    _meshPass.UpdateFrameConstants(frame.commandList, view, frame.width, frame.height, camera.nearZ, camera.farZ,
                                   _lighting.DirLightCount(), _debugView);
    _lastDrawStats = DrawScene(DrawSceneParams{.scene          = scene,
                                               .meshPass       = _meshPass,
                                               .frame          = frame,
                                               .view           = view,
                                               .projection     = projection,
                                               .nearZ          = camera.nearZ,
                                               .farZ           = camera.farZ,
                                               .frustumCulling = _frustumCulling,
                                               .sortDraws      = _sortDraws});

    DrawHighlightOutline(frame, projection * view, scene);
}

void SceneRenderer::DrawHighlightOutline(const Render::RenderFrame &frame, const glm::mat4 &viewProjection,
                                         ECS::Scene &scene)
{
    if (!_outlinePass.IsValid() || _highlightedEntity == ECS::NullEntity || !scene.IsAlive(_highlightedEntity))
    {
        return;
    }

    const Transform    *transform = scene.Get<Transform>(_highlightedEntity);
    const MeshRenderer *renderer  = scene.Get<MeshRenderer>(_highlightedEntity);
    if (transform == nullptr || renderer == nullptr || renderer->meshBuffer == nullptr)
    {
        return; // nothing to outline (no placement or no resolved mesh)
    }

    _outlinePass.Draw(frame, viewProjection, *renderer->meshBuffer, transform->worldMatrix);
}

} // namespace Assisi::Runtime
