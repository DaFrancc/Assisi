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
#include <Assisi/Render/Frustum.hpp>
#include <Assisi/Render/GpuMarker.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/FogVolumeGather.hpp>
#include <Assisi/Runtime/PostProcessResolve.hpp>
#include <Assisi/Runtime/Renderer.hpp>
#include <Assisi/Runtime/ScenePasses.generated.hpp>
#include <Assisi/Runtime/WaterGather.hpp>
#include <Assisi/Runtime/WindResolve.hpp>

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

// The analytic sky (see Render::SkyPass). A fullscreen triangle at the far
// plane, so its vertex stage is its own rather than the shared one: that one
// emits depth 0, and the sky has to land on the 1.0 the depth clear left.
constexpr const char *kSkyVertexShader = "shaders/sky.vert.spv";
constexpr const char *kSkyPixelShader = "shaders/sky.frag.spv";

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

SceneRenderer::SceneRenderer() : _graph(std::make_unique<Passes::ScenePassGraph>()) {}

SceneRenderer::~SceneRenderer() = default;

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

    // A failure is non-fatal — the scene renders unshadowed rather than not at
    // all: both shadow passes then fail to start, and say so.
    if (!_shadowDepthRenderer.Initialize(
            Render::ShadowDepthRenderer::InitParams{.device = _device,
                                                    .vertexShaderSpvPath = kShadowVertexShader,
                                                    .maskedVertexShaderSpvPath = kShadowMaskedVertexShader,
                                                    .maskedPixelShaderSpvPath = kShadowMaskedPixelShader,
                                                    .materialTable = params.materialTable,
                                                    .bindlessLayout = params.bindlessLayout,
                                                    .bindlessTable = params.bindlessTable}))
    {
        Core::Log::Warn("SceneRenderer: shadows unavailable (the shadow depth renderer failed to initialise).");
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

    // The passes' own pieces; each that fails to start says so and stays off.
    _graph->Initialize(Passes::ScenePassInit{.device = _device,
                                             .shadowDepthRenderer = &_shadowDepthRenderer,
                                             .meshFrameLayout = _meshPass.FrameBindingLayout()},
                       params.framebufferInfo);
    return true;
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
    _graph->OnTargetsChanged(framebufferInfo);
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
    const bool severalWinds = ResolveWind(scene).count > 1;
    if (severalWinds && !_severalWindsWarned)
    {
        Core::Log::Warn("SceneRenderer: more than one Wind in the scene; the first found is used.");
    }
    _severalWindsWarned = severalWinds;

    // Refresh world matrices before anything reads them (view matrix, draw). Only
    // entities whose transform changed since last frame are recomputed; the tick
    // bookmark carries that across frames — and belongs to the scene, not to us
    // (see the header).
    {
        ASSISI_PROFILE_SCOPE("propagate-transforms");
        propagationTick = ECS::PropagateTransforms(scene, propagationTick);
    }

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

    const bool fogged = _settings.fogEnabled && fog.present;
    const Render::FogConstants fogConstants =
        FrameFog(fog, fogged, sky, static_cast<float>(_viewOrigin.y), camera.farZ);

    // The placed fog volumes, which the player's fog setting turns off with
    // the level's own fog.
    _fogVolumeFrame.Clear();
    if (_settings.fogEnabled)
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
    // Uploaded now, ahead of every pass: only the lit pass and the sky read it.
    _fogVolumeBuffers.Upload(frame.commandList, _fogVolumeFrame);

    // The frame's lights. The shadow passes choose from them, and the light
    // upload stamps each one with where its shadow views landed.
    _lighting.Gather(scene, &sky.light, LightFrame{.view = _viewOrigin, .shadow = _shadowOrigin});

    // Before either shadow half, because both are invalidated against it and the
    // change ticks may only be consumed once a frame.
    UpdateShadowMovers(scene);

    // Before the shadow halves, because they select from it too: a caster's
    // shadow is drawn from the level the camera sees it at, or one coarser, and
    // all three passes measure with the one view set here. The GPU cull holds no
    // dead band, so while it draws the gathers hold none either.
    _lodSelector.SetHoldsDeadBand(!GpuCullDraws());
    _lodSelector.BeginFrame(CameraLodView(cameraInView, camera));

    // Opens the frame's shadow view table, which every kind of shadow map
    // appends to, so it belongs to none of them.
    _shadowDepthRenderer.BeginFrame(Math::TranslationOf(cameraInShadow));

    // The water in view, before the scene's distance is prepared, which it reads.
    _waterFrame.Clear();
    if (_settings.waterEnabled)
    {
        ASSISI_PROFILE_SCOPE("water-gather");
        const Render::Frustum frustum = Render::Frustum::FromViewProjection(projection * view);
        GatherWater(scene, WaterGatherView{.camera = _viewOrigin, .frustum = &frustum, .drawDistance = camera.farZ},
                    _waterFrame);
    }
    const bool waterReadsDistance = _settings.waterEnabled && !_waterFrame.bodies.empty();

    // Before the frame constants, which say whether the lit pass reads occlusion.
    const ScreenOcclusion occlusion = PrepareScreenOcclusion(frame, waterReadsDistance);

    // The passes, in the order their declarations fix: each hands what it makes
    // to the ones after it.
    const Render::RenderTarget target{.framebuffer = frame.framebuffer,
                                      .colorTexture = frame.colorTexture,
                                      .depthTexture = frame.depthTexture,
                                      .width = frame.width,
                                      .height = frame.height};
    _sceneColor.target = target;
    _sceneDepth.target = target;
    Passes::SceneImports imports{.meshPass = _meshPass,
                                 .skyPass = _skyPass,
                                 .lightingSystem = _lighting,
                                 .lodSelector = _lodSelector,
                                 .shadowDepthRenderer = _shadowDepthRenderer,
                                 .shadowCasterMobility = _casterMobility,
                                 .meshCuller = _meshCuller,
                                 .sceneDistancePass = _sceneDistancePass,
                                 .ssaoPass = _ssaoPass,
                                 .sceneColor = _sceneColor,
                                 .sceneDepth = _sceneDepth,
                                 .drawStats = _lastDrawStats};
    const Passes::SceneFrame sceneFrame{.scene = scene,
                                        .sky = sky,
                                        .settings = _settings,
                                        .viewOrigin = _viewOrigin,
                                        .shadowOrigin = _shadowOrigin,
                                        .shadowSpots = _lighting.ShadowCastingSpotLights(),
                                        .shadowPoints = _lighting.ShadowCastingPointLights(),
                                        .dynamicCasters = _dynamicCasters,
                                        .casterInvalidations = _casterInvalidations,
                                        .water = &_waterFrame,
                                        .waterSeconds = _waterSeconds,
                                        .occlusion = occlusion,
                                        .fog = fogConstants,
                                        .cameraInShadow = cameraInShadow,
                                        .shadowSun = _lighting.ShadowCastingSun(),
                                        .camera = camera,
                                        .fogVolumeLanes = fogVolumeLanes,
                                        .fogColor = fog.settings.color,
                                        .width = frame.width,
                                        .height = frame.height,
                                        .shadowFrameIndex = _shadowFrameIndex,
                                        .fogged = fogged,
                                        .skyAvailable = _skyPass.IsValid(),
                                        .waterReadsDistance = waterReadsDistance && _sceneDistancePass.DistanceTexture() != nullptr};
    const Passes::ViewFrame viewFrame{.projection = projection,
                                      .view = view,
                                      .cameraInView = cameraInView,
                                      .viewerInstance = ViewerInstance(scene, _viewingCamera)};
    _graph->Run(imports, frame.commandList, sceneFrame, viewFrame);

    // After both shadow halves, because it reports on both — and outside them,
    // because every early return either of them takes is still a frame with an
    // answer.
    BuildShadowDiagnostics();

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
    CountFrame();
}

