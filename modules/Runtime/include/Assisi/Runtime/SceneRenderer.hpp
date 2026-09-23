/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SceneRenderer.hpp
/// @brief Default scene-render path: the "just draw my scene" entry point.
///
/// SceneRenderer bundles the standard opaque-forward setup — clustered lighting
/// (LightingSystem + its froxel grid) and the shared MeshPass pipeline — behind
/// one call, so a game's OnRender is a single line rather than a hand-rolled
/// pipeline dance. It owns the render-side machinery; the game owns the scene
/// and its per-entity meshes/textures.
///
/// Typical lifetime, from an application:
///   Initialize(device, GetSceneFramebufferInfo(), w, h, camera);   // once
///   Resize(w, h, camera);                                          // on window resize
///   OnRenderTargetsChanged(framebufferInfo);                       // on AA/MSAA change
///   Render(frame, scene, cameraTransform, camera);                 // every frame
///
/// The camera is passed as its two components (world transform + projection
/// params) rather than pulled from the scene, so the camera may live in the
/// game scene or a separate one. Projection is derived internally from the
/// camera and the frame's dimensions — callers never build a projection matrix.

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/MeshCuller.hpp>
#include <Assisi/Render/MeshPass.hpp>
#include <Assisi/Render/RenderFrame.hpp>
#include <Assisi/Render/ShadowCadence.hpp>
#include <Assisi/Render/ShadowDiagnostics.hpp>
#include <Assisi/Render/ShadowPass.hpp>
#include <Assisi/Render/SkyPass.hpp>
#include <Assisi/Render/SkyProbe.hpp>
#include <Assisi/Render/SceneDistancePass.hpp>
#include <Assisi/Render/SsaoPass.hpp>
#include <Assisi/Render/SsaoSettings.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/IndirectResolve.hpp>
#include <Assisi/Runtime/LightingSystem.hpp>
#include <Assisi/Runtime/Renderer.hpp>

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <span>
#include <vector>

namespace Assisi::Runtime
{

class SceneRenderer
{
public:
    SceneRenderer() = default;

    /// @brief Everything Initialize() needs, gathered so callers name what they set.
    struct InitParams
    {
        nvrhi::IDevice *device = nullptr;
        /// Format/sample-count the mesh pipeline targets
        /// (e.g. Application::GetSceneFramebufferInfo()).
        nvrhi::FramebufferInfo framebufferInfo;
        /// Viewport size in pixels, for the initial cluster grid.
        int32_t width = 0;
        int32_t height = 0;
        /// Projection params (near/far/FOV) of the active camera.
        Camera camera;
        /// The scene AssetCache's bindless material-texture table + layout,
        /// threaded into the mesh pipeline (stage D). Must outlive the renderer.
        nvrhi::IBindingLayout *bindlessLayout = nullptr;
        nvrhi::IDescriptorTable *bindlessTable = nullptr;
        nvrhi::IBuffer *materialTable = nullptr;
    };

    /// @brief Bring up clustered lighting and the scene mesh pipeline against the
    /// given render-target format. Uses the engine's default scene shaders.
    /// @return false if the lighting compute shaders or the mesh pipeline failed
    /// to build.
    [[nodiscard]] bool Initialize(const InitParams &params);

    /// @brief Rebuild the cluster froxel grid for a new viewport/projection.
    /// Call from the application's resize hook. No-op until Initialize() succeeds.
    void Resize(int32_t width, int32_t height, const Camera &camera);

    /// @brief Rebuild just the graphics pipeline after the render-target format
    /// changes (e.g. an MSAA toggle); binding sets and shaders are reused.
    /// @return false if the pipeline failed to rebuild. No-op (true) before Initialize().
    [[nodiscard]] bool OnRenderTargetsChanged(const nvrhi::FramebufferInfo &framebufferInfo);

    /// @brief Draw `scene` from the given camera into `frame`. Propagates the
    /// scene's transforms, refreshes lighting, rebuilds the froxel grid if the
    /// projection changed since last frame, and draws every mesh entity. No-op
    /// until Initialize() succeeds.
    void Render(const Render::RenderFrame &frame, ECS::Scene &scene, const Transform &cameraTransform,
                const Camera &camera)
    {
        Render(frame, scene, cameraTransform, camera, _lastPropagationTick);
    }

