/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file SceneRendererShadows.cpp
/// @brief SceneRenderer's shadow half: what moved this frame, the sun's
/// cascades, the local-light atlas, and the readout the editor shows of them.
/// SceneRenderer.cpp has setup and the frame that calls into these.

#include <Assisi/Runtime/SceneRenderer.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/Renderer.hpp>

namespace Assisi::Runtime
{

void SceneRenderer::ForgetKeptShadows()
{
    // A kept cascade or atlas tile holds its casters at the levels chosen when
    // it was drawn, and nothing about a still scene would ever redraw it.
    _sunCadence.Forget();
    _localShadowSelector.Forget();
}

void SceneRenderer::UpdateShadowMovers(ECS::Scene &scene)
{
    // What moved since the last frame, taken from the Transform pool's change
    // ticks rather than by asking every caster. This is the whole invalidation
    // input for both halves of the shadow system, and on a still frame it is
    // empty — which is what makes a still frame free.
    //
    // Once per frame, before either half: the ticks are a cursor, and a second
    // read of them would come back empty and tell the second half that nothing
    // had moved.
    ++_shadowFrameIndex;
    _movedEntities.clear();
    scene.ChangedSince<Transform>(_lastMoverTick, _movedEntities);

    // The mobility table holds casters by handle across frames, so a caster
    // that has since been destroyed, or lost what made it one, must be let go
    // of — or its shadow stays in whichever layer last drew it. Read on the
    // same cursor as the movers, before it advances.
    _removedEntities.clear();
    const bool removalsComplete = scene.RemovedSince<Transform>(_lastMoverTick, _removedEntities) &&
                                  scene.RemovedSince<MeshRenderer>(_lastMoverTick, _removedEntities);
    _lastMoverTick = scene.CurrentChangeTick();

    if (removalsComplete)
    {
        for (const ECS::Entity entity : _removedEntities)
        {
            _casterMobility.Drop(ShadowCasterId(entity));
        }
    }
    else
    {
        // The log no longer reaches back to the last look, so which ones went
        // is unknown: every handle is checked instead.
        _casterMobility.DropIf([&scene](std::uint64_t casterId)
                               {
                                   const ECS::Entity entity = ShadowCasterEntity(casterId);
                                   return !scene.IsAlive(entity) || !scene.Has<Transform>(entity) ||
                                          !scene.Has<MeshRenderer>(entity);
                               });
    }

    GatherShadowMovers(scene, _movedEntities, _movedCasters);

    // Which casters move and which changed sides, decided once for both halves
    // of the shadow system: the sun's cascades and the local atlas each keep a
    // still layer, and they must agree about which one an object is in.
    //
    // Real time rather than the game's tick, which this has no view of: what it
    // measures is how long an object has stood still, and that is the same
    // either way wherever the game runs at speed.
    const double nowSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    _casterMobility.Update(nowSeconds, _shadowSettings.local.cache.promoteStillSeconds, _movedCasters,
                           _dynamicCasters, _casterInvalidations);
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
        // Whatever the slices hold describes a fit nothing has checked since,
        // and a reallocation may have thrown the texture away entirely. The
        // frame that brings the sun back draws every cascade.
        _sunCadence.Forget();
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
    const Render::CascadeFit candidate = Render::FitCascades(fitParams);

    // A reallocation leaves every slice holding depth of a texture that is gone,
    // so nothing may be kept across one.
    if (const std::uint32_t generation = _shadowPass.AllocationGeneration(); generation != _cascadeGeneration)
    {
        _cascadeGeneration = generation;
        _sunCadence.Forget();
    }

    // A cut in the clock — sleeping to morning, a cutscene, a scrub — moves the
    // sun by an arbitrary amount, and every kept cascade holds depth rasterized
    // from where it used to be. Forgetting here, before Plan, is what makes the
    // jump frame itself draw them all: the resolver has already produced the new
    // direction, Gather has already uploaded it, and ShadowPass::Render will draw
    // every cascade Plan names into this frame's command list. No frame is
    // displayed carrying a shadow from the old sun.
    //
    // Measured drift would catch a large jump on its own — the tolerance is a
    // hundredth of a degree — but this makes the guarantee independent of a
    // setting an author can turn up, and gives a consumer with temporal history
    // a signal that says "cut" rather than "fast".
    if (_lastSky.jumpSerial != _jumpSerial)
    {
        _jumpSerial = _lastSky.jumpSerial;
        _sunCadence.Forget();
    }

    // Which cascades this frame actually has to draw, and the fit to draw and
    // sample with — which is not the candidate for a cascade being kept, because
    // the depth in that slice was rasterized with the matrix it was fitted at.
    _sunCadence.Plan(Render::SunShadowCadenceFrame{.frameIndex = _shadowFrameIndex,
                                                   .settings = _shadowPass.Settings().cadence,
                                                   .lightDirection = sun->direction,
                                                   .dynamic = _dynamicCasters,
                                                   .invalidations = _casterInvalidations},
                     candidate, _sunCadencePlan);
    _cascadeFit = _sunCadencePlan.fit;

    const std::span<const std::uint32_t> redraw(_sunCadencePlan.redraw.data(), _sunCadencePlan.redrawCount);
    const std::span<const std::uint32_t> movingRedraw(_sunCadencePlan.movingRedraw.data(),
                                                      _sunCadencePlan.movingRedrawCount);

    // The gather, skipped whole when nothing needs drawing. This is where the
    // saving is: the depth pass is a fraction of a millisecond and walking every
    // Transform + MeshRenderer to decide what goes into it is not.
    if (redraw.empty() && movingRedraw.empty())
    {
        _shadowCasters.Clear();
    }
    else
    {
        // After the fit, because the gather classifies against it: the sun's
        // shadow distance lives in the cascades' own extent, and a caster that
        // reaches no cascade is one no view wants.
        //
        // One volume per view **being drawn**, in the order they are drawn: the
        // still layers being rebaked, then the slices movers are drawn over.
        // The bit a caster earns here is the view it draws into, so handing it
        // every cascade's volume would number the bits by cascade instead.
        //
        // The widths go to the selector in the same order, for the same reason:
        // a caster's level in a view is measured against that view's texels.
        std::array<Geometry::BoundingSphere, 2 * Render::kMaxShadowCascades> viewVolumes{};
        std::array<float, 2 * Render::kMaxShadowCascades> viewExtents{};
        std::uint32_t views = 0;
        for (const std::span<const std::uint32_t> cascades : {redraw, movingRedraw})
        {
            for (const std::uint32_t index : cascades)
            {
                const Render::ShadowCascade &cascade = _cascadeFit.cascades[index];
                viewVolumes[views] = Render::CascadeVolumeBounds(cascade);
                viewExtents[views] =
                    cascade.worldUnitsPerTexel * static_cast<float>(_shadowPass.Settings().resolution);
                ++views;
            }
        }
        _lodSelector.SetShadowViews(std::span<const float>(viewExtents.data(), views));
        _shadowCasters.SetViews(std::span<const Geometry::BoundingSphere>(viewVolumes.data(), views),
                                static_cast<std::uint32_t>(redraw.size()), sun->direction);
        _shadowCasters.Gather(scene, _casterMobility, &_lodSelector);
    }

    _lastShadowStats = _shadowPass.Render(frame.commandList, _sunCadencePlan, _shadowCasters.Result().casters);

    shadows.fit = &_cascadeFit;
    shadows.settings = _shadowPass.Settings();
    shadows.sunLightIndex = sun->index;
    shadows.debugView = _shadowDebugView;
    shadows.sunPcss = Render::PcssShadesSun(_shadowSettings);
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
    // Emptied here rather than where they are refilled, because every early
    // return below skips that — and a diagnostic reading last frame's requests
    // against this frame's lights reports shadows that are not there.
    _localCandidates.clear();
    _localRequests.clear();

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
        // mobility table stays: the sun's still layers are kept by it too, and
        // an atlas coming back cuts every tile afresh whatever it says.
        _localShadowSelector.Forget();
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

    Render::LocalShadowPass::Frame shadowFrame{.requests = _localRequests,
                                               .casters = {},
                                               .casterIndex = &_localShadowCasters.Index(),
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
        _localShadowCasters.Gather(scene, _localLightVolumes, _localShadowPass.StillCasterRequests(), _casterMobility,
                                   &_lodSelector);
        _localShadowCasters.BuildIndex();
        shadowFrame.casters = _localShadowCasters.Casters();
    }
    else
    {
        _localShadowCasters.Reset(static_cast<std::uint32_t>(_localRequests.size()));
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
    shadows.localPcss = Render::PcssShadesLocals(_shadowSettings);
}

void SceneRenderer::BuildShadowDiagnostics()
{
    if (!_shadowDiagnosticsEnabled)
    {
        return;
    }
    _shadowDiagnostics.Clear();

    // An inactive pass returns before it scores anything, so the candidate list
    // is empty — but the lights are still placed, and how many want a shadow is
    // what the readout's M means. Rebuilt from the gathered lights here rather
    // than scored in the pass, which would make a switched-off feature do work.
    const bool active = _localShadowPass.IsActive();
    if (!active)
    {
        for (const LightingSystem::LocalLight &light : _lighting.ShadowCastingSpotLights())
        {
            _localCandidates.push_back(
                Render::LocalShadowCandidate{.kind = Render::LocalLightKind::Spot, .lightIndex = light.index});
        }
        for (const LightingSystem::LocalLight &light : _lighting.ShadowCastingPointLights())
        {
            _localCandidates.push_back(
                Render::LocalShadowCandidate{.kind = Render::LocalLightKind::Point, .lightIndex = light.index});
        }
    }

    Render::BuildLocalShadowDiagnostics(
        Render::LocalShadowDiagnosticsFrame{.candidates = _localCandidates,
                                            .requests = _localRequests,
                                            .plans = _localShadowPass.Plans(),
                                            .served = _localShadowPass.ServedTiles(),
                                            .deferredFaces = _lastLocalShadowStats.deferredFaces,
                                            .budgetFaces = _shadowSettings.local.cache.enabled
                                                               ? _shadowSettings.local.cache.updateBudgetFaces
                                                               : 0u,
                                            .active = active},
        _shadowDiagnostics);

    // Every fitted cascade, not only the ones drawn: a cascade that kept its
    // depth is still a cascade the sun has, and a readout that dropped it would
    // report the sun losing cascades whenever it stopped paying for them.
    _shadowDiagnostics.cascadeCount = _cascadeFit.count;
    _shadowDiagnostics.cascadeCasters = _lastShadowStats.cascadeCasters;
    _shadowDiagnostics.cascadeAgeFrames = _sunCadencePlan.ageFrames;
    _shadowDiagnostics.cascadesRedrawn = _lastShadowStats.cascades;
}

const Render::LocalShadowLightReport *SceneRenderer::ShadowReportFor(ECS::Entity entity) const
{
    if (entity == ECS::NullEntity)
    {
        return nullptr;
    }
    // Both pools, because an entity carries one kind of light and which one is
    // not worth asking the scene about again — the gathered rows are a handful
    // and they are already in hand.
    for (const auto &[pool, kind] :
         {std::pair{_lighting.ShadowCastingSpotLights(), Render::LocalLightKind::Spot},
          std::pair{_lighting.ShadowCastingPointLights(), Render::LocalLightKind::Point}})
    {
        for (const LightingSystem::LocalLight &light : pool)
        {
            if (light.entity == entity)
            {
                return _shadowDiagnostics.Find(kind, light.index);
            }
        }
    }
    return nullptr;
}

} // namespace Assisi::Runtime