void SceneRenderer::CountFrame() const
{
    const Passes::SunShadowPass &sun = _graph->passes.sunShadow;
    const Passes::ShadowAtlasPass &atlas = _graph->passes.shadowAtlas;

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
    ASSISI_PROFILE_COUNTER("shadows/cascades", static_cast<double>(sun.stats.cascades));
    // What the cadence did. On a fixed sun with a still camera and a still scene
    // `cascades` settles at zero and this at every fitted cascade, which is the
    // whole claim in two numbers — and a capture where `cascades` never settles
    // is a cadence invalidating something that did not change.
    ASSISI_PROFILE_COUNTER("shadows/cascades-kept", static_cast<double>(sun.stats.cascadesKept));
    // Cascades a caster's motion dirtied, as against ones the camera or the sun
    // moved out from under. Those are different problems with different answers.
    ASSISI_PROFILE_COUNTER("shadows/cascades-by-motion", static_cast<double>(sun.cadence.Stats().dirtiedByMotion));
    ASSISI_PROFILE_COUNTER("shadows/instances", static_cast<double>(sun.stats.instances));
    ASSISI_PROFILE_COUNTER("shadows/batches", static_cast<double>(sun.stats.batches));
    // Reads zero for a scene with no cutout caster in it, which is what turns
    // "the alpha-tested variant costs nothing here" into something visible.
    ASSISI_PROFILE_COUNTER("shadows/masked-batches", static_cast<double>(sun.stats.maskedBatches));
    ASSISI_PROFILE_COUNTER("shadows/culled", static_cast<double>(sun.stats.culled));
    // Casters the gather never handed to a view at all, because nothing they
    // cast can reach the shadow distance. Walking content out past it moves this
    // and leaves every other shadow counter where it was.
    ASSISI_PROFILE_COUNTER("shadows/gather-culled", static_cast<double>(sun.casters.Result().culledEntities));
    // Caster-cascade pairs drawn a level coarser than on screen: where shadow
    // LOD saves vertices. Zero with it off, and in a scene with no LOD chains.
    ASSISI_PROFILE_COUNTER("shadows/lod-coarser", static_cast<double>(sun.casters.Result().coarserViews));

    // The local-light atlas on its own tracks. `dropped-by-cap` and `unserved`
    // answer different questions about a lamp with no shadow — the first is the
    // importance cap, the second is the atlas running out — and `occupancy` says
    // which of the two the scene is actually near.
    ASSISI_PROFILE_COUNTER("shadows/atlas-lights", static_cast<double>(atlas.stats.lights));
    ASSISI_PROFILE_COUNTER("shadows/atlas-views", static_cast<double>(atlas.stats.views));
    ASSISI_PROFILE_COUNTER("shadows/atlas-batches", static_cast<double>(atlas.stats.batches));
    ASSISI_PROFILE_COUNTER("shadows/atlas-occupancy", static_cast<double>(atlas.stats.occupancy));
    ASSISI_PROFILE_COUNTER("shadows/dropped-by-cap", static_cast<double>(atlas.selection.droppedByCap));
    ASSISI_PROFILE_COUNTER("shadows/atlas-unserved", static_cast<double>(atlas.stats.unserved));

    // What the cache did, and the shape a missed invalidation takes in a trace:
    // `atlas-resting` should be every served light on a still scene and
    // `atlas-baked` zero, while a caster walking under a lamp shows two bakes at
    // the ends of the motion and none in between. A capture where `atlas-baked`
    // never settles is a cache invalidating something that did not move.
    ASSISI_PROFILE_COUNTER("shadows/atlas-resting", static_cast<double>(atlas.stats.restingLights));
    ASSISI_PROFILE_COUNTER("shadows/atlas-baked", static_cast<double>(atlas.stats.bakedFaces));
    ASSISI_PROFILE_COUNTER("shadows/atlas-mover-faces", static_cast<double>(atlas.stats.moverFaces));
    ASSISI_PROFILE_COUNTER("shadows/atlas-waiting", static_cast<double>(atlas.stats.deferredLights));
    ASSISI_PROFILE_COUNTER("shadows/atlas-movers", static_cast<double>(atlas.stats.dynamicCasters));

    // A still sky bakes once and then reads zero here; a running clock shows a
    // bake each time the sun crosses the rebake tolerance. One that never
    // settles on a still sky is an input the comparison should not be seeing.
    const Render::SkyProbe::Stats &probeStats = _graph->passes.skyProbe.probe.LastStats();
    ASSISI_PROFILE_COUNTER("sky-probe/baked", probeStats.bakedThisFrame ? 1.0 : 0.0);
    ASSISI_PROFILE_COUNTER("sky-probe/age", static_cast<double>(probeStats.ageFrames));
}