    /// @brief As above, with the transform-propagation bookmark supplied by the
    /// caller instead of kept in the renderer.
    ///
    /// The bookmark is the scene tick at the end of that scene's last
    /// propagation, and it is only meaningful *per scene*: one renderer drawing
    /// two scenes with a single bookmark would compare scene B's tick against
    /// scene A's and skip propagating whichever it drew second. An app holding
    /// several worlds resident stores the bookmark in the world
    /// (App::World::propagationTick) and passes it here; the overload above
    /// keeps the renderer's own for the single-scene case.
    void Render(const Render::RenderFrame &frame, ECS::Scene &scene, const Transform &cameraTransform,
                const Camera &camera, uint64_t &propagationTick);

    [[nodiscard]] bool IsValid() const { return _meshPass.IsValid(); }

    /// @brief The underlying mesh pipeline, for games that need direct draw control
    /// beyond the default Render() path.
    [[nodiscard]] Render::MeshPass &MeshPass() { return _meshPass; }

    /// @brief Evict the mesh pass's cached material binding sets. Call after the
    /// asset set backing the scene's materials changes (level unload / asset-cache
    /// clear) so freed GPU resources aren't reused. No-op before Initialize().
    void InvalidateAssetBindings() { _meshPass.InvalidateBindingSets(); }

    /// @brief Enable/disable view-frustum culling in the default draw path (on by
    /// default). Turning it off submits every mesh, for A/B comparing the cull.
    void SetFrustumCulling(bool enabled) { _frustumCulling = enabled; }
    [[nodiscard]] bool FrustumCulling() const { return _frustumCulling; }

    /// @brief Enable/disable draw-list sorting by sort key (on by default). Off
    /// submits in query order — the image is identical, but LastDrawStats().batches
    /// climbs (identical meshes no longer land adjacent to coalesce), which is how
    /// the sort's instancing payoff is measured.
    void SetSortDraws(bool enabled) { _sortDraws = enabled; }
    [[nodiscard]] bool SortDraws() const { return _sortDraws; }

    /// @brief Take the GPU-driven cull path instead of the CPU extract/sort path
    /// (off by default). A compute pass frustum-culls every object, selects its
    /// LOD level and builds the indirect draw commands on the GPU; the CPU issues
    /// one drawIndexedIndirect. An A/B toggle against the CPU path — the opaque
    /// image is identical but for instances inside the LOD dead band, which this
    /// path does not hold. No-op if the culler failed to initialize (falls back to
    /// the CPU path). `SortDraws` doesn't affect this path; `FrustumCulling` gates
    /// the GPU frustum test.
    void SetGpuCulling(bool enabled) { _gpuCulling = enabled; }
    [[nodiscard]] bool GpuCulling() const { return _gpuCulling; }

    /// @brief The knobs screen-size LOD selection reads (on by default).
    ///
    /// Applies to both draw paths and to both shadow gathers. The local lights
    /// cast at the level on screen; each sun cascade may go one level coarser
    /// where its texels are too coarse to show the difference. The GPU-driven
    /// cull path keeps no memory per instance, so while it draws, `hysteresis`
    /// is held nowhere.
    ///
    /// `bias` is the quality dial — above 1 holds a finer level further away —
    /// and `enabled` false pins everything to LOD0, which is the A/B against
    /// the whole feature. `shadowLod` false is the A/B against the cascades'
    /// own pick. `forcedLevel` puts every instance on one level for
    /// inspection. A mesh with no LOD chain is unaffected by any of them.
    ///
    /// A change redraws every kept shadow map, which hold casters at the
    /// levels the old settings chose.
    void SetLodSettings(const Runtime::LodSettings &settings);
    [[nodiscard]] const Runtime::LodSettings &LodSettings() const { return _lodSelector.Settings(); }

