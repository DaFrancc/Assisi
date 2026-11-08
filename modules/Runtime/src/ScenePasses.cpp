/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file ScenePasses.cpp
/// @brief What each of the scene's passes records. The order they run in is
/// reflectgen's, worked out from their declarations in ScenePasses.hpp.

#include <Assisi/Runtime/ScenePasses.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

#include <Assisi/Chiara/Profile.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/Math/Matrix.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/LightComponents.hpp>
#include <Assisi/Runtime/SkinnedMeshPose.hpp>

namespace Assisi::Runtime::Passes
{
namespace
{
// Clears a local-light tile back to far depth before it is redrawn (see
// Render::LocalShadowPass::ResetTile), and the sun's restore draws through the
// same triangle.
constexpr const char *kShadowTileResetShader = "shaders/shadow_tile_reset.vert.spv";
// Writes a cascade's still depth back where movers stood (see
// Render::ShadowPass::RestoreStillDepth).
constexpr const char *kShadowDepthRestoreShader = "shaders/shadow_depth_restore.frag.spv";

// The sky probe's GGX prefilter (see Render::SkyProbe). The capture itself goes
// through the sky's own shaders.
constexpr const char *kSkyPrefilterShader = "shaders/sky_prefilter.comp.spv";

// The clouds (see Render::CloudPass): their noise, drawn once, and the sky map
// and shadow map, drawn every frame.
constexpr const char *kCloudNoiseShader = "shaders/cloud_noise.comp.spv";
constexpr const char *kCloudNoiseDownsampleShader = "shaders/cloud_noise_downsample.comp.spv";
constexpr const char *kCloudWeatherShader = "shaders/cloud_weather.comp.spv";
constexpr const char *kCloudMapShader = "shaders/cloud_map.comp.spv";
constexpr const char *kCloudMapResolveShader = "shaders/cloud_map_resolve.comp.spv";
constexpr const char *kCloudShadowShader = "shaders/cloud_shadow.comp.spv";

// The SSAO pass's two timers, as its APASS declares them.
constexpr const char *kOcclusionTimer = "ssao-occlusion";
constexpr const char *kBlurTimer = "ssao-blur";

/// Where @p light's shadow is drawn from, measured from @p origin as the
/// gathered light is: where it is drawn, or, with @p followSteps, where its
/// Transform chain holds it. That pose changes only when a fixed step or a
/// direct write moves the light, not as the light is drawn between steps, so a
/// tile kept against it is redrawn at the tick rate.
Render::LocalShadowLightPose ShadowPoseOf(const ECS::Scene &scene, const LightingSystem::LocalLight &light,
                                          Render::LocalLightKind kind, bool followSteps, const glm::dvec3 &origin)
{
    Render::LocalShadowLightPose pose{.position = light.position,
                                      .direction = light.direction,
                                      .range = light.range,
                                      .outerAngleDegrees = light.outerAngleDegrees};
    if (!followSteps)
    {
        return pose;
    }
    const ECS::Transform world = ECS::WorldTransformOf(scene, light.entity);
    pose.position = glm::vec3(world.position - origin);
    if (kind == Render::LocalLightKind::Spot)
    {
        pose.direction = SpotWorldDirection(glm::mat4_cast(world.rotation) * glm::scale(glm::mat4(1.f), world.scale));
    }
    return pose;
}

/// What fraction of the screen's height a light of @p range at @p position
/// spans, from a camera at @p cameraPosition. The selector's coverage input,
/// and the one place the camera enters the ordering. A light the camera stands
/// inside saturates at 1.
float LocalLightScreenCoverage(const glm::vec3 &position, float range, const glm::vec3 &cameraPosition,
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

/// Each light of @p pool, of @p kind, scored against the camera as the selector
/// takes it.
void AddLocalCandidates(std::span<const LightingSystem::LocalLight> pool, Render::LocalLightKind kind,
                        const SceneFrame &frame, std::vector<Render::LocalShadowCandidate> &candidates)
{
    // The lights and their tiles are measured from the shadow origin, so the
    // camera they are scored against is too.
    const glm::vec3 cameraPosition = Math::TranslationOf(frame.cameraInShadow);
    const float tanHalfFovY = std::tan(glm::radians(frame.camera.fovDegrees) * 0.5f);
    for (const LightingSystem::LocalLight &light : pool)
    {
        candidates.push_back(Render::LocalShadowCandidate{
                    .kind = kind,
                    .lightIndex = light.index,
                    .screenCoverage = LocalLightScreenCoverage(light.position, light.range, cameraPosition, tanHalfFovY),
                    .intensity = light.intensity,
                    .priority = light.shadowPriority,
                    .pinned = light.shadowAlwaysOn,
                    // Every gathered light is a candidate. A frustum test here would drop
                    // the shadows of lights just off screen, and their casters are exactly
                    // the ones whose shadows reach into it.
                    .visible = true});
    }
}
} // namespace

// --- skin -------------------------------------------------------------------

bool SkinPass::Initialize(const ScenePassInit &init)
{
    // Non-fatal: skinned meshes then draw whatever their ranges last held.
    if (!skinner.Initialize(init.device))
    {
        Core::Log::Warn("SceneRenderer: skinning unavailable (skin compute pipeline failed to build).");
        return false;
    }
    return true;
}

void SkinPass::Record(Recorder &rec, const SceneFrame &frame, Writes<PosedVertices> /*out*/)
{
    const Render::MeshBuffer *anyMesh = GatherPosedInstances(frame.scene, batch);
    if (anyMesh == nullptr)
    {
        return;
    }
    skinner.Dispatch(rec.CommandList(), batch, anyMesh->VertexBuffer(), anyMesh->SkinBuffer());
}

// --- cluster-rebuild --------------------------------------------------------

void ClusterRebuildPass::Record(Recorder &rec, const SceneFrame &frame, const ViewFrame &view,
                                Writes<LightingSystem> out)
{
    // Keep the froxel grid aligned with the render projection; a drift (window
    // resize, runtime FOV/near/far edit) makes peripheral froxels stop matching
    // it and shows rectangular lighting artifacts. Rare, and timed all the same:
    // it is a full grid rebuild, so the one frame that pays it should say so.
    if (view.projection == builtFor)
    {
        return;
    }
    out.Get<LightingSystem>().Resize(rec.CommandList(), static_cast<int32_t>(frame.width),
                                     static_cast<int32_t>(frame.height), frame.camera.nearZ, frame.camera.farZ,
                                     view.projection);
    builtFor = view.projection;
}

// --- clouds -----------------------------------------------------------------

bool CloudMapPass::Initialize(const ScenePassInit &init)
{
    // Non-fatal: without it a sky has no clouds, which is what every sky had
    // before there were any. Its ambient light still follows a level's clouds.
    if (!maps.Initialize(Render::CloudPass::InitParams{.device = init.device,
                                                       .noiseSpvPath = kCloudNoiseShader,
                                                       .noiseDownsampleSpvPath = kCloudNoiseDownsampleShader,
                                                       .weatherSpvPath = kCloudWeatherShader,
                                                       .mapSpvPath = kCloudMapShader,
                                                       .mapResolveSpvPath = kCloudMapResolveShader,
                                                       .shadowSpvPath = kCloudShadowShader}))
    {
        Core::Log::Warn("SceneRenderer: clouds unavailable (their compute pipelines failed to initialise).");
        return false;
    }
    return true;
}

bool CloudMapPass::Wanted(const SceneFrame &frame) const
{
    // The textures exist exactly while the sky has clouds and the player draws
    // them. Released rather than kept otherwise — pay for what you place.
    return frame.settings.cloudsEnabled && !failed && frame.skyAvailable && frame.sky.status == SkyStatus::Ready &&
           frame.sky.clouds.present;
}

void CloudMapPass::Release()
{
    motionTime.reset();
    maps.Release();
}

bool CloudMapPass::Record(Recorder &rec, const SceneFrame &frame, Writes<CloudMap> out)
{
    if (!maps.Configure(frame.settings.cloudQuality))
    {
        Core::Log::Warn("SceneRenderer: clouds disabled (their textures failed to allocate).");
        failed = true;
        return false;
    }

    const Render::CloudSettings &settings = frame.sky.clouds.settings;
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (motionTime)
    {
        motion = Render::AdvanceClouds(motion, settings, std::chrono::duration<double>(now - *motionTime).count());
    }
    motionTime = now;

    // The fog's constants carry the sky's scatter whenever there is a sky,
    // with or without a Fog, and the clouds are seen against that sky.
    Render::CloudMapConstants map{.clouds = Render::PlaceClouds(settings, frame.viewOrigin, motion,
                                                                frame.settings.cloudQuality.steps,
                                                                frame.settings.cloudQuality.resolution),
                                  .fog = frame.fog};
    Render::LightClouds(map.clouds, frame.sky.sun, frame.sky.moon, frame.sky.settings);
    const Render::SkyAmbient clear = Render::AmbientFromSky(frame.sky.sun, frame.sky.moon, frame.sky.settings);
    Render::AmbientLightClouds(map.clouds, glm::vec3(clear.sky), glm::vec3(clear.ground));

    const Render::CloudShadowPlacement placement =
        Render::PlaceCloudShadow(settings, frame.viewOrigin, frame.sky.light.direction);
    const Render::CloudShadowMapConstants shadow{
        .clouds = map.clouds,
        .area = glm::vec4(placement.centre, Render::kCloudShadowWidth, static_cast<float>(Render::kCloudShadowSteps)),
        .light = glm::vec4(placement.light, 0.0f)};
    maps.Render(rec.CommandList(), map, shadow);

    // After Render: a reallocation swaps the textures, and each pass that reads
    // them rebuilds its binding set when it notices.
    out.Get<CloudMap>() = CloudMap{.shadowConstants = Render::MakeCloudShadowConstants(
                                       settings, placement, static_cast<float>(frame.viewOrigin.y)),
                                   .drift = motion.drift,
                                   .sky = maps.MapTexture(),
                                   .shadow = maps.ShadowTexture()};
    return true;
}

// --- sky-probe --------------------------------------------------------------

bool SkyProbePass::Initialize(const ScenePassInit &init)
{
    // Non-fatal: without it a sky lights the scene with the hemisphere alone,
    // which is what it did before there was a probe.
    if (!probe.Initialize(init.device, kSkyPrefilterShader))
    {
        Core::Log::Warn("SceneRenderer: sky reflections unavailable (the prefilter failed to initialise).");
        return false;
    }
    return true;
}

bool SkyProbePass::Wanted(const SceneFrame &frame) const
{
    // The probe is the sky's, so it exists exactly while a sky lights the
    // scene: a pinned ambient answers instead of the sky, and a scene with no
    // sky has nothing to capture. Released rather than kept in either case —
    // pay for what you place.
    return frame.settings.environment.enabled && !failed && !frame.settings.ambient.active && frame.skyAvailable &&
           frame.sky.status == SkyStatus::Ready;
}

void SkyProbePass::Release()
{
    probe.Release();
}

bool SkyProbePass::Record(Recorder &rec, const SceneFrame &frame, Reads<CloudMap> in,
                          Writes<SkyReflections, SkyPass> out)
{
    if (!probe.Configure(frame.settings.environment))
    {
        Core::Log::Warn("SceneRenderer: sky reflections disabled (the probe targets failed to allocate).");
        failed = true;
        return false;
    }

    const SkyResolution &sky = frame.sky;
    Render::SkyProbeInputs inputs = Render::MakeSkyProbeInputs(sky.sun, sky.moon, sky.settings);
    // The clouds as drawn this frame, if they are: the bake shows them, so it
    // goes stale as they drift.
    const CloudMap &clouds = in.Get<CloudMap>();
    SkyPass &skyPass = out.Get<SkyPass>();
    skyPass.SetClouds(clouds.sky);
    if (clouds.sky != nullptr)
    {
        inputs.clouds = Render::SkyProbeClouds{
            .settings = sky.clouds.settings,
            .across = glm::dvec2(frame.viewOrigin.x, frame.viewOrigin.z) - clouds.drift,
            .heightAbove = static_cast<float>(static_cast<double>(sky.clouds.settings.baseHeight) - frame.viewOrigin.y),
            .present = true};
    }
    if (probe.NeedsBake(inputs, sky.jumpSerial))
    {
        probe.Bake(rec.CommandList(), skyPass, inputs, sky.jumpSerial);
    }
    else
    {
        probe.Keep();
    }

    SkyReflections &reflections = out.Get<SkyReflections>();
    reflections = probe.IsReady() ? SkyReflections{.specularCube = probe.SpecularTexture(),
                                                   .probe = SpecularProbe{.ready = true, .maxLod = probe.MaxLod()}}
                                  : SkyReflections{};
    return true;
}

// --- shadow-cascades --------------------------------------------------------

bool SunShadowPass::Initialize(const ScenePassInit &init)
{
    // Non-fatal — the scene renders unshadowed rather than not at all.
    if (!shadowPass.Initialize(Render::ShadowPass::InitParams{.device = init.device,
                                                              .depthRenderer = init.shadowDepthRenderer,
                                                              .restoreVertexShaderSpvPath = kShadowTileResetShader,
                                                              .restorePixelShaderSpvPath = kShadowDepthRestoreShader}))
    {
        Core::Log::Warn("SceneRenderer: sun shadows unavailable (the depth pass failed to initialise).");
        return false;
    }
    return true;
}

bool SunShadowPass::Wanted(const SceneFrame &frame) const
{
    // No shadow-casting sun means no allocation and no pass — which is the
    // blank-scene rule, and the reason this is a scene fact as well as a setting.
    return frame.settings.shadows.sun.enabled && !failed && frame.shadowSun.has_value();
}

void SunShadowPass::Release()
{
    // Whatever the slices hold describes a fit nothing has checked since, and a
    // reallocation may have thrown the texture away entirely. The frame that
    // brings the sun back draws every cascade.
    (void)shadowPass.Configure(shadowPass.Settings(), /*active=*/ false);
    cadence.Forget();
    stats = Render::ShadowPass::Stats{};
    fit = Render::CascadeFit{};
}

bool SunShadowPass::Record(Recorder &rec, const SceneFrame &frame, Reads<PosedVertices> /*in*/,
                           Writes<SunShadows, ShadowDepthRenderer, ShadowCasterMobility, LodSelector> out)
{
    stats = Render::ShadowPass::Stats{};
    fit = Render::CascadeFit{};
    if (!shadowPass.Configure(frame.settings.shadows.sun, /*active=*/ true))
    {
        Core::Log::Warn("SceneRenderer: sun shadows disabled (the cascade targets failed to rebuild).");
        failed = true;
        return false;
    }
    if (!shadowPass.IsActive())
    {
        return false;
    }
    const LightingSystem::ShadowSun &sun = *frame.shadowSun;
    const glm::vec3 sampledFrom = Math::TranslationOf(frame.cameraInShadow);

    // Fitted, snapped and kept in the shadow origin's frame, which holds still
    // while the camera moves: a cascade fitted from the camera would slide its
    // texels with every step and never be kept.
    Render::CascadeFitParams fitParams;
    fitParams.cameraView = ViewMatrix(frame.cameraInShadow);
    fitParams.tanHalfFovY = std::tan(glm::radians(frame.camera.fovDegrees) * 0.5f);
    fitParams.aspectRatio = AspectRatio(static_cast<int32_t>(frame.width), static_cast<int32_t>(frame.height));
    fitParams.nearZ = frame.camera.nearZ;
    fitParams.farZ = frame.camera.farZ;
    fitParams.lightDirection = sun.direction;
    // The pass's own copy, not the setting: Configure sanitized it, and the fit
    // has to agree with the array that was actually allocated.
    fitParams.settings = shadowPass.Settings();
    const Render::CascadeFit candidate = Render::FitCascades(fitParams);

    // A reallocation leaves every slice holding depth of a texture that is gone,
    // so nothing may be kept across one.
    if (const std::uint32_t generation = shadowPass.AllocationGeneration(); generation != cascadeGeneration)
    {
        cascadeGeneration = generation;
        cadence.Forget();
    }

    // A cut in the clock — sleeping to morning, a cutscene, a scrub — moves the
    // sun by an arbitrary amount, and every kept cascade holds depth rasterized
    // from where it used to be. Forgetting here, before Plan, is what makes the
    // jump frame itself draw them all, so no frame is displayed carrying a
    // shadow from the old sun. Measured drift would catch a large jump on its
    // own, but this makes the guarantee independent of a setting an author can
    // turn up.
    if (frame.sky.jumpSerial != jumpSerial)
    {
        jumpSerial = frame.sky.jumpSerial;
        cadence.Forget();
    }

    // Which cascades this frame actually has to draw, and the fit to draw and
    // sample with — which is not the candidate for a cascade being kept, because
    // the depth in that slice was rasterized with the matrix it was fitted at.
    cadence.Plan(Render::SunShadowCadenceFrame{.frameIndex = frame.shadowFrameIndex,
                                               .settings = shadowPass.Settings().cadence,
                                               .lightDirection = sun.direction,
                                               .dynamic = frame.dynamicCasters,
                                               .invalidations = frame.casterInvalidations},
                 candidate, plan);
    fit = plan.fit;

    const std::span<const std::uint32_t> redraw(plan.redraw.data(), plan.redrawCount);
    const std::span<const std::uint32_t> movingRedraw(plan.movingRedraw.data(), plan.movingRedrawCount);

    // The gather, skipped whole when nothing needs drawing. This is where the
    // saving is: the depth pass is a fraction of a millisecond and walking every
    // Transform + MeshRenderer to decide what goes into it is not.
    if (redraw.empty() && movingRedraw.empty())
    {
        casters.Clear();
    }
    else
    {
        // After the fit, because the gather classifies against it. One volume
        // per view being drawn, in the order they are drawn: the still layers
        // being rebaked, then the slices movers are drawn over. The bit a caster
        // earns is the view it draws into, and the widths go to the selector in
        // the same order, because a caster's level in a view is measured against
        // that view's texels.
        std::array<Geometry::BoundingSphere, Render::kMaxSunShadowViews> viewVolumes{};
        std::array<float, Render::kMaxSunShadowViews> viewExtents{};
        std::uint32_t views = 0;
        for (const std::span<const std::uint32_t> cascades : {redraw, movingRedraw})
        {
            for (const std::uint32_t index : cascades)
            {
                const Render::ShadowCascade &cascade = fit.cascades[index];
                viewVolumes[views] = Render::CascadeVolumeBounds(cascade);
                viewExtents[views] = cascade.worldUnitsPerTexel * static_cast<float>(shadowPass.Settings().resolution);
                ++views;
            }
        }
        LodSelector &lodSelector = out.Get<LodSelector>();
        lodSelector.SetShadowViews(std::span<const float>(viewExtents.data(), views));
        casters.SetFrame(ShadowGatherFrame{.origin = frame.shadowOrigin, .camera = sampledFrom});
        casters.SetViews(std::span<const Geometry::BoundingSphere>(viewVolumes.data(), views),
                         static_cast<std::uint32_t>(redraw.size()), sun.direction);
        casters.Gather(frame.scene, out.Get<ShadowCasterMobility>(), &lodSelector);
    }

    stats = shadowPass.Render(rec.CommandList(), plan, casters.Result().casters);

    // After Render: a reallocation swaps the texture handle, and the mesh pass
    // rebuilds its binding set when it notices.
    out.Get<SunShadows>() = SunShadows{.frame = Render::MeshPass::SunShadowFrame{.fit = &fit,
                                                                                 .settings = shadowPass.Settings(),
                                                                                 .sampledFrom = sampledFrom,
                                                                                 .sunLightIndex = sun.index,
                                                                                 .pcss = Render::PcssShadesSun(
                                                                                     frame.settings.shadows)},
                                       .cascades = shadowPass.CascadeTexture()};
    return true;
}

// --- shadow-atlas -----------------------------------------------------------

bool ShadowAtlasPass::Initialize(const ScenePassInit &init)
{
    // The same renderer as the sun's, so every shadow view of the frame lands
    // in one table whichever kind of map produced it.
    if (!atlas.Initialize(Render::LocalShadowPass::InitParams{.device = init.device,
                                                              .depthRenderer = init.shadowDepthRenderer,
                                                              .tileResetVertexShaderSpvPath = kShadowTileResetShader}))
    {
        Core::Log::Warn("SceneRenderer: local-light shadows unavailable (the depth pass failed to initialise).");
        return false;
    }
    return true;
}

bool ShadowAtlasPass::Wanted(const SceneFrame &frame) const
{
    // No shadow-casting local light means no allocation and no pass — the same
    // blank-scene rule the cascades keep.
    return frame.settings.shadows.local.enabled && !failed &&
           !(frame.shadowSpots.empty() && frame.shadowPoints.empty());
}

void ShadowAtlasPass::Release()
{
    // The selector's memory is of an atlas that is no longer there, so the next
    // frame that turns shadows back on takes its demand outright. The mobility
    // table stays: the sun's still layers are kept by it too.
    (void)atlas.Configure(atlas.Settings(), /*active=*/ false);
    selector.Forget();
    stats = Render::LocalShadowPass::Stats{};
    selection.Clear();
    candidates.clear();
    requests.clear();
}

bool ShadowAtlasPass::Record(Recorder &rec, const SceneFrame &frame, Reads<PosedVertices> /*in*/,
                             Writes<LocalShadows, ShadowDepthRenderer, ShadowCasterMobility, LodSelector> out)
{
    const glm::vec3 cameraPosition = Math::TranslationOf(frame.cameraInShadow);
    const Render::ShadowSettings &settings = frame.settings.shadows;
    stats = Render::LocalShadowPass::Stats{};
    selection.Clear();
    // Emptied here rather than where they are refilled, because every early
    // return below skips that — and a diagnostic reading last frame's requests
    // against this frame's lights reports shadows that are not there.
    candidates.clear();
    requests.clear();

    // Selected before the atlas is configured, since a selection the caps leave
    // empty is no pass as well: the atlas is released and its kept tiles
    // forgotten, so none come back stale when a light is let in again.
    {
        ASSISI_PROFILE_SCOPE("local-shadow-select");
        candidates.reserve(frame.shadowSpots.size() + frame.shadowPoints.size());
        AddLocalCandidates(frame.shadowSpots, Render::LocalLightKind::Spot, frame, candidates);
        AddLocalCandidates(frame.shadowPoints, Render::LocalLightKind::Point, frame, candidates);
        selector.Select(candidates, settings.local, settings.selection, selection);
    }
    if (!atlas.Configure(settings.local, /*active=*/ !selection.lights.empty()))
    {
        Core::Log::Warn("SceneRenderer: local-light shadows disabled (the atlas failed to rebuild).");
        failed = true;
        return false;
    }
    LocalShadows &shadows = out.Get<LocalShadows>();
    if (!atlas.IsActive())
    {
        selector.Forget();
        shadows = LocalShadows{};
        return true;
    }

    // Back to the light each winner names, for the geometry its views are built
    // from. The selection carries scores and classes; it deliberately does not
    // carry positions, so that it can be tested without a scene.
    lightVolumes.clear();
    requests.reserve(selection.lights.size());
    lightVolumes.reserve(selection.lights.size());
    for (const Render::LocalShadowAssignment &winner : selection.lights)
    {
        const std::span<const LightingSystem::LocalLight> pool =
            winner.kind == Render::LocalLightKind::Point ? frame.shadowPoints : frame.shadowSpots;
        const std::span<const LightingSystem::LocalLight>::iterator light =
            std::ranges::find(pool, winner.lightIndex, &LightingSystem::LocalLight::index);
        if (light == pool.end())
        {
            continue;
        }
        const Render::LocalShadowLightPose pose = ShadowPoseOf(
            frame.scene, *light, winner.kind, !settings.local.cache.redrawMovingLightsEveryFrame, frame.shadowOrigin);
        requests.push_back(Render::LocalShadowRequest{
                .kind = winner.kind, .lightIndex = light->index, .pose = pose, .sizeClass = winner.sizeClass});
        // The light's whole reach, whatever shape it lights within it. A spot's
        // cone would be a tighter volume, but the cone test belongs per face
        // where the frustum already makes it — bounding the sphere here keeps
        // one gather serving both kinds.
        lightVolumes.push_back(Geometry::BoundingSphere{pose.position, light->range});
    }

    Render::LocalShadowPass::Frame shadowFrame{.requests = requests,
                                               .casters = {},
                                               .casterIndex = &casters.Index(),
                                               .movers = frame.dynamicCasters,
                                               .invalidations = frame.casterInvalidations,
                                               .frameIndex = frame.shadowFrameIndex};

    // Asked rather than guessed. A frame with nothing to draw skips the gather —
    // the per-object preparation the cost model says dominates — and the atlas
    // keeps what it holds. What needs drawing is not "did a caster move": a
    // light that moved, or came back from not casting, needs its still layer
    // baked out of casters that are standing perfectly still, and the cache
    // already answers that.
    bool drawsCasters = false;
    {
        ASSISI_PROFILE_SCOPE("local-shadow-plan");
        drawsCasters = atlas.PlanFrame(shadowFrame);
    }
    if (drawsCasters)
    {
        casters.SetFrame(ShadowGatherFrame{.origin = frame.shadowOrigin, .camera = cameraPosition});
        casters.Gather(frame.scene, lightVolumes, atlas.StillCasterRequests(), out.Get<ShadowCasterMobility>(),
                       &out.Get<LodSelector>());
        casters.BuildIndex();
        shadowFrame.casters = casters.Casters();
    }
    else
    {
        casters.Reset(static_cast<std::uint32_t>(requests.size()));
    }

    stats = atlas.Render(rec.CommandList(), shadowFrame);

    shadows = LocalShadows{.tiles = atlas.Tiles(),
                           .atlas = atlas.AtlasTexture(),
                           .frame = Render::MeshPass::LocalShadowFrame{.settings = atlas.Settings(),
                                                                       .active = stats.lights > 0,
                                                                       .pcss = Render::PcssShadesLocals(settings)}};
    return true;
}

// --- lighting ---------------------------------------------------------------

void LightUploadPass::Record(Recorder &rec, const SceneFrame & /*frame*/, const ViewFrame &view,
                             Reads<LocalShadows> in, Writes<LightingSystem> out)
{
    // Stamp each served light with where its views landed. A light the atlas
    // could not serve has no tile and keeps the kNoShadowView the gather left,
    // so it lights unshadowed rather than sampling someone else's depth.
    LightingSystem &lighting = out.Get<LightingSystem>();
    for (const Render::LocalShadowPass::Tile &tile : in.Get<LocalShadows>().tiles)
    {
        if (tile.kind == Render::LocalLightKind::Point)
        {
            lighting.SetPointShadowView(tile.lightIndex, tile.firstView);
        }
        else
        {
            lighting.SetSpotShadowView(tile.lightIndex, tile.firstView);
        }
    }
    lighting.Upload(rec.CommandList(), view.view);
}

// --- mesh-constants ---------------------------------------------------------

void FrameConstantsPass::Record(
    Recorder &rec, const SceneFrame &frame, const ViewFrame &view,
    Reads<SunShadows, LocalShadows, SkyReflections, CloudMap, ShadowDepthRenderer, LightingSystem> in,
    Writes<FrameConstants, MeshPass> out)
{
    const SunShadows &sun = in.Get<SunShadows>();
    const LocalShadows &local = in.Get<LocalShadows>();
    const SkyReflections &reflections = in.Get<SkyReflections>();
    const CloudMap &clouds = in.Get<CloudMap>();

    // What the earlier passes made, bound where the mesh pass's frame begins,
    // before the prepass, which draws through the same binding set.
    MeshPass &meshPass = out.Get<MeshPass>();
    meshPass.SetShadowMap(sun.cascades);
    meshPass.SetShadowAtlas(local.atlas);
    meshPass.SetShadowViewTable(in.Get<ShadowDepthRenderer>().ViewTable());
    meshPass.SetEnvironment(reflections.specularCube);
    meshPass.SetAmbientOcclusion(frame.occlusion.texture);
    meshPass.SetCloudShadow(clouds.shadow);
    meshPass.UpdateFrameConstants(
        rec.CommandList(),
        Render::MeshPass::FrameConstantsParams{.viewProjection = view.projection * view.view,
                                               .view = view.view,
                                               .screenWidth = frame.width,
                                               .screenHeight = frame.height,
                                               .nearZ = frame.camera.nearZ,
                                               .farZ = frame.camera.farZ,
                                               .dirLightCount = in.Get<LightingSystem>().DirLightCount(),
                                               .debugView = frame.settings.debugView,
                                               .shadowDebugView = frame.settings.shadowDebugView,
                                               .indirect = ResolveIndirect(frame.sky, frame.settings.ambient,
                                                                           reflections.probe),
                                               .sunShadows = sun.frame,
                                               .localShadows = local.frame,
                                               .fog = frame.fog,
                                               .fogVolumes = frame.fogVolumeLanes,
                                               .screenOcclusion = frame.occlusion.texture != nullptr,
                                               .cloudShadow = clouds.shadowConstants});
}

// --- depth-prepass ----------------------------------------------------------

void DepthPrepassPass::Record(Recorder &rec, const SceneFrame &frame, const ViewFrame &view,
                              Reads<FrameConstants, PosedVertices> /*in*/,
                              Writes<OpaqueDraws, BlendedDraws, SceneDepth, MeshPass, LodSelector, MeshCuller, DrawStats> out)
{
    // The extract, cull and sort, kept for the lit pass to shade.
    const SceneSettings &settings = frame.settings;
    out.Get<DrawStats>() = DrawScene(DrawSceneParams{.scene = frame.scene,
                                                     .meshPass = out.Get<MeshPass>(),
                                                     .recorder = rec,
                                                     .target = out.Get<SceneDepth>().target,
                                                     .view = view.view,
                                                     .projection = view.projection,
                                                     .origin = frame.viewOrigin,
                                                     .nearZ = frame.camera.nearZ,
                                                     .farZ = frame.camera.farZ,
                                                     .frustumCulling = settings.frustumCulling,
                                                     .sortDraws = settings.sortDraws,
                                                     .gpuCulling = settings.gpuCulling,
                                                     .culler = &out.Get<MeshCuller>(),
                                                     .cullBuilder = &cullTables,
                                                     .lodSelector = &out.Get<LodSelector>(),
                                                     .viewerInstance = view.viewerInstance,
                                                     .blendedItems = &out.Get<BlendedDraws>().items});
}

// --- scene-distance and ssao ------------------------------------------------

bool DistancePass::Wanted(const SceneFrame &frame) const
{
    return frame.occlusion.texture != nullptr;
}

void DistancePass::Record(Recorder &rec, const SceneFrame & /*frame*/, const ViewFrame &view,
                          Reads<SceneDepth> /*in*/, Writes<SceneDistance, SceneDistancePass> out)
{
    out.Get<SceneDistancePass>().Render(rec.CommandList(), view.projection);
}

bool OcclusionPass::Wanted(const SceneFrame &frame) const
{
    return frame.occlusion.texture != nullptr;
}

void OcclusionPass::Record(Recorder &rec, const SceneFrame &frame, const ViewFrame &view,
                           Reads<SceneDistance> /*in*/, Writes<Occlusion, SsaoPass> out)
{
    const SsaoPass &ssao = out.Get<SsaoPass>();
    rec.Begin(kOcclusionTimer);
    ssao.RenderOcclusion(rec.CommandList(), Render::SsaoPass::Frame{.projection = view.projection,
                                                                    .farZ = frame.camera.farZ,
                                                                    .settings = frame.settings.ssao});
    rec.Begin(kBlurTimer);
    ssao.RenderBlur(rec.CommandList());
}

// --- draw-scene -------------------------------------------------------------

void LitPass::Record(Recorder &rec, const SceneFrame & /*frame*/, const ViewFrame & /*view*/,
                     Reads<OpaqueDraws, SceneDepth, Occlusion, FrameConstants, LightingSystem> /*in*/,
                     Writes<SceneColor, MeshPass, DrawStats> out)
{
    out.Get<DrawStats>().drawCalls +=
        out.Get<MeshPass>().Redraw(rec.Frame(out.Get<SceneColor>()), Render::MeshPassStage::Lit).drawCalls;
}

// --- sky --------------------------------------------------------------------

bool SkyBackgroundPass::Wanted(const SceneFrame &frame) const
{
    // A foggy level with no sky still draws the pass, in the fog's own colour,
    // so that what the fog fades into is what is behind it.
    return frame.skyAvailable && (frame.sky.status == SkyStatus::Ready || frame.fogged);
}

void SkyBackgroundPass::Record(Recorder &rec, const SceneFrame &frame, const ViewFrame &view, Reads<SceneDepth, CloudMap> in,
                               Writes<SceneColor, SkyPass> out)
{
    // The sky goes last, into whatever the geometry left at the depth clear. Both
    // halves of it come from the scene — the sun from a directional light, the
    // look from the Skybox component on that same entity — so a level authors its
    // own world and a light that moves takes the sky with it.
    SkyPass &skyPass = out.Get<SkyPass>();
    skyPass.SetClouds(in.Get<CloudMap>().sky);
    skyPass.SetFogVolumes(frame.fog, frame.fogVolumeLanes);
    const glm::mat4 viewProjection = view.projection * view.view;
    const glm::vec3 eye = Math::TranslationOf(view.cameraInView);
    if (frame.sky.status == SkyStatus::Ready)
    {
        skyPass.SetFog(frame.fogged ? Render::SkyFogMode::BelowHorizon : Render::SkyFogMode::Off, frame.fogColor);
        skyPass.Draw(rec.Frame(out.Get<SceneColor>()), viewProjection, eye, frame.sky.sun, frame.sky.moon,
                     frame.sky.settings);
    }
    else
    {
        skyPass.SetFog(Render::SkyFogMode::Flat, frame.fogColor);
        skyPass.Draw(rec.Frame(out.Get<SceneColor>()), viewProjection, eye, Render::SkySun{}, Render::SkyMoon{},
                     Render::SkySettings{});
    }
}

// --- draw-blended -----------------------------------------------------------

void BlendedPass::Record(Recorder &rec, const SceneFrame & /*frame*/, const ViewFrame & /*view*/,
                         Reads<BlendedDraws, SceneDepth> in, Writes<SceneColor, MeshPass, DrawStats> out)
{
    // See-through surfaces last, over the solid scene and the sky they show.
    const std::vector<Render::DrawItem> &items = in.Get<BlendedDraws>().items;
    if (items.empty())
    {
        return;
    }
    const Render::MeshPass::SubmitStats blended = out.Get<MeshPass>().SubmitBlended(rec.Frame(out.Get<SceneColor>()), items);
    DrawStats &stats = out.Get<DrawStats>();
    stats.drawnItems += blended.instances;
    stats.batches += blended.batches;
    stats.drawCalls += blended.drawCalls;
}

} // namespace Assisi::Runtime::Passes
