/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Runtime/SceneRenderer.hpp>

#include <Assisi/Runtime/SkyResolve.hpp>

#include <cmath>
#include <cstdint>

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

// The depth prepass every screen-space feature reads, and the lit pass's vertex
// stage for drawing over it (see MeshPass::PreparePrepass). Loaded only once a
// frame asks for scene depth.
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

// The moon's albedo, stamped on its disk. An engine constant rather than a field
// on the Moon component: authoring a second moon's texture is a feature nobody
// has asked for, and a path in a level file is a path that can rot there.
constexpr const char *kMoonTexture = "textures/moon.jpg";
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
    // unshadowed rather than not at all.
    if (!_shadowDepthRenderer.Initialize(
            Render::ShadowDepthRenderer::InitParams{.device = _device,
                                                    .vertexShaderSpvPath = kShadowVertexShader,
                                                    .maskedVertexShaderSpvPath = kShadowMaskedVertexShader,
                                                    .maskedPixelShaderSpvPath = kShadowMaskedPixelShader,
                                                    .materialTable = params.materialTable,
                                                    .bindlessLayout = params.bindlessLayout,
                                                    .bindlessTable = params.bindlessTable}) ||
        !_shadowPass.Initialize(
            Render::ShadowPass::InitParams{.device = _device,
                                           .depthRenderer = &_shadowDepthRenderer,
                                           .restoreVertexShaderSpvPath = kShadowTileResetShader,
                                           .restorePixelShaderSpvPath = kShadowDepthRestoreShader}))
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

    if (!_meshPass.Initialize(Render::MeshPass::InitParams{.device = _device,
                                                           .framebufferInfo = params.framebufferInfo,
                                                           .vertexShaderSpvPath = kSceneVertexShader,
                                                           .pixelShaderSpvPath = kScenePixelShader,
                                                           .maskedPixelShaderSpvPath = kSceneMaskedPixelShader,
                                                           .depthVertexShaderSpvPath = kSceneDepthVertexShader,
                                                           .maskedDepthVertexShaderSpvPath =
                                                               kSceneMaskedDepthVertexShader,
                                                           .maskedDepthPixelShaderSpvPath =
                                                               kSceneMaskedDepthPixelShader,
                                                           .invariantVertexShaderSpvPath = kSceneInvariantVertexShader,
                                                           .clusterGrid = &_lighting.Grid(),
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
                                                         .moonTexturePath = kMoonTexture}))
    {
        Core::Log::Warn("SceneRenderer: sky unavailable (the sky pass failed to initialise).");
    }

    // Non-fatal as well: without it a sky lights the scene with the hemisphere
    // alone, which is what it did before there was a probe.
    if (!_skyProbe.Initialize(_device, kSkyPrefilterShader))
    {
        Core::Log::Warn("SceneRenderer: sky reflections unavailable (the prefilter failed to initialise).");
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

    return true;
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

void SceneRenderer::Render(const Render::RenderFrame &frame, ECS::Scene &scene, const Transform &cameraTransform,
                           const Camera &camera, uint64_t &propagationTick)
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

    const glm::mat4 projection =
        ProjectionMatrix(camera, AspectRatio(static_cast<int32_t>(frame.width), static_cast<int32_t>(frame.height)));
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

    // Before the mesh pass reads it, and off the same sky the frame is lit by.
    const SpecularProbe probe = UpdateSkyProbe(frame, sky);

    // Gathered but not yet uploaded: the local-light atlas decides which lights
    // hold tiles and stamps each winner's view index into the light record, and
    // that stamp has to happen before the lights reach the GPU.
    _lighting.Gather(scene, &sky.light);

    // Before either half draws, because both are invalidated against it and the
    // change ticks may only be consumed once a frame.
    UpdateShadowMovers(scene);

    // Before the shadow halves, because they select from it too: a caster's
    // shadow is drawn from the level the camera sees it at, or one coarser, and
    // all three passes measure with the one view set here. The GPU cull holds no
    // dead band, so while it draws the gathers hold none either.
    _lodSelector.SetHoldsDeadBand(!GpuCullDraws());
    _lodSelector.BeginFrame(CameraLodView(cameraTransform, camera));

    Render::MeshPass::ShadowFrameData shadows = RenderSunShadows(frame, scene, camera, view);
    RenderLocalShadows(frame, scene, camera, cameraTransform, shadows);
    // After both, because it reports on both — and outside them, because every
    // early return either of them takes is still a frame with an answer.
    BuildShadowDiagnostics();

    _lighting.Upload(frame.commandList, view);

    // Before the frame constants, which say whether the lit pass reads occlusion.
    const bool prepass = PrepareDepthPrepass(frame);
    const bool occlusion = PrepareScreenOcclusion(frame, prepass);

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
                                                                    .screenOcclusion = occlusion};
        _meshPass.UpdateFrameConstants(frame.commandList, frameConstants);
    }
    // With a prepass, this draws depth alone: the same extract, cull and sort as
    // ever, kept for the lit pass below to shade. Without, it is the lit pass.
    _lastDrawStats = DrawScene(DrawSceneParams{.scene = scene,
                                               .meshPass = _meshPass,
                                               .frame = frame,
                                               .view = view,
                                               .projection = projection,
                                               .nearZ = camera.nearZ,
                                               .farZ = camera.farZ,
                                               .frustumCulling = _frustumCulling,
                                               .sortDraws = _sortDraws,
                                               .gpuCulling = _gpuCulling,
                                               .culler = &_meshCuller,
                                               .cullBuilder = &_cullBuilder,
                                               .lodSelector = &_lodSelector,
                                               .stage = prepass ? Render::MeshPassStage::DepthPrepass
                                                                : Render::MeshPassStage::Lit});

    if (prepass)
    {
        if (occlusion)
        {
            _sceneDistancePass.Render(frame.commandList, projection);
            _ssaoPass.Render(
                frame.commandList,
                Render::SsaoPass::Frame{.projection = projection, .farZ = camera.farZ, .settings = _ssaoSettings});
        }

        // Under the name the lit pass has always been measured by, so a capture
        // with a prepass reads `draw-scene` against its own history and the
        // prepass, the distance and occlusion as the new rows beside it.
        ASSISI_PROFILE_GPU_PASS(frame.commandList, "draw-scene");
        _lastDrawStats.drawCalls += _meshPass.Redraw(frame, Render::MeshPassStage::LitAfterPrepass).drawCalls;
    }

    // The sky goes last, into whatever the geometry left at the depth clear. Both
    // halves of it come from the scene — the sun from a directional light, the
    // look from the Skybox component on that same entity — so a level authors its
    // own world and a light that moves takes the sky with it.
    if (sky.status == SkyStatus::Ready)
    {
        _skyPass.Draw(frame, projection * view, glm::vec3(cameraTransform.worldMatrix[3]), sky.sun, sky.moon,
                      sky.settings);
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
    // What the cadence did. On a fixed sun with a still camera and a still scene
    // `cascades` settles at zero and this at every fitted cascade, which is the
    // whole claim in two numbers — and a capture where `cascades` never settles
    // is a cadence invalidating something that did not change.
    ASSISI_PROFILE_COUNTER("shadows/cascades-kept", static_cast<double>(_lastShadowStats.cascadesKept));
    // Cascades a caster's motion dirtied, as against ones the camera or the sun
    // moved out from under. Those are different problems with different answers.
    ASSISI_PROFILE_COUNTER("shadows/cascades-by-motion",
                           static_cast<double>(_sunCadence.Stats().dirtiedByMotion));
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

    const Render::SkyProbeInputs inputs = Render::MakeSkyProbeInputs(sky.sun, sky.moon, sky.settings);
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

bool SceneRenderer::PrepareDepthPrepass(const Render::RenderFrame &frame)
{
    if (_prepassFailed || frame.depthTexture == nullptr)
    {
        return false;
    }
    if (!_meshPass.PreparePrepass())
    {
        Core::Log::Warn("SceneRenderer: the depth prepass failed to build; the scene is lit without one.");
        _prepassFailed = true;
        return false;
    }
    return true;
}

bool SceneRenderer::PrepareScreenOcclusion(const Render::RenderFrame &frame, bool prepass)
{
    // Released rather than kept while off — pay for what you place. Occlusion
    // reads the prepass's depth, so a frame without one has nothing to read.
    const bool wanted = _ssaoSettings.enabled && _ssaoPass.IsValid() && _sceneDistancePass.IsValid() && prepass;
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
    // Both selectors, because both hold state keyed to entities that are gone: the
    // sun's cascades hold depth of that geometry, and the atlas holds tiles for
    // lights that no longer exist.
    _sunCadence.Forget();
    _localShadowSelector.Forget();

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

LodView SceneRenderer::CameraLodView(const Transform &cameraTransform, const Camera &camera)
{
    return LodView{.cameraPosition = glm::vec3(cameraTransform.worldMatrix[3]),
                   .tanHalfFovY = std::tan(glm::radians(camera.fovDegrees) * 0.5f)};
}

Runtime::LodReport SceneRenderer::LodReportFor(ECS::Entity entity, const Render::MeshBuffer &mesh,
                                               const glm::mat4 &worldMatrix) const
{
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
    return DescribeLodSelection(mesh.Lods(), Geometry::TransformedBoundingSphere(mesh.LocalBounds(), worldMatrix),
                                DrawnLodLevel(entity, mesh, worldMatrix), _lodSelector.View(), settings);
}

uint32_t SceneRenderer::DrawnLodLevel(ECS::Entity entity, const Render::MeshBuffer &mesh,
                                      const glm::mat4 &worldMatrix) const
{
    if (!GpuCullDraws())
    {
        return _lodSelector.Remembered(entity);
    }
    // Only a caster the shadow gathers reached was selected on the CPU this
    // frame; everything else the GPU measured unseen. With the dead band
    // released, a preview is that same measurement.
    return _lodSelector.Preview(entity, mesh.Lods(),
                                Geometry::TransformedBoundingSphere(mesh.LocalBounds(), worldMatrix));
}

} // namespace Assisi::Runtime