    /// @brief Draw @p entity at @p level whatever its size asks for, or release
    /// the pin with a negative @p level. One entity at a time.
    ///
    /// Where `forcedLevel` puts the whole viewport on one level, this puts one
    /// instance there — for judging a chain on the thing that carries it without
    /// moving every other instance in the scene off the level it earned. Never
    /// saved: it is a way of looking at a scene, not a fact about it.
    ///
    /// Redraws every kept shadow map, as a settings change does.
    void PinLod(ECS::Entity entity, int32_t level);

    /// @brief The level @p entity is pinned to, or -1 when it is not pinned.
    [[nodiscard]] int32_t PinnedLod(ECS::Entity entity) const { return _lodSelector.PinnedLevel(entity); }

    /// @brief Whether any instance is pinned — what lets a viewport say that
    /// something on screen is not at the level the scene's rules would give it.
    [[nodiscard]] bool HasPinnedLod() const { return _lodSelector.HasPin(); }

    /// @brief What LOD selection did with the instance on @p entity, drawing
    /// @p mesh at @p worldMatrix.
    ///
    /// The join the selector cannot make on its own: it remembers levels by
    /// entity and has no way to reach the mesh or the pose an inspector is
    /// looking at. Measured against this frame's camera, so it answers for the
    /// image on screen.
    [[nodiscard]] Runtime::LodReport LodReportFor(ECS::Entity entity, const Render::MeshBuffer &mesh,
                                                  const glm::mat4 &worldMatrix) const;

    /// @brief The level the mesh pass drew @p entity at, drawing @p mesh at
    /// @p worldMatrix.
    ///
    /// The CPU path remembers what it drew. The GPU cull remembers nothing, but
    /// its pick is a function of the frame alone, so the same measurement taken
    /// here lands where it did.
    [[nodiscard]] uint32_t DrawnLodLevel(ECS::Entity entity, const Render::MeshBuffer &mesh,
                                         const glm::mat4 &worldMatrix) const;

    /// @brief Select a material-channel debug view (None = normal lit render).
    /// The mesh pass short-circuits its shader to that channel — for inspecting
    /// base colour / metallic / roughness / normal / occlusion / emissive.
    void SetDebugView(Render::MaterialDebugView view) { _debugView = view; }
    [[nodiscard]] Render::MaterialDebugView DebugView() const { return _debugView; }

    /// @brief Pin the indirect term to a uniform colour and intensity, in place
    /// of whatever the scene would be lit by.
    ///
    /// The blueprint editor turns it up: inspecting a model means seeing all of
    /// it, and a scene lit only by a key light hides half of one in black. An
    /// interior wants it for the honest reason — a room is not lit by a sky it
    /// cannot see.
    ///
    /// It overrides rather than adds, and it stays pinned until ClearAmbient().
    void SetAmbient(const Assisi::Math::Color3<Assisi::Math::ColorSpace::Linear>&color, float intensity)
    {
        _ambient = AmbientOverride{.active = true, .color = color, .intensity = intensity};
    }

    /// @brief Give the scene back its own indirect lighting: a sky lights what
    /// is under it, and a scene without one keeps the flat default.
    void ClearAmbient() { _ambient = AmbientOverride{}; }

    [[nodiscard]] bool AmbientOverridden() const { return _ambient.active; }
    [[nodiscard]] Assisi::Math::Color3<Assisi::Math::ColorSpace::Linear>AmbientColor() const { return _ambient.color; }
    [[nodiscard]] float AmbientIntensity() const { return _ambient.intensity; }

    /// @brief Whether the sky is reflected, and how finely. Applied on the next
    /// Render().
    ///
    /// Off lights a sky's scene exactly as it was lit before there was a probe
    /// — hemisphere diffuse, no environment specular — and holds no texture of
    /// it. A scene with no sky, or with a pinned ambient, never has a probe
    /// whatever these say.
    void SetEnvironmentSettings(const Render::EnvironmentSettings &settings)
    {
        _environmentSettings = Render::Sanitized(settings);
    }
    [[nodiscard]] const Render::EnvironmentSettings &EnvironmentSettings() const { return _environmentSettings; }

    /// @brief Whether screen-space occlusion darkens the indirect term, and how.
    /// Applied on the next Render().
    ///
    /// Off draws the frame there was before it existed — no depth prepass, the
    /// lit pass through its own pipelines — and holds no texture of it.
    void SetSsaoSettings(const Render::SsaoSettings &settings) { _ssaoSettings = Render::Sanitized(settings); }
    [[nodiscard]] const Render::SsaoSettings &SsaoSettings() const { return _ssaoSettings; }

