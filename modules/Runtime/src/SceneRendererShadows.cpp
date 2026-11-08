/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file SceneRendererShadows.cpp
/// @brief SceneRenderer's shadow bookkeeping: what moved this frame, where the
/// shadow maps are measured from, and the readout the editor shows of them.
/// The shadow passes themselves are in ScenePasses.cpp.

#include <Assisi/Runtime/SceneRenderer.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>

#include <Assisi/ECS/WorldMatrix.hpp>
#include <Assisi/Math/Matrix.hpp>
#include <Assisi/Runtime/ScenePasses.generated.hpp>

namespace Assisi::Runtime
{

void SceneRenderer::PlaceOrigins(const ECS::WorldMatrix &cameraWorld)
{
    _viewOrigin = ECS::WorldPositionOf(cameraWorld);
    _shadowOrigin = Math::WorldCellCentre(cameraWorld.cell);
    if (_shadowCell != cameraWorld.cell)
    {
        _shadowCell = cameraWorld.cell;
        ForgetKeptShadows();
    }
}

void SceneRenderer::ForgetKeptShadows()
{
    // A kept cascade or atlas tile holds its casters where and at the levels
    // they were when it was drawn, and nothing about a still scene would ever
    // redraw it. The selector only remembers the tiles' sizes; the tiles
    // themselves are the pass's.
    _graph->passes.sunShadow.cadence.Forget();
    _graph->passes.shadowAtlas.selector.Forget();
    _graph->passes.shadowAtlas.atlas.ForgetKeptTiles();
}

void SceneRenderer::UpdateShadowMovers(ECS::Scene &scene)
{
    // What moved since the last frame, taken from the WorldMatrix pool's change
    // ticks rather than by asking every caster. This is the whole invalidation
    // input for both halves of the shadow system, and on a still frame it is
    // empty — which is what makes a still frame free.
    //
    // Once per frame, before either half: the ticks are a cursor, and a second
    // read of them would come back empty and tell the second half that nothing
    // had moved.
    ++_shadowFrameIndex;
    if (!_moverTickPrimed)
    {
        // A scene's first frame finds every matrix written by its load.
        // Read as motion, that would draw every caster as a mover until they
        // all settled at once and the bake budget met every light together.
        // Nothing is kept from before, so every map draws them as still.
        _lastMoverTick = scene.CurrentChangeTick();
        _moverTickPrimed = true;
    }
    // From the drawn matrices rather than the Transforms: a body blending
    // between two fixed steps moves on screen every frame though its Transform
    // is written only on step frames, and a shadow kept between them would be
    // left behind the body it belongs to. A child of a moving body, and an
    // entity a RenderOffset moves, are caught the same way.
    _movedEntities.clear();
    scene.ChangedSince<ECS::WorldMatrix>(_lastMoverTick, _movedEntities);
    // A skinned mesh posing where it stands moves its shadow without moving its
    // matrix. Its change ticks cannot say so: evaluation writes it every frame,
    // changed or not.
    for (auto [entity, skinned] : scene.Query<SkinnedMesh>())
    {
        if (skinned.poseChanged && std::ranges::find(_movedEntities, entity) == _movedEntities.end())
        {
            _movedEntities.push_back(entity);
        }
    }

    // The mobility table holds casters by handle across frames, so a caster
    // that has since been destroyed, or lost what made it one, must be let go
    // of — or its shadow stays in whichever layer last drew it. Read on the
    // same cursor as the movers, before it advances.
    _removedEntities.clear();
    const bool removalsComplete = scene.RemovedSince<Transform>(_lastMoverTick, _removedEntities) &&
                                  scene.RemovedSince<MeshRenderer>(_lastMoverTick, _removedEntities);
    // An edited MeshRenderer changes what its kept shadows should hold without
    // moving anything: Casts Shadows turned off, another mesh, a material that
    // is now masked or double-sided. It is forgotten, which redraws where it was
    // kept, and moves, which draws it as it is now. One that also moved is
    // listed twice, which the mobility table takes as one move.
    const std::size_t editedFrom = _movedEntities.size();
    scene.ChangedSince<MeshRenderer>(_lastMoverTick, _movedEntities);
    for (std::size_t edited = editedFrom; edited < _movedEntities.size(); ++edited)
    {
        _casterMobility.Drop(ShadowCasterId(_movedEntities[edited]));
    }
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

    GatherShadowMovers(scene, _movedEntities, _shadowOrigin, _movedCasters);

    // Which casters move and which changed sides, decided once for both halves
    // of the shadow system: the sun's cascades and the local atlas each keep a
    // still layer, and they must agree about which one an object is in.
    //
    // On the simulation's clock rather than the wall's: movers are written on
    // its tick, and a caster's shadow edge differs by a hair between the two
    // layers, so settling on real time would make the same run draw slightly
    // different pictures from one launch to the next.
    _casterMobility.Update(_simulationSeconds, _settings.shadows.local.cache.promoteStillSeconds, _movedCasters,
                           _dynamicCasters, _casterInvalidations);
}

void SceneRenderer::BuildShadowDiagnostics()
{
    if (!_shadowDiagnosticsEnabled)
    {
        return;
    }
    _shadowDiagnostics.Clear();
    Passes::ShadowAtlasPass &atlas = _graph->passes.shadowAtlas;
    const Passes::SunShadowPass &sun = _graph->passes.sunShadow;

    // An inactive pass returns before it scores anything, so the candidate list
    // is empty — but the lights are still placed, and how many want a shadow is
    // what the readout's M means. Rebuilt from the gathered lights here rather
    // than scored in the pass, which would make a switched-off feature do work.
    const bool active = atlas.atlas.IsActive();
    if (!active)
    {
        for (const LightingSystem::LocalLight &light : _lighting.ShadowCastingSpotLights())
        {
            atlas.candidates.push_back(
                Render::LocalShadowCandidate{.kind = Render::LocalLightKind::Spot, .lightIndex = light.index});
        }
        for (const LightingSystem::LocalLight &light : _lighting.ShadowCastingPointLights())
        {
            atlas.candidates.push_back(
                Render::LocalShadowCandidate{.kind = Render::LocalLightKind::Point, .lightIndex = light.index});
        }
    }

    Render::BuildLocalShadowDiagnostics(
        Render::LocalShadowDiagnosticsFrame{.candidates = atlas.candidates,
                                            .requests = atlas.requests,
                                            .plans = atlas.atlas.Plans(),
                                            .served = atlas.atlas.ServedTiles(),
                                            .deferredFaces = atlas.stats.deferredFaces,
                                            .budgetFaces = _settings.shadows.local.cache.enabled
                                                               ? _settings.shadows.local.cache.updateBudgetFaces
                                                               : 0u,
                                            .active = active},
        _shadowDiagnostics);

    // Every fitted cascade, not only the ones drawn: a cascade that kept its
    // depth is still a cascade the sun has, and a readout that dropped it would
    // report the sun losing cascades whenever it stopped paying for them.
    _shadowDiagnostics.cascadeCount = sun.fit.count;
    _shadowDiagnostics.cascadeCasters = sun.stats.cascadeCasters;
    _shadowDiagnostics.cascadeAgeFrames = sun.plan.ageFrames;
    _shadowDiagnostics.cascadesRedrawn = sun.stats.cascades;
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