ScreenOcclusion SceneRenderer::PrepareScreenOcclusion(const Render::RenderFrame &frame, bool waterReadsDistance)
{
    // Released rather than kept while off — pay for what you place.
    const bool occlusionWanted = _settings.ssao.enabled && _ssaoPass.IsValid() && _sceneDistancePass.IsValid();
    const bool distanceWanted = (occlusionWanted || waterReadsDistance) && _sceneDistancePass.IsValid();
    const bool distanceHeld =
        distanceWanted && _sceneDistancePass.Configure(frame.width, frame.height, frame.depthTexture);
    if (distanceWanted && !distanceHeld)
    {
        Core::Log::Warn("SceneRenderer: the scene's distance failed to allocate; ambient occlusion is off and water "
                        "is not drawn this frame.");
    }
    if (!distanceHeld)
    {
        _sceneDistancePass.Release();
    }
    if (occlusionWanted &&
        (!distanceHeld || !_ssaoPass.Configure(frame.width, frame.height, _sceneDistancePass.DistanceTexture())))
    {
        Core::Log::Warn("SceneRenderer: ambient occlusion disabled (its targets failed to allocate).");
        _settings.ssao.enabled = false;
    }
    if (!occlusionWanted || !_settings.ssao.enabled)
    {
        _ssaoPass.Release();
        return ScreenOcclusion{};
    }
    // After Configure: a reallocation swaps the texture handle, and the mesh
    // pass rebuilds its binding set when it notices.
    return ScreenOcclusion{.texture = _ssaoPass.OcclusionTexture()};
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

    // The water's wave textures are kept by entity too.
    _graph->passes.waterWaves.renderer.Release();

    // Reset rather than carried, because the incoming scene's clock starts its own
    // count and a serial that happened to match would skip the very Forget the
    // load needs.
    _graph->passes.sunShadow.jumpSerial = 0;
    _lastSky = SkyResolution{};

    // The incoming scene's clock counts its cuts from zero too, so a serial that
    // happened to match would keep a bake of the last level's sky.
    _graph->passes.skyProbe.probe.Release();

    // The mobility table holds the old scene's handles, which the new scene's
    // entities reuse; and the mover bookmark is re-taken from the new scene on
    // its first frame rather than read from zero.
    _casterMobility.Clear();
    _moverTickPrimed = false;
}