    /// @brief The probe as the most recent Render() left it: its face size and
    /// mips (both zero while it holds nothing), and how its bakes have gone.
    [[nodiscard]] const Render::SkyProbe &SkyProbe() const { return _skyProbe; }

    /// @brief The shadow knobs, in both halves. Applied on the next Render():
    /// a cascade count, resolution or format change reallocates the array
    /// there, and everything else rides into the shader as a frame constant.
    ///
    /// Nothing is allocated until a shadow-casting directional light exists, so
    /// a scene with no sun in it pays neither the memory nor the pass whatever
    /// these say.
    void SetShadowSettings(const Render::ShadowSettings &settings) { _shadowSettings = settings; }
    [[nodiscard]] const Render::ShadowSettings &ShadowSettings() const { return _shadowSettings; }

    /// @brief Which shadow diagnostic the mesh shader draws over the lit image.
    /// Runtime only, and off by default — every one of these changes the picture.
    void SetShadowDebugView(Render::ShadowDebugView view) { _shadowDebugView = view; }
    [[nodiscard]] Render::ShadowDebugView ShadowDebugView() const { return _shadowDebugView; }

    /// @brief What the shadow pass drew in the most recent Render(); all zero
    /// when nothing casts. Read against the draw stats: cascades at 0 with a sun
    /// in the scene means the pass is inactive, not that it found nothing.
    [[nodiscard]] Render::ShadowPass::Stats LastShadowStats() const { return _lastShadowStats; }

    /// @brief Caster-cascade pairs the most recent sun gather drew one level
    /// coarser than on screen. Zero on a frame that redrew no cascade.
    [[nodiscard]] std::uint32_t LastShadowLodCoarser() const { return _shadowCasters.coarserViews; }

    /// @brief What the local-light atlas drew in the most recent Render().
    ///
    /// `droppedByCap` against `unserved` is the reading that matters when a lamp
    /// has no shadow: the first says the importance cap turned it away, the
    /// second says the atlas had no room. They are different settings.
    [[nodiscard]] Render::LocalShadowPass::Stats LastLocalShadowStats() const { return _lastLocalShadowStats; }

    /// @brief Which light holds which atlas tile and how long its still layer
    /// has stood, for the atlas inspector. Empty while tiles are not cached.
    [[nodiscard]] std::span<const Render::LocalShadowCache::Residency> CachedShadowTiles() const
    {
        return _localShadowPass.CachedTiles();
    }

    /// @brief Shadowed local lights the importance cap turned away in the most
    /// recent Render(). Zero is what "the cap does not bind here" looks like.
    [[nodiscard]] uint32_t LastShadowDroppedByCap() const { return _lastSelection.droppedByCap; }

    /// @brief Gather what became of each shadow-casting light every frame, for
    /// an editor that is showing it.
    ///
    /// Off by default and off in a game, and the gate is the whole design: the
    /// report is one row per shadow-casting light in the scene, which is work
    /// proportional to content that nobody is reading. A closed panel pays
    /// nothing, the same rule an unplaced feature keeps.
    void SetShadowDiagnosticsEnabled(bool enabled)
    {
        _shadowDiagnosticsEnabled = enabled;
        // The sun's half is gathered inside its pass, where the draw list it
        // reads lives and dies within the call — so the pass is told directly
        // rather than asked afterwards.
        _shadowPass.SetCascadeCountsEnabled(enabled);
        if (!enabled)
        {
            _shadowDiagnostics.Clear();
        }
    }
    [[nodiscard]] bool ShadowDiagnosticsEnabled() const { return _shadowDiagnosticsEnabled; }

    /// @brief What the shadow system did to each light in the most recent
    /// Render(). Empty unless SetShadowDiagnosticsEnabled(true) was called.
    [[nodiscard]] const Render::ShadowDiagnostics &ShadowDiagnostics() const { return _shadowDiagnostics; }

