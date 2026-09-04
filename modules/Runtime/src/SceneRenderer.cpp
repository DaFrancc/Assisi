/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Runtime/SceneRenderer.hpp>

#include <Assisi/Runtime/SkyResolve.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
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

// The analytic sky (see Render::SkyPass). A fullscreen triangle at the far
// plane, so its vertex stage is its own rather than the shared one: that one
// emits depth 0, and the sky has to land on the 1.0 the depth clear left.
constexpr const char *kSkyVertexShader = "shaders/sky.vert.spv";
constexpr const char *kSkyPixelShader = "shaders/sky.frag.spv";

// Selection-outline shaders (screen-space edge detect; see Render::OutlinePass):
// a mask pass that stamps the silhouette, and a fullscreen edge pass that paints
// the orange border. Editor-only, so they live under editor/shaders/ — except the
// edge pass's vertex stage, which reuses the shared fullscreen-triangle shader.
constexpr const char *kOutlineMaskVertexShader = "editor/shaders/outline_mask.vert.spv";
constexpr const char *kOutlineMaskPixelShader = "editor/shaders/outline_mask.frag.spv";
constexpr const char *kOutlineEdgeVertexShader = "shaders/fullscreen.vert.spv";
constexpr const char *kOutlineEdgePixelShader = "editor/shaders/outline_edge.frag.spv";

// Generic overlay-line renderer (see Render::LinePass). Editor-only in practice
// (collider wireframes), so the shaders live under editor/shaders/.
constexpr const char *kLineVertexShader = "editor/shaders/line.vert.spv";
constexpr const char *kLinePixelShader = "editor/shaders/line.frag.spv";

// Editor entity-icon billboard (see Render::IconPass), editor-only. The icon image
// is authored content dropped at this virtual path; until it exists the pass shows
// a magenta placeholder.
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

bool SceneRenderer::Initialize(const InitParams &params)
{
    _device = params.device;
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
            Render::ShadowPass::InitParams{.device = _device, .depthRenderer = &_shadowDepthRenderer}))
    {
        Core::Log::Warn("SceneRenderer: sun shadows unavailable (the depth pass failed to initialise).");
    }
    // The same renderer, so every shadow view of the frame lands in one table
    // whichever kind of map produced it.
    if (!_localShadowPass.Initialize(
            Render::LocalShadowPass::InitParams{.device = _device, .depthRenderer = &_shadowDepthRenderer}))
    {
        Core::Log::Warn("SceneRenderer: local-light shadows unavailable (the depth pass failed to initialise).");
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
    _meshPass.SetShadowAtlas(_localShadowPass.AtlasTexture());

    // The sky. Non-fatal: without it the scene target keeps its clear colour,
    // which is what every scene looked like before there was a sky at all.
    if (!_skyPass.Initialize(Render::SkyPass::InitParams{.device = _device,
                                                         .framebufferInfo = params.framebufferInfo,
                                                         .vertexShaderSpvPath = kSkyVertexShader,
                                                         .pixelShaderSpvPath = kSkyPixelShader}))
    {
        Core::Log::Warn("SceneRenderer: sky unavailable (the sky pass failed to initialise).");
    }

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
    // The sky targets the scene format, not the overlay one — it holds radiance
    // and is drawn before the tone map, like the geometry it sits behind.
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

    // Gathered but not yet uploaded: the local-light atlas decides which lights
    // hold tiles and stamps each winner's view index into the light record, and
    // that stamp has to happen before the lights reach the GPU.
    _lighting.Gather(scene);

    Render::MeshPass::ShadowFrameData shadows = RenderSunShadows(frame, scene, camera, view);
    RenderLocalShadows(frame, scene, camera, cameraTransform, shadows);

    _lighting.Upload(frame.commandList, view);

    // The scene's sky, which both lights the geometry and is drawn behind it.
    // Resolved before the geometry because the indirect term comes off it: a
    // surface in shadow is still under the sky, and what saves it from reading
    // as a hole is the sky's own colour arriving here.
    const SkyResolution sky = ResolveSky(scene);

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
                                                                    .indirect = ResolveIndirect(sky, _ambient),
                                                                    .shadows = shadows};
        _meshPass.UpdateFrameConstants(frame.commandList, frameConstants);
    }
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
                                               .cullBuilder = &_cullBuilder});

    // The sky goes last, into whatever the geometry left at the depth clear. Both
    // halves of it come from the scene — the sun from a directional light, the
    // look from the Skybox component on that same entity — so a level authors its
    // own world and a light that moves takes the sky with it.
    if (sky.status == SkyStatus::Ready)
    {
        _skyPass.Draw(frame, projection * view, glm::vec3(cameraTransform.worldMatrix[3]), sky.sun, sky.settings);
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
    ASSISI_PROFILE_COUNTER("shadows/atlas-copied", static_cast<double>(_lastLocalShadowStats.copiedFaces));
    ASSISI_PROFILE_COUNTER("shadows/atlas-waiting", static_cast<double>(_lastLocalShadowStats.deferredLights));
    ASSISI_PROFILE_COUNTER("shadows/atlas-movers", static_cast<double>(_lastLocalShadowStats.dynamicCasters));
}

