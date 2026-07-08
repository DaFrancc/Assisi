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

float AspectRatio(int width, int height)
{
    return height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.f;
}
} // namespace

bool SceneRenderer::Initialize(nvrhi::IDevice *device, const nvrhi::FramebufferInfo &framebufferInfo, int width,
                               int height, const CameraComponent &camera)
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

    return true;
}

void SceneRenderer::RebuildClusterGrid(int width, int height, const CameraComponent &camera,
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

void SceneRenderer::Resize(int width, int height, const CameraComponent &camera)
{
    RebuildClusterGrid(width, height, camera, ProjectionMatrix(camera, AspectRatio(width, height)));
}

bool SceneRenderer::OnRenderTargetsChanged(const nvrhi::FramebufferInfo &framebufferInfo)
{
    if (!_meshPass.IsValid())
    {
        return true; // nothing built yet — nothing to rebuild
    }
    return _meshPass.RebuildPipeline(framebufferInfo);
}

void SceneRenderer::Render(const Render::RenderFrame &frame, ECS::Scene &scene,
                           const TransformComponent &cameraTransform, const CameraComponent &camera)
{
    if (!_meshPass.IsValid())
    {
        return;
    }

    // Refresh world matrices before anything reads them (view matrix, draw).
    PropagateTransforms(scene);

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
                                   _lighting.DirLightCount());
    DrawScene(scene, view, projection, frame.commandList, frame.framebuffer, frame.width, frame.height, _meshPass);
}

} // namespace Assisi::Runtime