    /// @brief The report for the light on @p entity, or null when it carries no
    /// shadow-casting local light or nothing gathered this frame.
    ///
    /// The join the Render module cannot make: it names lights by their buffer
    /// row, and an inspector has only the entity that placed one.
    [[nodiscard]] const Render::LocalShadowLightReport *ShadowReportFor(ECS::Entity entity) const;

    /// @brief Drawn/culled counts from the most recent Render(); zero before the
    /// first frame. Reflects whether culling is actually removing anything.
    [[nodiscard]] DrawStats LastDrawStats() const { return _lastDrawStats; }

    /// @brief Whether the moon's albedo photograph loaded.
    ///
    /// Surfaced because the failure is invisible: a failed load leaves a flat
    /// white disk, which is a perfectly plausible-looking moon.
    [[nodiscard]] Render::SkyPass::MoonTexture MoonTextureState() const { return _skyPass.MoonTextureState(); }

    /// @brief What the sky resolved to on the most recent Render().
    ///
    /// One frame behind for anything reading it outside Render — which is what
    /// the inspector and the gizmos want, and what the shadow verdict already
    /// does. The point is that a panel showing where the sun is shows where the
    /// renderer actually put it, rather than re-deriving it and being able to
    /// disagree.
    [[nodiscard]] const SkyResolution &LastSky() const { return _lastSky; }

    /// @brief Forget everything keyed to the scene that was here.
    ///
    /// A level load and leaving play both replace the scene wholesale, and every
    /// cascade then holds depth of geometry that no longer exists under a sun
    /// that may be somewhere else entirely. One frame of today's cost, at exactly
    /// the moment a spike is invisible.
    void OnSceneReplaced();

private:
    /// @brief Take this frame's moved casters off the Transform pool's change
    /// ticks, and advance the shadow frame counter.
    ///
    /// Once per frame and before either shadow half, because both are
    /// invalidated against the result and the ticks are a cursor: a second read
    /// comes back empty and would tell the second half that nothing moved.
    void UpdateShadowMovers(ECS::Scene &scene);

    /// @brief Redraw every kept cascade and atlas tile on the next frame.
    void ForgetKeptShadows();

    /// @brief Hold the sky probe for @p sky, baking it into @p frame's command
    /// list when the sky has moved enough to see, and point the mesh pass at
    /// the result. Releases it while there is no sky to reflect, a pinned
    /// ambient, or the setting is off.
    ///
    /// Before UpdateFrameConstants, which is told through the returned value
    /// whether an environment answers this frame.
    [[nodiscard]] SpecularProbe UpdateSkyProbe(const Render::RenderFrame &frame, const SkyResolution &sky);

    /// @brief Hold the prepass pipelines for @p frame.
    ///
    /// Every frame with a depth target draws one, because the lit pass then
    /// shades each pixel once instead of once per overlapping surface: shading
    /// runs the whole light loop and every shadow filter, and re-rasterising the
    /// scene as depth alone costs far less than the overdraw it removes. If the
    /// pipelines fail to build, the frame is lit directly and it is said once.
    ///
    /// @return whether this frame draws a depth prepass.
    [[nodiscard]] bool PrepareDepthPrepass(const Render::RenderFrame &frame);

    /// @brief Hold the scene distance target and occlusion's targets for
    /// @p frame, and point the mesh pass at the result. Releases all of it while
    /// the setting is off or there is no @p prepass to read depth from, and turns
    /// the setting off if any of it fails.
    ///
    /// @return whether this frame runs occlusion.
    [[nodiscard]] bool PrepareScreenOcclusion(const Render::RenderFrame &frame, bool prepass);

    /// @brief Fit the sun's cascades and fill them, before the mesh pass reads
    /// them. Returns what the mesh shader needs to sample the result — a null
    /// fit when nothing casts, which is what makes the lookup free.
    ///
    /// Called after LightingSystem::Gather, which is where the shadow-casting
    /// sun comes from, and before UpdateFrameConstants, which borrows the fit.
    [[nodiscard]] Render::MeshPass::ShadowFrameData RenderSunShadows(const Render::RenderFrame &frame,
                                                                     ECS::Scene &scene, const Camera &camera,
                                                                     const glm::mat4 &view);