Render::MeshPass::ShadowFrameData SceneRenderer::RenderSunShadows(const Render::RenderFrame &frame, ECS::Scene &scene,
                                                                  const Camera &camera, const glm::mat4 &view)
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

    // After the fit, because the gather classifies against it: the sun's shadow
    // distance lives in the cascades' own extent, and a caster that reaches no
    // cascade is one no view wants. The fit reads nothing from the gather, so
    // this is the order it always could have been in.
    //
    // One volume per cascade rather than one around all of them: the bit a
    // caster earns here is the cascade it draws into, so the classification is
    // what the per-view sweep used to redo, and a caster inside the shadow
    // distance but in only one cascade now walks only that one.
    std::array<Geometry::BoundingSphere, Render::kMaxShadowCascades> cascadeVolumes{};
    const std::uint32_t volumeCount = Render::CascadeVolumeBounds(_cascadeFit, cascadeVolumes);
    GatherShadowCasters(scene, sun->direction,
                        std::span<const Geometry::BoundingSphere>(cascadeVolumes.data(), volumeCount), _shadowCasters);

    _lastShadowStats = _shadowPass.Render(frame.commandList, _cascadeFit, _shadowCasters.casters);

    shadows.fit = &_cascadeFit;
    shadows.settings = _shadowPass.Settings();
    shadows.sunLightIndex = sun->index;
    shadows.debugView = _shadowDebugView;
    return shadows;
}

float SceneRenderer::LocalLightScreenCoverage(const glm::vec3 &position, float range, const glm::vec3 &cameraPosition,
                                              float tanHalfFovY)
{
    if (!(range > 0.f) || !(tanHalfFovY > 0.f))
    {
        return 0.f;
    }
    const glm::vec3 toLight = position - cameraPosition;
    const float distance = std::sqrt(glm::dot(toLight, toLight));
    // Inside the light's own volume it fills the view, and there is nothing
    // further to say: the ratio past that point grows without bound and would
    // make one lamp's score swamp every other light in the level.
    if (distance <= range)
    {
        return 1.f;
    }
    // Half the screen's height spans `distance * tanHalfFovY` at the light's
    // distance, and the light spans `range` — so this is the light's diameter
    // over the view's height.
    return std::min(range / (distance * tanHalfFovY), 1.f);
}

