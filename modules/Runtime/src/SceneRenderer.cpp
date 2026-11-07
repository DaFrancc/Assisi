/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Runtime/SceneRenderer.hpp>

#include <Assisi/Runtime/SkyResolve.hpp>

#include <cmath>
#include <cstdint>

#include <Assisi/Chiara/Profile.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/Math/Matrix.hpp>
#include <Assisi/Render/GpuMarker.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/FogVolumeGather.hpp>
#include <Assisi/Runtime/PostProcessResolve.hpp>
#include <Assisi/Runtime/Renderer.hpp>
#include <Assisi/Runtime/SkinnedMeshPose.hpp>

namespace Assisi::Runtime
{
namespace
{
// Engine default scene shaders (opaque lit geometry, clustered forward). Compiled
// under the asset root by the build; resolved through Core::AssetSystem.
constexpr const char *kSceneVertexShader = "shaders/mesh.vert.spv";
constexpr const char *kScenePixelShader = "shaders/mesh.frag.spv";
// The same pixel shader built for see-through surfaces, for the mesh pass's
// blended pipelines.
constexpr const char *kSceneBlendedPixelShader = "shaders/mesh.frag.blended.spv";

// The depth prepass, and the lit pass's vertex stage for drawing over it (see
// MeshPass::RebuildPipeline).
constexpr const char *kSceneDepthVertexShader = "shaders/mesh.vert.depth.spv";
constexpr const char *kSceneMaskedDepthVertexShader = "shaders/mesh.vert.depth.masked.spv";
constexpr const char *kSceneMaskedDepthPixelShader = "shaders/mesh_depth.frag.spv";
constexpr const char *kSceneInvariantVertexShader = "shaders/mesh.vert.invariant.spv";

// The prepass's depth as a distance per pixel (see Render::SceneDistancePass).
constexpr const char *kFullscreenVertexShader = "shaders/fullscreen.vert.spv";
constexpr const char *kSceneDistanceShader = "shaders/scene_distance.frag.spv";
constexpr const char *kSceneMultisampleDistanceShader = "shaders/scene_distance.frag.msaa.spv";

// Screen-space occlusion's three fullscreen steps (see Render::SsaoPass).
constexpr const char *kSsaoOcclusionShader = "shaders/ssao.frag.spv";
constexpr const char *kSsaoBlurShader = "shaders/ssao_blur.frag.spv";

// The sun's cascade depth pass — a vertex stage and nothing else, since the
// pipeline writes depth and no colour (see Render::ShadowPass).
constexpr const char *kShadowVertexShader = "shaders/shadow_depth.vert.spv";
// The alpha-tested caster variant: the same vertex stage built to carry the UV
// and material row, and the fragment stage that discards on them, so a cutout
// material casts a shadow with its hole in it.
constexpr const char *kShadowMaskedVertexShader = "shaders/shadow_depth.vert.masked.spv";
constexpr const char *kShadowMaskedPixelShader = "shaders/shadow_depth.frag.spv";
// Sets one tile of the local-light atlas back to far depth (see
// Render::LocalShadowPass::ResetTile).
constexpr const char *kShadowTileResetShader = "shaders/shadow_tile_reset.vert.spv";
// Writes a cascade's still depth back where movers stood (see
// Render::ShadowPass::RestoreStillDepth), through the tile reset's triangle.
constexpr const char *kShadowDepthRestoreShader = "shaders/shadow_depth_restore.frag.spv";

// The analytic sky (see Render::SkyPass). A fullscreen triangle at the far
// plane, so its vertex stage is its own rather than the shared one: that one
// emits depth 0, and the sky has to land on the 1.0 the depth clear left.
constexpr const char *kSkyVertexShader = "shaders/sky.vert.spv";
constexpr const char *kSkyPixelShader = "shaders/sky.frag.spv";

// The sky probe's GGX prefilter (see Render::SkyProbe). The capture itself goes
// through the sky shaders above.
constexpr const char *kSkyPrefilterShader = "shaders/sky_prefilter.comp.spv";

// The clouds (see Render::CloudPass): their noise, drawn once, and the sky map
// and shadow map, drawn every frame.
constexpr const char *kCloudNoiseShader = "shaders/cloud_noise.comp.spv";
constexpr const char *kCloudNoiseDownsampleShader = "shaders/cloud_noise_downsample.comp.spv";
constexpr const char *kCloudWeatherShader = "shaders/cloud_weather.comp.spv";
constexpr const char *kCloudMapShader = "shaders/cloud_map.comp.spv";
constexpr const char *kCloudMapResolveShader = "shaders/cloud_map_resolve.comp.spv";
constexpr const char *kCloudShadowShader = "shaders/cloud_shadow.comp.spv";

// The moon's albedo, stamped on its disk. An engine constant rather than a field
// on the Moon component: authoring a second moon's texture is a feature nobody
// has asked for, and a path in a level file is a path that can rot there.
constexpr const char *kMoonTexture = "textures/moon.jpg";

// The frame's fog constants, coloured by @p sky when the scene has one and by
// the fog's own colour when it does not. Off unless @p drawn, but coloured all
// the same: a lit fog volume takes the haze's colour whether or not the level
// has a Fog, and in a level with neither a sky nor a Fog that colour is white,
// so the volume shows its own.
Render::FogConstants FrameFog(const FogResolution &fog, bool drawn, const SkyResolution &sky, float cameraHeight,
                              float drawDistance)
{
    const Render::FogSettings settings = fog.present ? fog.settings : Render::FogSettings{.color = glm::vec3(1.f)};
    Render::FogConstants constants;
    if (sky.status != SkyStatus::Ready)
    {
        constants = Render::MakeFogConstants(settings, cameraHeight, drawDistance, nullptr);
    }
    else
    {
        const Render::SkyScatterConstants scatter = Render::MakeSkyScatterConstants(sky.sun, sky.moon, sky.settings);
        constants = Render::MakeFogConstants(settings, cameraHeight, drawDistance, &scatter);
    }
    if (!drawn)
    {
        constants.view.x = 0.f;
    }
    return constants;
}

// The blueprint instance @p camera belongs to, or none for a camera in no
// instance or no camera at all.
ECS::InstanceId ViewerInstance(const ECS::Scene &scene, ECS::Entity camera)
{
    const ECS::BlueprintMember *member = camera == ECS::NullEntity ? nullptr : scene.Get<ECS::BlueprintMember>(camera);
    return member != nullptr ? member->instanceId : ECS::InstanceId{};
}
} // namespace

bool SceneRenderer::Initialize(const InitParams &params)
{
    _device = params.device;

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

    // Before the mesh pass: it binds both shadow maps into every binding set it
    // builds, and each pass keeps a one-texel empty texture there until
    // something wants shadows. A failure is non-fatal — the scene renders
    // unshadowed rather than not at all. Each pass initialises even when the
    // depth renderer failed, since that is what creates its empty texture.
    const bool depthRendererOk = _shadowDepthRenderer.Initialize(
        Render::ShadowDepthRenderer::InitParams{.device = _device,
                                                .vertexShaderSpvPath = kShadowVertexShader,
                                                .maskedVertexShaderSpvPath = kShadowMaskedVertexShader,
                                                .maskedPixelShaderSpvPath = kShadowMaskedPixelShader,
                                                .materialTable = params.materialTable,
                                                .bindlessLayout = params.bindlessLayout,
                                                .bindlessTable = params.bindlessTable});
    const bool sunShadowsOk = _shadowPass.Initialize(
        Render::ShadowPass::InitParams{.device = _device,
                                       .depthRenderer = &_shadowDepthRenderer,
                                       .restoreVertexShaderSpvPath = kShadowTileResetShader,
                                       .restorePixelShaderSpvPath = kShadowDepthRestoreShader});
    if (!depthRendererOk || !sunShadowsOk)
    {
        Core::Log::Warn("SceneRenderer: sun shadows unavailable (the depth pass failed to initialise).");
    }
    // The same renderer, so every shadow view of the frame lands in one table
    // whichever kind of map produced it.
    if (!_localShadowPass.Initialize(
            Render::LocalShadowPass::InitParams{.device = _device,
                                                .depthRenderer = &_shadowDepthRenderer,
                                                .tileResetVertexShaderSpvPath = kShadowTileResetShader}))
    {
        Core::Log::Warn("SceneRenderer: local-light shadows unavailable (the depth pass failed to initialise).");
    }

    // Before both passes that read them, which name the buffers in their binding
    // sets.
    if (!_fogVolumeBuffers.Initialize(_device))
    {
        Core::Log::Error("SceneRenderer: failed to create the fog volume buffers.");
        return false;
    }

    if (!_meshPass.Initialize(
            Render::MeshPass::InitParams{.device = _device,
                                         .framebufferInfo = params.framebufferInfo,
                                         .vertexShaderSpvPath = kSceneVertexShader,
                                         .pixelShaderSpvPath = kScenePixelShader,
                                         .blendedPixelShaderSpvPath = kSceneBlendedPixelShader,
                                         .depthVertexShaderSpvPath = kSceneDepthVertexShader,
                                         .maskedDepthVertexShaderSpvPath = kSceneMaskedDepthVertexShader,
                                         .maskedDepthPixelShaderSpvPath = kSceneMaskedDepthPixelShader,
                                         .invariantVertexShaderSpvPath = kSceneInvariantVertexShader,
                                         .clusterGrid = &_lighting.Grid(),
                                         .fogVolumes = &_fogVolumeBuffers,
                                         .bindlessLayout = params.bindlessLayout,
                                         .bindlessTable = params.bindlessTable,
                                         .materialTable = params.materialTable}))
    {
        Core::Log::Error("SceneRenderer: failed to initialise the scene mesh pass.");
        return false;
    }
    _meshPass.SetShadowMap(_shadowPass.CascadeTexture());
    _meshPass.SetShadowAtlas(_localShadowPass.AtlasTexture());

    // The sky. Non-fatal: without it the scene target keeps its clear colour,
    // which is what every scene looked like before there was a sky at all.
    if (!_skyPass.Initialize(Render::SkyPass::InitParams{.device = _device,
                                                         .framebufferInfo = params.framebufferInfo,
                                                         .vertexShaderSpvPath = kSkyVertexShader,
                                                         .pixelShaderSpvPath = kSkyPixelShader,
                                                         .moonTexturePath = kMoonTexture,
                                                         .fogVolumes = &_fogVolumeBuffers}))
    {
        Core::Log::Warn("SceneRenderer: sky unavailable (the sky pass failed to initialise).");
    }

    // Non-fatal as well: without it a sky lights the scene with the hemisphere
    // alone, which is what it did before there was a probe.
    if (!_skyProbe.Initialize(_device, kSkyPrefilterShader))
    {
        Core::Log::Warn("SceneRenderer: sky reflections unavailable (the prefilter failed to initialise).");
    }

    // Non-fatal: without it a sky has no clouds, which is what every sky had
    // before there were any. Its ambient light still follows a level's clouds.
    if (!_cloudPass.Initialize(Render::CloudPass::InitParams{.device = _device,
                                                             .noiseSpvPath = kCloudNoiseShader,
                                                             .noiseDownsampleSpvPath = kCloudNoiseDownsampleShader,
                                                             .weatherSpvPath = kCloudWeatherShader,
                                                             .mapSpvPath = kCloudMapShader,
                                                             .mapResolveSpvPath = kCloudMapResolveShader,
                                                             .shadowSpvPath = kCloudShadowShader}))
    {
        Core::Log::Warn("SceneRenderer: clouds unavailable (their compute pipelines failed to initialise).");
    }

    // Non-fatal: without it no screen-space feature runs, and the frame is the
    // one there was before any of them existed.
    if (!_sceneDistancePass.Initialize(
            Render::SceneDistancePass::InitParams{.device = _device,
                                                  .vertexShaderSpvPath = kFullscreenVertexShader,
                                                  .distanceShaderSpvPath = kSceneDistanceShader,
                                                  .multisampleDistanceShaderSpvPath = kSceneMultisampleDistanceShader}))
    {
        Core::Log::Warn("SceneRenderer: scene depth unavailable (its pipelines failed to initialise); ambient "
                        "occlusion is off.");
    }

    // Non-fatal too: without it the indirect term is unoccluded, which is what
    // it was before there was any occlusion.
    if (!_ssaoPass.Initialize(Render::SsaoPass::InitParams{.device = _device,
                                                           .vertexShaderSpvPath = kFullscreenVertexShader,
                                                           .occlusionShaderSpvPath = kSsaoOcclusionShader,
                                                           .blurShaderSpvPath = kSsaoBlurShader}))
    {
        Core::Log::Warn("SceneRenderer: ambient occlusion unavailable (its pipelines failed to initialise).");
    }

    // GPU-driven cull (stage F1). Non-fatal: if the compute pipeline fails to
    // build, the "GPU Cull" toggle stays a no-op and the CPU draw path runs.
    if (!_meshCuller.Initialize(_device))
    {
        Core::Log::Warn("SceneRenderer: GPU cull unavailable (mesh_cull compute pipeline failed to build).");
    }

    // Non-fatal too: skinned meshes then draw whatever their ranges last held.
    if (!_meshSkinner.Initialize(_device))
    {
        Core::Log::Warn("SceneRenderer: skinning unavailable (skin compute pipeline failed to build).");
    }

    return true;
}

void SceneRenderer::SkinPosedInstances(const Render::RenderFrame &frame, const ECS::Scene &scene)
{
    const Render::MeshBuffer *anyMesh = GatherPosedInstances(scene, _skinBatch);
    if (anyMesh == nullptr)
    {
        return;
    }
    ASSISI_PROFILE_GPU_PASS(frame.commandList, "skin");
    _meshSkinner.Dispatch(frame.commandList, _skinBatch, anyMesh->VertexBuffer(), anyMesh->SkinBuffer());
}

void SceneRenderer::RebuildClusterGrid(int32_t width, int32_t height, const Camera &camera, const glm::mat4 &projection)
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

void SceneRenderer::WarnNewSkyWarnings(const SkyWarnings &warnings)
{
    const SkyWarnings raised = warnings & ~_skyWarnings;
    _skyWarnings = warnings;
    for (std::size_t bit = 0; bit < raised.size(); ++bit)
    {
        if (raised.test(bit))
        {
            Core::Log::Warn("SceneRenderer: {}", SkyWarningMessage(static_cast<SkyWarning>(bit)));
        }
    }
}

void SceneRenderer::Resize(int32_t width, int32_t height, const Camera &camera)
{
    RebuildClusterGrid(width, height, camera, ProjectionMatrix(camera, AspectRatio(width, height)));
}

bool SceneRenderer::OnRenderTargetsChanged(const nvrhi::FramebufferInfo &framebufferInfo)
{
    if (!_meshPass.IsValid())
    {
        return true; // nothing built yet — nothing to rebuild
    }
    // The sky targets the scene format: it holds radiance and is drawn before the
    // tone map, like the geometry it sits behind.
    if (!_skyPass.RebuildPipeline(framebufferInfo))
    {
        Core::Log::Warn("SceneRenderer: sky pipeline rebuild failed; sky disabled.");
    }
    return _meshPass.RebuildPipeline(framebufferInfo);
}

void SceneRenderer::Render(const Render::RenderFrame &frame, ECS::Scene &scene, const ECS::WorldMatrix &cameraWorld,
                           const Camera &viewCamera, uint64_t &propagationTick)
{
    if (!_meshPass.IsValid())
    {
        return;
    }

    // Before anything is built from the camera: the fog says how far it draws.
    const FogResolution fog = ResolveFog(scene);
    Camera camera = viewCamera;
    if (fog.present)
    {
        camera.farZ = Render::FogDrawDistance(viewCamera.farZ, fog.settings);
    }
    if (fog.several && !_severalFogsWarned)
    {
        Core::Log::Warn("SceneRenderer: more than one Fog in the scene; the first found is used.");
    }
    _severalFogsWarned = fog.several;

    // Refresh world matrices before anything reads them (view matrix, draw). Only
    // entities whose transform changed since last frame are recomputed; the tick
    // bookmark carries that across frames — and belongs to the scene, not to us
    // (see the header).
    {
        ASSISI_PROFILE_SCOPE("propagate-transforms");
        propagationTick = ECS::PropagateTransforms(scene, propagationTick);
    }

    // Before every pass that draws geometry: the shadows and the main pass both
    // read the posed vertices.
    SkinPosedInstances(frame, scene);

    PlaceOrigins(cameraWorld);

    // The look the volumes give where the camera stands.
    const PostProcessResolution look = ResolvePostProcess(scene, ECS::WorldPositionOf(cameraWorld));
    _postProcess = look.settings;
    if (look.levelVolumes > 1u && !_severalLevelPostProcessWarned)
    {
        Core::Log::Warn("SceneRenderer: {} PostProcessVolumes cover the whole level; the highest priority wins.",
                        look.levelVolumes);
    }
    _severalLevelPostProcessWarned = look.levelVolumes > 1u;

    // The camera at the lit pass's origin is its rotation alone, and so is the
    // view: nothing the lit pass reads holds the camera's distance from the
    // world's origin.
    const glm::mat4 cameraInView = ECS::MatrixFrom(cameraWorld, _viewOrigin);
    const glm::mat4 cameraInShadow = ECS::MatrixFrom(cameraWorld, _shadowOrigin);

    const glm::mat4 projection =
        ProjectionMatrix(camera, AspectRatio(static_cast<int32_t>(frame.width), static_cast<int32_t>(frame.height)));
    const glm::mat4 view = ViewMatrix(cameraInView);

    // Keep the froxel grid aligned with the render projection; a drift (window
    // resize, runtime FOV/near/far edit) makes peripheral froxels stop matching
    // it and shows rectangular lighting artifacts.
    if (projection != _clusterProjection && _meshPass.IsValid())
    {
        // Scoped even though it is rare: it is a full grid rebuild, so the one
        // frame that pays it should say so rather than look like a random spike.
        // Recorded into the frame's own list, which is what the scope times, and
        // so an animated FOV or far plane submits nothing extra.
        ASSISI_PROFILE_GPU_PASS(frame.commandList, "cluster-rebuild");
        _lighting.Resize(frame.commandList, static_cast<int32_t>(frame.width), static_cast<int32_t>(frame.height),
                         camera.nearZ, camera.farZ, projection);
        _clusterProjection = projection;
    }

    // The scene's sky, which lights the geometry, is drawn behind it, and says
    // where the sun and the moon are this frame.
    //
    // First, because everything after it depends on the answer: the sun's row in
    // the light buffer is the direction resolved here, the cascades are fitted to
    // that direction, and the indirect term comes off the same sky. Deriving it
    // once at the top is also what makes the frame after a load or a time jump
    // correct — nothing had to tick first.
    _lastSky = ResolveSky(scene);
    const SkyResolution &sky = _lastSky;

    const bool fogged = _fogEnabled && fog.present;
    const Render::FogConstants fogConstants =
        FrameFog(fog, fogged, sky, static_cast<float>(_viewOrigin.y), camera.farZ);

    // Before the probe, whose bake shows them, and seen through the same fog.
    const Render::CloudShadowConstants cloudShadow = UpdateClouds(frame, sky, fogConstants);

    // Before the mesh pass reads it, and off the same sky the frame is lit by.
    const SpecularProbe probe = UpdateSkyProbe(frame, sky);

    // The placed fog volumes, which the player's fog setting turns off with
    // the level's own fog.
    _fogVolumeFrame.Clear();
    if (_fogEnabled)
    {
        ASSISI_PROFILE_SCOPE("fog-volumes");
        const FogVolumeGatherResult gathered = GatherFogVolumes(scene,
                                                                FogVolumeView{.viewProjection = projection * view,
                                                                              .origin = _viewOrigin,
                                                                              .width = frame.width,
                                                                              .height = frame.height},
                                                                _fogVolumeFrame);
        if (gathered.dropped > 0u && !_droppedFogVolumesWarned)
        {
            Core::Log::Warn("SceneRenderer: {} fog volumes past what one frame draws are left out.", gathered.dropped);
        }
        _droppedFogVolumesWarned = gathered.dropped > 0u;
    }
    const glm::uvec4 fogVolumeLanes = _fogVolumeFrame.Lanes();

    // Gathered but not yet uploaded: the local-light atlas decides which lights
    // hold tiles and stamps each winner's view index into the light record, and
    // that stamp has to happen before the lights reach the GPU.
    _lighting.Gather(scene, &sky.light, LightFrame{.view = _viewOrigin, .shadow = _shadowOrigin});

    // Before either half draws, because both are invalidated against it and the
    // change ticks may only be consumed once a frame.
    UpdateShadowMovers(scene);

    // Before the shadow halves, because they select from it too: a caster's
    // shadow is drawn from the level the camera sees it at, or one coarser, and
    // all three passes measure with the one view set here. The GPU cull holds no
    // dead band, so while it draws the gathers hold none either.
    _lodSelector.SetHoldsDeadBand(!GpuCullDraws());
    _lodSelector.BeginFrame(CameraLodView(cameraInView, camera));

    // Each half under a scope of its own, so the main thread's time in their
    // planning and gathers is never left unaccounted between its child scopes.
    Render::MeshPass::ShadowFrameData shadows;
    {
        ASSISI_PROFILE_SCOPE("sun-shadows");
        shadows = RenderSunShadows(frame, scene, camera, cameraInShadow);
    }
    {
        ASSISI_PROFILE_SCOPE("local-shadows");
        RenderLocalShadows(frame, scene, camera, cameraInShadow, shadows);
    }
    // After both, because it reports on both — and outside them, because every
    // early return either of them takes is still a frame with an answer.
    BuildShadowDiagnostics();

    _lighting.Upload(frame.commandList, view);
    _fogVolumeBuffers.Upload(frame.commandList, _fogVolumeFrame);

    // Before the frame constants, which say whether the lit pass reads occlusion.
    const bool occlusion = PrepareScreenOcclusion(frame);

    {
        ASSISI_PROFILE_GPU_SCOPE(frame.commandList, "mesh-constants");
        const Render::MeshPass::FrameConstantsParams frameConstants{.viewProjection = projection * view,
                                                                    .view = view,
                                                                    .screenWidth = frame.width,
                                                                    .screenHeight = frame.height,
                                                                    .nearZ = camera.nearZ,
                                                                    .farZ = camera.farZ,
                                                                    .dirLightCount = _lighting.DirLightCount(),
                                                                    .debugView = _debugView,
                                                                    .indirect = ResolveIndirect(sky, _ambient, probe),
                                                                    .shadows = shadows,
                                                                    .fog = fogConstants,
                                                                    .fogVolumes = fogVolumeLanes,
                                                                    .screenOcclusion = occlusion,
                                                                    .cloudShadow = cloudShadow};
        _meshPass.UpdateFrameConstants(frame.commandList, frameConstants);
    }
    // The depth prepass: the extract, cull and sort, kept for the lit pass below
    // to shade.
    _lastDrawStats =
        DrawScene(DrawSceneParams{.scene = scene,
                                  .meshPass = _meshPass,
                                  .frame = frame,
                                  .view = view,
                                  .projection = projection,
                                  .origin = _viewOrigin,
                                  .nearZ = camera.nearZ,
                                  .farZ = camera.farZ,
                                  .frustumCulling = _frustumCulling,
                                  .sortDraws = _sortDraws,
                                  .gpuCulling = _gpuCulling,
                                  .culler = &_meshCuller,
                                  .cullBuilder = &_cullBuilder,
                                  .lodSelector = &_lodSelector,
                                  .viewerInstance = ViewerInstance(scene, _viewingCamera),
                                  .blendedItems = &_blendedItems});

    if (occlusion)
    {
        _sceneDistancePass.Render(frame.commandList, projection);
        _ssaoPass.Render(frame.commandList,
                         Render::SsaoPass::Frame{.projection = projection, .farZ = camera.farZ, .settings = _ssaoSettings});
    }

    {
        ASSISI_PROFILE_GPU_PASS(frame.commandList, "draw-scene");
        _lastDrawStats.drawCalls += _meshPass.Redraw(frame, Render::MeshPassStage::Lit).drawCalls;
    }

    // The sky goes last, into whatever the geometry left at the depth clear. Both
    // halves of it come from the scene — the sun from a directional light, the
    // look from the Skybox component on that same entity — so a level authors its
    // own world and a light that moves takes the sky with it.
    //
    // A foggy level with no sky still draws the pass, in the fog's own colour,
    // so that what the fog fades into is what is behind it.
    _skyPass.SetFogVolumes(fogConstants, fogVolumeLanes);
    if (sky.status == SkyStatus::Ready)
    {
        _skyPass.SetFog(fogged ? Render::SkyFogMode::BelowHorizon : Render::SkyFogMode::Off, fog.settings.color);
        _skyPass.Draw(frame, projection * view, Math::TranslationOf(cameraInView), sky.sun, sky.moon,
                      sky.settings);
    }
    else if (fogged)
    {
        _skyPass.SetFog(Render::SkyFogMode::Flat, fog.settings.color);
        _skyPass.Draw(frame, projection * view, Math::TranslationOf(cameraInView), Render::SkySun{},
                      Render::SkyMoon{}, Render::SkySettings{});
    }

    // See-through surfaces last, over the solid scene and the sky they show.
    if (!_blendedItems.empty())
    {
        ASSISI_PROFILE_GPU_PASS(frame.commandList, "draw-blended");
        const Render::MeshPass::SubmitStats blended = _meshPass.SubmitBlended(frame, _blendedItems);
        _lastDrawStats.drawnItems += blended.instances;
        _lastDrawStats.batches += blended.batches;
        _lastDrawStats.drawCalls += blended.drawCalls;
    }
    // Said once, because silently dropping the sky sends someone reading shader
    // code, and saying it every frame is its own kind of unreadable.
    if (sky.status == SkyStatus::MultipleDirectionalLights)
    {
        if (!_multipleSunsWarned)
        {
            Core::Log::Warn("SceneRenderer: more than one directional light in the scene; the sky is unsupported "
                            "there and is not drawn.");
            _multipleSunsWarned = true;
        }
    }
    else
    {
        _multipleSunsWarned = false;
    }
    WarnNewSkyWarnings(sky.warnings);

    // What the frame actually drew, on their own tracks. These are the numbers you
    // reach for the moment `draw-scene` moves: a jump in batches or draw calls says
    // the scene grew, a jump with flat counts says the cost is elsewhere.
    ASSISI_PROFILE_COUNTER("render/draw-calls", static_cast<double>(_lastDrawStats.drawCalls));
    ASSISI_PROFILE_COUNTER("render/batches", static_cast<double>(_lastDrawStats.batches));
    ASSISI_PROFILE_COUNTER("render/drawn-items", static_cast<double>(_lastDrawStats.drawnItems));
    ASSISI_PROFILE_COUNTER("render/culled-meshes", static_cast<double>(_lastDrawStats.culledMeshes));
    // Fog volumes on screen, which the lit pass and the sky test per pixel.
    ASSISI_PROFILE_COUNTER("render/fog-volumes", static_cast<double>(_fogVolumeFrame.volumes.size()));

    // The shadow pass on its own tracks, for the same reason: a jump in
    // `shadow-cascades` is explained by one of these or by none of them, and
    // "none of them" is the interesting answer.
    ASSISI_PROFILE_COUNTER("shadows/cascades", static_cast<double>(_lastShadowStats.cascades));
    // What the cadence did. On a fixed sun with a still camera and a still scene
    // `cascades` settles at zero and this at every fitted cascade, which is the
    // whole claim in two numbers — and a capture where `cascades` never settles
    // is a cadence invalidating something that did not change.
    ASSISI_PROFILE_COUNTER("shadows/cascades-kept", static_cast<double>(_lastShadowStats.cascadesKept));
    // Cascades a caster's motion dirtied, as against ones the camera or the sun
    // moved out from under. Those are different problems with different answers.
    ASSISI_PROFILE_COUNTER("shadows/cascades-by-motion", static_cast<double>(_sunCadence.Stats().dirtiedByMotion));
    ASSISI_PROFILE_COUNTER("shadows/instances", static_cast<double>(_lastShadowStats.instances));
    ASSISI_PROFILE_COUNTER("shadows/batches", static_cast<double>(_lastShadowStats.batches));
    // Reads zero for a scene with no cutout caster in it, which is what turns
    // "the alpha-tested variant costs nothing here" into something visible.
    ASSISI_PROFILE_COUNTER("shadows/masked-batches", static_cast<double>(_lastShadowStats.maskedBatches));
    ASSISI_PROFILE_COUNTER("shadows/culled", static_cast<double>(_lastShadowStats.culled));
    // Casters the gather never handed to a view at all, because nothing they
    // cast can reach the shadow distance. Walking content out past it moves this
    // and leaves every other shadow counter where it was.
    ASSISI_PROFILE_COUNTER("shadows/gather-culled", static_cast<double>(_shadowCasters.Result().culledEntities));
    // Caster-cascade pairs drawn a level coarser than on screen: where shadow
    // LOD saves vertices. Zero with it off, and in a scene with no LOD chains.
    ASSISI_PROFILE_COUNTER("shadows/lod-coarser", static_cast<double>(_shadowCasters.Result().coarserViews));

    // The local-light atlas on its own tracks. `dropped-by-cap` and `unserved`
    // answer different questions about a lamp with no shadow — the first is the
    // importance cap, the second is the atlas running out — and `occupancy` says
    // which of the two the scene is actually near.
    ASSISI_PROFILE_COUNTER("shadows/atlas-lights", static_cast<double>(_lastLocalShadowStats.lights));
    ASSISI_PROFILE_COUNTER("shadows/atlas-views", static_cast<double>(_lastLocalShadowStats.views));
    ASSISI_PROFILE_COUNTER("shadows/atlas-batches", static_cast<double>(_lastLocalShadowStats.batches));
    ASSISI_PROFILE_COUNTER("shadows/atlas-occupancy", static_cast<double>(_lastLocalShadowStats.occupancy));
    ASSISI_PROFILE_COUNTER("shadows/dropped-by-cap", static_cast<double>(_lastSelection.droppedByCap));
    ASSISI_PROFILE_COUNTER("shadows/atlas-unserved", static_cast<double>(_lastLocalShadowStats.unserved));

    // What the cache did, and the shape a missed invalidation takes in a trace:
    // `atlas-resting` should be every served light on a still scene and
    // `atlas-baked` zero, while a caster walking under a lamp shows two bakes at
    // the ends of the motion and none in between. A capture where `atlas-baked`
    // never settles is a cache invalidating something that did not move.
    ASSISI_PROFILE_COUNTER("shadows/atlas-resting", static_cast<double>(_lastLocalShadowStats.restingLights));
    ASSISI_PROFILE_COUNTER("shadows/atlas-baked", static_cast<double>(_lastLocalShadowStats.bakedFaces));
    ASSISI_PROFILE_COUNTER("shadows/atlas-mover-faces", static_cast<double>(_lastLocalShadowStats.moverFaces));
    ASSISI_PROFILE_COUNTER("shadows/atlas-waiting", static_cast<double>(_lastLocalShadowStats.deferredLights));
    ASSISI_PROFILE_COUNTER("shadows/atlas-movers", static_cast<double>(_lastLocalShadowStats.dynamicCasters));

    // A still sky bakes once and then reads zero here; a running clock shows a
    // bake each time the sun crosses the rebake tolerance. One that never
    // settles on a still sky is an input the comparison should not be seeing.
    const Render::SkyProbe::Stats &probeStats = _skyProbe.LastStats();
    ASSISI_PROFILE_COUNTER("sky-probe/baked", probeStats.bakedThisFrame ? 1.0 : 0.0);
    ASSISI_PROFILE_COUNTER("sky-probe/age", static_cast<double>(probeStats.ageFrames));
}

SpecularProbe SceneRenderer::UpdateSkyProbe(const Render::RenderFrame &frame, const SkyResolution &sky)
{
    // The probe is the sky's, so it exists exactly while a sky lights the
    // scene: a pinned ambient answers instead of the sky, and a scene with no
    // sky has nothing to capture. Released rather than kept in either case —
    // pay for what you place.
    const bool wanted = _environmentSettings.enabled && !_ambient.active && sky.status == SkyStatus::Ready &&
                        _skyProbe.IsValid() && _skyPass.IsValid();
    if (wanted && !_skyProbe.Configure(_environmentSettings))
    {
        Core::Log::Warn("SceneRenderer: sky reflections disabled (the probe targets failed to allocate).");
        _environmentSettings.enabled = false;
    }
    if (!wanted || !_environmentSettings.enabled)
    {
        _skyProbe.Release();
        _meshPass.SetEnvironment(nullptr);
        return SpecularProbe{};
    }

    Render::SkyProbeInputs inputs = Render::MakeSkyProbeInputs(sky.sun, sky.moon, sky.settings);
    // The clouds as drawn this frame, if they are: the bake shows them, so it
    // goes stale as they drift.
    if (_cloudPass.MapTexture() != nullptr)
    {
        inputs.clouds = Render::SkyProbeClouds{
            .settings = sky.clouds.settings,
            .across = glm::dvec2(_viewOrigin.x, _viewOrigin.z) - _cloudMotion.drift,
            .heightAbove = static_cast<float>(static_cast<double>(sky.clouds.settings.baseHeight) - _viewOrigin.y),
            .present = true};
    }
    if (_skyProbe.NeedsBake(inputs, sky.jumpSerial))
    {
        ASSISI_PROFILE_GPU_PASS(frame.commandList, "sky-probe");
        _skyProbe.Bake(frame.commandList, _skyPass, inputs, sky.jumpSerial);
    }
    else
    {
        _skyProbe.Keep();
    }

    if (!_skyProbe.IsReady())
    {
        _meshPass.SetEnvironment(nullptr);
        return SpecularProbe{};
    }
    _meshPass.SetEnvironment(_skyProbe.SpecularTexture());
    return SpecularProbe{.ready = true, .maxLod = _skyProbe.MaxLod()};
}

Render::CloudShadowConstants SceneRenderer::UpdateClouds(const Render::RenderFrame &frame, const SkyResolution &sky,
                                                         const Render::FogConstants &fog)
{
    // The textures exist exactly while the sky has clouds and the player draws
    // them. Released rather than kept otherwise — pay for what you place.
    const bool wanted = _cloudsEnabled && sky.status == SkyStatus::Ready && sky.clouds.present &&
                        _cloudPass.IsValid() && _skyPass.IsValid();
    if (wanted && !_cloudPass.Configure(_cloudQuality))
    {
        Core::Log::Warn("SceneRenderer: clouds disabled (their textures failed to allocate).");
        _cloudsEnabled = false;
    }
    if (!wanted || !_cloudsEnabled)
    {
        _cloudMotionTime.reset();
        _cloudPass.Release();
        _skyPass.SetClouds(nullptr);
        _meshPass.SetCloudShadow(nullptr);
        return Render::CloudShadowConstants{};
    }

    const Render::CloudSettings &settings = sky.clouds.settings;
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (_cloudMotionTime)
    {
        _cloudMotion = Render::AdvanceClouds(_cloudMotion, settings,
                                             std::chrono::duration<double>(now - *_cloudMotionTime).count());
    }
    _cloudMotionTime = now;

    // The fog's constants carry the sky's scatter whenever there is a sky,
    // with or without a Fog, and the clouds are seen against that sky.
    Render::CloudMapConstants map{.clouds = Render::PlaceClouds(settings, _viewOrigin, _cloudMotion,
                                                                _cloudQuality.steps, _cloudQuality.resolution),
                                  .fog = fog};
    Render::LightClouds(map.clouds, sky.sun, sky.moon, sky.settings);
    const Render::SkyAmbient clear = Render::AmbientFromSky(sky.sun, sky.moon, sky.settings);
    Render::AmbientLightClouds(map.clouds, glm::vec3(clear.sky), glm::vec3(clear.ground));

    const Render::CloudShadowPlacement placement =
        Render::PlaceCloudShadow(settings, _viewOrigin, sky.light.direction);
    const Render::CloudShadowMapConstants shadow{
        .clouds = map.clouds,
        .area = glm::vec4(placement.centre, Render::kCloudShadowWidth, static_cast<float>(Render::kCloudShadowSteps)),
        .light = glm::vec4(placement.light, 0.0f)};
    {
        ASSISI_PROFILE_GPU_PASS(frame.commandList, "clouds");
        _cloudPass.Render(frame.commandList, map, shadow);
    }
    // After Render: a reallocation swaps the textures, and each pass rebuilds
    // its binding set when it notices.
    _skyPass.SetClouds(_cloudPass.MapTexture());
    _meshPass.SetCloudShadow(_cloudPass.ShadowTexture());
    return Render::MakeCloudShadowConstants(settings, placement, static_cast<float>(_viewOrigin.y));
}

bool SceneRenderer::PrepareScreenOcclusion(const Render::RenderFrame &frame)
{
    // Released rather than kept while off — pay for what you place.
    const bool wanted = _ssaoSettings.enabled && _ssaoPass.IsValid() && _sceneDistancePass.IsValid();
    if (wanted)
    {
        if (!_sceneDistancePass.Configure(frame.width, frame.height, frame.depthTexture) ||
            !_ssaoPass.Configure(frame.width, frame.height, _sceneDistancePass.DistanceTexture()))
        {
            Core::Log::Warn("SceneRenderer: ambient occlusion disabled (its targets failed to allocate).");
            _ssaoSettings.enabled = false;
        }
    }
    if (!wanted || !_ssaoSettings.enabled)
    {
        _ssaoPass.Release();
        _sceneDistancePass.Release();
        _meshPass.SetAmbientOcclusion(nullptr);
        return false;
    }
    // After Configure: a reallocation swaps the texture handle, and the mesh
    // pass rebuilds its binding set when it notices.
    _meshPass.SetAmbientOcclusion(_ssaoPass.OcclusionTexture());
    return true;
}

void SceneRenderer::OnSceneReplaced()
{
    // Every kept shadow, because each holds depth of geometry that is gone: the
    // sun's cascades, and the atlas's tiles, which a light coming back at the
    // same index and pose would otherwise keep.
    ForgetKeptShadows();

    // The remembered LOD levels go with them, for the same reason and one more:
    // entity indices are reused across a load, so a level held from the old
    // scene would be read as the new occupant's.
    _lodSelector.Clear();

    // Reset rather than carried, because the incoming scene's clock starts its own
    // count and a serial that happened to match would skip the very Forget the
    // load needs.
    _jumpSerial = 0;
    _lastSky = SkyResolution{};

    // The incoming scene's clock counts its cuts from zero too, so a serial that
    // happened to match would keep a bake of the last level's sky.
    _skyProbe.Release();

    // The mobility table holds the old scene's handles, which the new scene's
    // entities reuse; and the mover bookmark is re-taken from the new scene on
    // its first frame rather than read from zero.
    _casterMobility.Clear();
    _moverTickPrimed = false;
}

void SceneRenderer::SetLodSettings(const Runtime::LodSettings &settings)
{
    if (settings == _lodSelector.Settings())
    {
        return;
    }
    _lodSelector.SetSettings(settings);
    ForgetKeptShadows();
}

void SceneRenderer::PinLod(ECS::Entity entity, int32_t level)
{
    _lodSelector.Pin(entity, level);
    ForgetKeptShadows();
}

LodView SceneRenderer::CameraLodView(const glm::mat4 &cameraWorld, const Camera &camera)
{
    return LodView{.cameraPosition = Math::TranslationOf(cameraWorld),
                   .tanHalfFovY = std::tan(glm::radians(camera.fovDegrees) * 0.5f)};
}

Runtime::LodReport SceneRenderer::LodReportFor(ECS::Entity entity, const Render::MeshBuffer &mesh,
                                               const ECS::WorldMatrix &worldMatrix) const
{
    const glm::mat4 drawn = ECS::MatrixFrom(worldMatrix, _viewOrigin);
    // Whichever way this instance's level was named, the report has to read as
    // named: a pin is the entity's own and never reaches the shared settings.
    Runtime::LodSettings settings = _lodSelector.Settings();
    settings.forcedLevel = _lodSelector.NamedLevelFor(entity);
    // A band nothing holds would be a switch point nothing switches at.
    if (!_lodSelector.HoldsDeadBand())
    {
        settings.hysteresis = 0.f;
    }

    // The drawn level rather than a fresh selection: this says what was drawn,
    // and the whole-mesh bounds are what selection measured, at every level.
    return DescribeLodSelection(mesh.Lods(), Geometry::TransformedBoundingSphere(mesh.LocalBounds(), drawn),
                                DrawnLodLevel(entity, mesh, worldMatrix), _lodSelector.View(), settings);
}

uint32_t SceneRenderer::DrawnLodLevel(ECS::Entity entity, const Render::MeshBuffer &mesh,
                                      const ECS::WorldMatrix &worldMatrix) const
{
    if (!GpuCullDraws())
    {
        return _lodSelector.Remembered(entity);
    }
    // Only a caster the shadow gathers reached was selected on the CPU this
    // frame; everything else the GPU measured unseen. With the dead band
    // released, a preview is that same measurement.
    return _lodSelector.Preview(
        entity, mesh.Lods(),
        Geometry::TransformedBoundingSphere(mesh.LocalBounds(), ECS::MatrixFrom(worldMatrix, _viewOrigin)));
}

} // namespace Assisi::Runtime