    /// @brief Choose which local lights hold atlas tiles, draw their faces, and
    /// stamp each winner's view index into the light record the shader reads.
    ///
    /// Between LightingSystem::Gather and LightingSystem::Upload, which is the
    /// only window in which that stamp reaches the GPU. Fills in @p shadows'
    /// local half.
    void RenderLocalShadows(const Render::RenderFrame &frame, ECS::Scene &scene, const Camera &camera,
                            const Transform &cameraTransform, Render::MeshPass::ShadowFrameData &shadows);

    /// @brief Join this frame's selection, allocation and budget into one row
    /// per shadow-casting light. A no-op unless an editor asked for it.
    ///
    /// After both shadow passes have drawn, because it reports on both: the
    /// per-cascade caster counts come from the sun's stats and everything else
    /// from the atlas's.
    void BuildShadowDiagnostics();

    /// @brief What fraction of the screen's height a light of @p range at
    /// @p position spans, from a camera at @p cameraPosition.
    ///
    /// The selector's coverage input, and the one place the camera enters the
    /// ordering. A light the camera stands inside saturates at 1.
    [[nodiscard]] static float LocalLightScreenCoverage(const glm::vec3 &position, float range,
                                                        const glm::vec3 &cameraPosition, float tanHalfFovY);

    /// @brief The view LOD selection measures against this frame.
    ///
    /// Derived once and handed to every consumer rather than re-derived per
    /// pass: the draw path and the two shadow gathers select the same instance
    /// in the same frame, and a difference in what they measured with would put
    /// them on different levels.
    [[nodiscard]] static LodView CameraLodView(const Transform &cameraTransform, const Camera &camera);

    /// @brief Rebuild the froxel grid on its own command list (setup/resize path).
    void RebuildClusterGrid(int32_t width, int32_t height, const Camera &camera, const glm::mat4 &projection);

    nvrhi::IDevice *_device = nullptr;
    LightingSystem _lighting;
    Render::MeshPass _meshPass;
    // GPU-driven cull (stage F1): the compute cull pass + the reused host-side
    // table builder it uploads from. Initialized alongside the mesh pass; the draw
    // path uses them only when _gpuCulling is on (else the CPU path runs).
    Render::MeshCuller _meshCuller;
    Render::CullTableBuilder _cullBuilder;
    // Every shadow map's depth drawing, and the sun's cascades over it. The
    // renderer is separate because the local-light atlas draws through the same
    // one, into the same view table, from the same instance buffer.
    Render::ShadowDepthRenderer _shadowDepthRenderer;
    // The fit is a member because UpdateFrameConstants borrows it after the
    // pass has drawn with it.
    Render::ShadowPass _shadowPass;
    Render::CascadeFit _cascadeFit;
    ShadowCasterGather _shadowCasters;
    // Which cascades still hold the right depth, and this frame's answer. The
    // plan is a member so a steady state allocates nothing, and because the fit
    // it publishes is what the mesh pass borrows.
    Render::SunShadowCadence _sunCadence;
    Render::SunShadowCadencePlan _sunCadencePlan;
    // The cascade allocation the kept slices belong to. A different one means
    // they hold depth of a texture that no longer exists.
    std::uint32_t _cascadeGeneration = 0;
    // The clock's cut count as of the last frame drawn. A different one means the
    // sun teleported and no kept cascade describes where it is now.
    std::uint32_t _jumpSerial = 0;
    // What the sky resolved to on the last frame, for the panels and gizmos that
    // must show what the renderer used rather than re-deriving it.
    SkyResolution _lastSky;
    // The local-light half: the shared atlas, who gets a tile in it, and the
    // casters each tile-holder reaches. Kept as members so a steady state
    // allocates nothing.
    Render::LocalShadowPass _localShadowPass;
    Render::LocalShadowSelector _localShadowSelector;
    Render::LocalShadowSelection _lastSelection;
    std::vector<Render::LocalShadowCandidate> _localCandidates;
    std::vector<Render::LocalShadowRequest> _localRequests;
    std::vector<Geometry::BoundingSphere> _localLightVolumes;
    LocalShadowCasterGather _localShadowCasters;
    // The cached half: which casters are moving, and the two sets the atlas is
    // reconciled against each frame.
    Render::ShadowCasterMobility _casterMobility;
    std::vector<ECS::Entity> _movedEntities;
    std::vector<Render::ShadowMover> _movedCasters;
    std::vector<Render::ShadowMover> _dynamicCasters;
    std::vector<Render::ShadowMover> _casterInvalidations;
    // The scene tick the mover set was last taken at. Everything written after
    // it has moved since, which is the whole of the invalidation input.
    uint64_t _lastMoverTick = 0;
    // Entities that lost their Transform or MeshRenderer since the last look,
    // kept for the capacity.
    std::vector<ECS::Entity> _removedEntities;
    // Counts frames for the atlas's throttle phase and its tile ages. Its own
    // counter rather than the scene's tick, which advances per write.
    std::uint32_t _shadowFrameIndex = 0;
    // Drawn after the opaque geometry, into the pixels it left at the depth
    // clear. See SkyPass for why that ordering is the cheap one.
    Render::SkyPass _skyPass;
    // The sky captured and prefiltered for reflection, through _skyPass's own
    // shader. Holds nothing while there is no sky.
    Render::SkyProbe _skyProbe;
    Render::EnvironmentSettings _environmentSettings;
    // The depth prepass's depth as a distance, for every screen-space feature.
    // Holds nothing while occlusion, the only one, is off.
    Render::SceneDistancePass _sceneDistancePass;
    // Reads that distance, between the prepass and the lit pass. Holds nothing
    // while the setting is off.
    Render::SsaoPass _ssaoPass;
    Render::SsaoSettings _ssaoSettings;