void SceneRenderer::RenderLocalShadows(const Render::RenderFrame &frame, ECS::Scene &scene, const Camera &camera,
                                       const Transform &cameraTransform, Render::MeshPass::ShadowFrameData &shadows)
{
    _lastLocalShadowStats = Render::LocalShadowPass::Stats{};
    _lastSelection.Clear();

    const std::span<const LightingSystem::LocalLight> spots = _lighting.ShadowCastingSpotLights();
    const std::span<const LightingSystem::LocalLight> points = _lighting.ShadowCastingPointLights();

    // No shadow-casting local light means no allocation and no pass — the same
    // blank-scene rule the cascades keep, and the reason `active` is a scene
    // fact rather than a settings one.
    const bool active = _shadowSettings.local.enabled && !(spots.empty() && points.empty());
    if (!_localShadowPass.Configure(_shadowSettings.local, active))
    {
        Core::Log::Warn("SceneRenderer: local-light shadows disabled (the atlas failed to rebuild).");
        _shadowSettings.local.enabled = false;
    }
    // After Configure: a reallocation swaps the texture handle, and the mesh
    // pass rebuilds its binding set when it notices.
    _meshPass.SetShadowAtlas(_localShadowPass.AtlasTexture());
    _meshPass.SetShadowViewTable(_shadowDepthRenderer.ViewTable());

    if (!_localShadowPass.IsActive())
    {
        // The selector's memory is of an atlas that is no longer there, so the
        // next frame that turns shadows back on takes its demand outright rather
        // than resisting a change from a size class that no longer exists. The
        // mobility table goes with it: the poses it holds are poses of a kept
        // layer that no longer exists either.
        _localShadowSelector.Forget();
        _casterMobility.Clear();
        // Left where it is deliberately, so the frame that turns shadows back on
        // sees everything written while they were off as having moved.
        return;
    }

    // The sun opens the frame's view table when it draws. With no sun, nothing
    // has, and the local views would append to last frame's.
    if (!_shadowPass.IsActive())
    {
        _shadowDepthRenderer.BeginFrame();
    }

    const glm::vec3 cameraPosition = glm::vec3(cameraTransform.worldMatrix[3]);
    const float tanHalfFovY = std::tan(glm::radians(camera.fovDegrees) * 0.5f);

    _localCandidates.clear();
    _localCandidates.reserve(spots.size() + points.size());
    const auto addCandidate = [&](const LightingSystem::LocalLight &light, Render::LocalLightKind kind)
                              {
                                  _localCandidates.push_back(Render::LocalShadowCandidate{
                .kind = kind,
                .lightIndex = light.index,
                .screenCoverage = LocalLightScreenCoverage(light.position, light.range, cameraPosition, tanHalfFovY),
                .intensity = light.intensity,
                .priority = light.shadowPriority,
                .pinned = light.shadowAlwaysOn,
                // Every gathered light is a candidate. A frustum test here would
                // drop the shadows of lights just off screen, and their casters
                // are exactly the ones whose shadows reach into it.
                .visible = true});
                              };
    for (const LightingSystem::LocalLight &light : spots)
    {
        addCandidate(light, Render::LocalLightKind::Spot);
    }
    for (const LightingSystem::LocalLight &light : points)
    {
        addCandidate(light, Render::LocalLightKind::Point);
    }

    _localShadowSelector.Select(_localCandidates, _shadowSettings.local, _shadowSettings.selection, _lastSelection);
    if (_lastSelection.lights.empty())
    {
        return;
    }

    // Back to the light each winner names, for the geometry its views are built
    // from. The selection carries scores and classes; it deliberately does not
    // carry positions, so that it can be tested without a scene.
    _localRequests.clear();
    _localLightVolumes.clear();
    _localRequests.reserve(_lastSelection.lights.size());
    _localLightVolumes.reserve(_lastSelection.lights.size());
    for (const Render::LocalShadowAssignment &winner : _lastSelection.lights)
    {
        const std::span<const LightingSystem::LocalLight> &pool =
            winner.kind == Render::LocalLightKind::Point ? points : spots;
        const LightingSystem::LocalLight *light = nullptr;
        for (const LightingSystem::LocalLight &candidate : pool)
        {
            if (candidate.index == winner.lightIndex)
            {
                light = &candidate;
                break;
            }
        }
        if (light == nullptr)
        {
            continue;
        }
        _localRequests.push_back(Render::LocalShadowRequest{
                .kind = winner.kind,
                .lightIndex = light->index,
                .pose = Render::LocalShadowLightPose{.position = light->position,
                                                     .direction = light->direction,
                                                     .range = light->range,
                                                     .outerAngleDegrees = light->outerAngleDegrees},
                .sizeClass = winner.sizeClass});
        // The light's whole reach, whatever shape it lights within it. A spot's
        // cone would be a tighter volume, but the cone test belongs per face
        // where the frustum already makes it — bounding the sphere here keeps
        // one gather serving both kinds.
        _localLightVolumes.push_back(Geometry::BoundingSphere{light->position, light->range});
    }

    // What moved since the last frame, taken from the Transform pool's change
    // ticks rather than by asking every caster. This is the whole invalidation
    // input, and on a still frame it is empty.
    ++_shadowFrameIndex;
    _movedEntities.clear();
    scene.ChangedSince<Transform>(_lastMoverTick, _movedEntities);
    _lastMoverTick = scene.CurrentChangeTick();
    GatherShadowMovers(scene, _movedEntities, _movedCasters);
    _casterMobility.Update(_shadowFrameIndex, _shadowSettings.local.cache.promoteStillFrames, _movedCasters,
                           _dynamicCasters, _casterInvalidations);

    Render::LocalShadowPass::Frame shadowFrame{.requests = _localRequests,
                                               .casters = {},
                                               .casterIndex = &_localCasterIndex,
                                               .movers = _dynamicCasters,
                                               .invalidations = _casterInvalidations,
                                               .frameIndex = _shadowFrameIndex};

    // Asked rather than guessed. A frame with nothing to draw skips the gather —
    // the per-object preparation the cost model says dominates — and the atlas
    // keeps what it holds. What needs drawing is *not* "did a caster move": a
    // light that moved, or came back from not casting, needs its still layer
    // baked out of casters that are standing perfectly still. Deciding that here
    // would be a second answer to a question the cache already answers, and the
    // two disagreeing means baking a tile from an empty caster list — which
    // blanks it and leaves that one light with no shadow until something else
    // happens to dirty it again.
    if (_localShadowPass.PlanFrame(shadowFrame))
    {
        GatherLocalShadowCasters(scene, _localLightVolumes, _casterMobility, _localShadowCasters, _localCasterIndex);
        shadowFrame.casters = _localShadowCasters.casters;
    }
    else
    {
        // The index still has to describe every request, because the composite
        // walks a row per served face whether or not it finds anything in it.
        _localShadowCasters.casters.clear();
        _localCasterIndex.Clear();
        _localCasterIndex.start.assign(_localRequests.size() + 1u, 0u);
    }

    _lastLocalShadowStats = _localShadowPass.Render(frame.commandList, shadowFrame);

    // Stamp each served light with where its views landed. A light the atlas
    // could not serve has no tile and keeps the kNoShadowView the gather left,
    // so it lights unshadowed rather than sampling someone else's depth.
    for (const Render::LocalShadowPass::Tile &tile : _localShadowPass.Tiles())
    {
        if (tile.kind == Render::LocalLightKind::Point)
        {
            _lighting.SetPointShadowView(tile.lightIndex, tile.firstView);
        }
        else
        {
            _lighting.SetSpotShadowView(tile.lightIndex, tile.firstView);
        }
    }

    // Re-read after the draw: the table grew, which swapped its handle.
    _meshPass.SetShadowViewTable(_shadowDepthRenderer.ViewTable());
    shadows.localActive = _lastLocalShadowStats.lights > 0;
    shadows.localSettings = _localShadowPass.Settings();
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
    const glm::mat4 projection =
        ProjectionMatrix(camera, AspectRatio(static_cast<int32_t>(frame.width), static_cast<int32_t>(frame.height)));
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

void SceneRenderer::SubmitOutlineGroup(std::span<const Render::OutlinePass::OutlineItem> items, const glm::vec3 &color)
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
                                            const glm::mat4 &viewProjection, const glm::mat4 &view, ECS::Scene &scene)
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
    const bool loadingMesh = renderer != nullptr && renderer->meshBuffer == nullptr && !IsIconSuppressed(entity);

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