void SceneRenderer::SetEnvironmentSettings(const Render::EnvironmentSettings &settings)
{
    _settings.environment = Render::Sanitized(settings);
    _graph->passes.skyProbe.failed = false;
}

void SceneRenderer::SetCloudsEnabled(bool enabled)
{
    _settings.cloudsEnabled = enabled;
    _graph->passes.cloudMap.failed = false;
}

void SceneRenderer::SetCloudQuality(const Render::CloudQuality &quality)
{
    _settings.cloudQuality = Render::Sanitized(quality);
    _graph->passes.cloudMap.failed = false;
}

void SceneRenderer::SetShadowSettings(const Render::ShadowSettings &settings)
{
    _settings.shadows = settings;
    _graph->passes.sunShadow.failed = false;
    _graph->passes.shadowAtlas.failed = false;
}

void SceneRenderer::SetShadowDiagnosticsEnabled(bool enabled)
{
    _shadowDiagnosticsEnabled = enabled;
    // The sun's half is gathered inside its pass, where the draw list it reads
    // lives and dies within the call — so the pass is told directly rather than
    // asked afterwards.
    _graph->passes.sunShadow.shadowPass.SetCascadeCountsEnabled(enabled);
    if (!enabled)
    {
        _shadowDiagnostics.Clear();
    }
}

const Render::SkyProbe &SceneRenderer::SkyProbe() const
{
    return _graph->passes.skyProbe.probe;
}

Render::ShadowPass::Stats SceneRenderer::LastShadowStats() const
{
    return _graph->passes.sunShadow.stats;
}

std::uint32_t SceneRenderer::LastShadowLodCoarser() const
{
    return _graph->passes.sunShadow.casters.Result().coarserViews;
}

Render::LocalShadowPass::Stats SceneRenderer::LastLocalShadowStats() const
{
    return _graph->passes.shadowAtlas.stats;
}

std::span<const Render::LocalShadowCache::Residency> SceneRenderer::CachedShadowTiles() const
{
    return _graph->passes.shadowAtlas.atlas.CachedTiles();
}

uint32_t SceneRenderer::LastShadowDroppedByCap() const
{
    return _graph->passes.shadowAtlas.selection.droppedByCap;
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