    // Projection the froxel grid was last built against; a mismatch in Render()
    // triggers a rebuild. Identity forces one on the first frame.
    glm::mat4 _clusterProjection{1.f};

    bool _frustumCulling = true; // default draw path culls off-screen meshes
    bool _sortDraws = true;      // default draw path sorts by sort key before submit
    bool _gpuCulling = false;    // GPU-driven cull path; CPU path is the default reference
    // The chosen level per entity, and the knobs that chose it. Shared by the
    // draw path, both shadow gathers and the selection outline, so every one of
    // them draws the same instance as the same geometry.
    LodSelector _lodSelector;

    /// Whether the GPU cull is what draws the scene: asked for, and able to.
    /// DrawScene falls back to the CPU path otherwise, and whatever follows the
    /// path that drew has to follow the fallback too.
    [[nodiscard]] bool GpuCullDraws() const { return _gpuCulling && _meshCuller.IsValid(); }
    Render::MaterialDebugView _debugView = Render::MaterialDebugView::None; // material-channel debug visualization
    AmbientOverride _ambient;                                               // inactive: the scene lights itself
    Render::ShadowSettings _shadowSettings;                                 // the sun's cascade knobs
    // Latched so the unsupported-scene warning is said once rather than at the
    // frame rate. Cleared when the scene stops having several suns, so fixing it
    // and breaking it again is reported both times.
    bool _multipleSunsWarned = false;
    // Set once the prepass pipelines have failed to build, so the failure is
    // said once and every later frame is lit directly without retrying.
    bool _prepassFailed = false;
    Render::ShadowDebugView _shadowDebugView = Render::ShadowDebugView::None;
    DrawStats _lastDrawStats;                             // drawn/culled from the last Render(), for the overlay
    Render::ShadowPass::Stats _lastShadowStats;           // what the shadow pass drew, for the same overlay
    Render::LocalShadowPass::Stats _lastLocalShadowStats; // the same for the local-light atlas
    // Per-light shadow outcomes for an editor readout. Gathered only while
    // something is looking, which is what keeps a closed panel free.
    Render::ShadowDiagnostics _shadowDiagnostics;
    bool _shadowDiagnosticsEnabled = false;

    // Change-detection bookmark for PropagateTransforms used by the single-scene
    // Render() overload: the scene tick at the end of the last propagation. 0
    // forces a full recompute on the first frame. Apps with several worlds
    // resident pass their own per-world bookmark instead.
    uint64_t _lastPropagationTick = 0;
};

} // namespace Assisi::Runtime
