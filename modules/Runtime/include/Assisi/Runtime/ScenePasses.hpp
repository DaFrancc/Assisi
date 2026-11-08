/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ScenePasses.hpp
/// @brief The scene's render passes, declared with APASS, and what they share.
///
/// reflectgen reads this header and writes ScenePassGraph, which records the
/// passes in the order their Reads<> and Writes<> fix (see
/// Assisi/Render/FramePass.hpp); SceneRenderer owns the graph and the objects
/// SceneImports hands it. A new pass is a struct here: what it reads and writes
/// puts it in the frame.

#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Render/CloudPass.hpp>
#include <Assisi/Render/CloudQuality.hpp>
#include <Assisi/Render/EnvironmentSettings.hpp>
#include <Assisi/Render/FramePass.hpp>
#include <Assisi/Render/LocalShadowCache.hpp>
#include <Assisi/Render/LocalShadowPass.hpp>
#include <Assisi/Render/MeshCuller.hpp>
#include <Assisi/Render/MeshPass.hpp>
#include <Assisi/Render/MeshSkinner.hpp>
#include <Assisi/Render/SceneDistancePass.hpp>
#include <Assisi/Render/ShadowCadence.hpp>
#include <Assisi/Render/ShadowCascades.hpp>
#include <Assisi/Render/ShadowDepthRenderer.hpp>
#include <Assisi/Render/ShadowImportance.hpp>
#include <Assisi/Render/ShadowPass.hpp>
#include <Assisi/Render/ShadowSettings.hpp>
#include <Assisi/Render/SkyPass.hpp>
#include <Assisi/Render/SkyProbe.hpp>
#include <Assisi/Render/SsaoPass.hpp>
#include <Assisi/Render/SsaoSettings.hpp>
#include <Assisi/Render/WaterFrame.hpp>
#include <Assisi/Render/WaterRenderer.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/IndirectResolve.hpp>
#include <Assisi/Runtime/LightingSystem.hpp>
#include <Assisi/Runtime/LodSelection.hpp>
#include <Assisi/Runtime/Renderer.hpp>
#include <Assisi/Runtime/SceneResources.hpp>
#include <Assisi/Runtime/ShadowCasterGather.hpp>
#include <Assisi/Runtime/SkyResolve.hpp>

#include <nvrhi/nvrhi.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Assisi::Runtime::Passes
{

// The shared objects and the pass vocabulary by the bare names the passes'
// signatures use, which is how reflectgen tells one resource from another.
using Render::MeshCuller;
using Render::MeshPass;
using Render::Reads;
using Render::Recorder;
using Render::SceneDistancePass;
using Render::ShadowCasterMobility;
using Render::ShadowDepthRenderer;
using Render::SkyPass;
using Render::SsaoPass;
using Render::Writes;

/// @brief The knobs the scene is drawn with, as its owner was last told them.
struct SceneSettings
{
    Render::ShadowSettings shadows;
    Render::EnvironmentSettings environment;
    Render::SsaoSettings ssao;
    Render::CloudQuality cloudQuality;
    AmbientOverride ambient;
    Render::MaterialDebugView debugView = Render::MaterialDebugView::None;
    Render::ShadowDebugView shadowDebugView = Render::ShadowDebugView::None;
    bool fogEnabled = true;
    bool cloudsEnabled = true;
    bool waterEnabled = true;
    bool frustumCulling = true;
    bool sortDraws = true;
    bool gpuCulling = false;
};

/// @brief What the scene's passes read and never write in a frame, all of it
/// decided before the first pass runs.
struct SceneFrame
{
    /// Mutable because the gathers query through it; no pass changes it.
    ECS::Scene &scene;
    const SkyResolution &sky;
    const SceneSettings &settings;
    /// The lit pass's origin, the camera; and the shadow maps', its cell's centre.
    glm::dvec3 viewOrigin{0.0};
    glm::dvec3 shadowOrigin{0.0};
    std::span<const LightingSystem::LocalLight> shadowSpots;
    std::span<const LightingSystem::LocalLight> shadowPoints;
    /// Casters moving this frame, and those whose shadows changed sides.
    std::span<const Render::ShadowMover> dynamicCasters;
    std::span<const Render::ShadowMover> casterInvalidations;
    /// The water in view, and the time its waves are drawn at on the shared
    /// clock.
    const Render::WaterFrame *water = nullptr;
    double waterSeconds = 0.0;
    ScreenOcclusion occlusion;
    Render::FogConstants fog;
    /// The camera measured from the shadow origin.
    glm::mat4 cameraInShadow{1.f};
    std::optional<LightingSystem::ShadowSun> shadowSun;
    /// The camera's lens, its far plane drawn in to the fog.
    Camera camera;
    glm::uvec4 fogVolumeLanes{0u};
    glm::vec3 fogColor{1.f};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t shadowFrameIndex = 0;
    /// Whether the level's fog is drawn.
    bool fogged = false;
    /// Whether the sky pass built, which the clouds and the probe draw through.
    bool skyAvailable = false;
    /// Whether the water reads the scene's distance this frame, which then
    /// runs whether or not ambient occlusion does.
    bool waterReadsDistance = false;
};

/// @brief One view of the scene: the camera's, and later each eye's.
struct ViewFrame
{
    glm::mat4 projection{1.f};
    glm::mat4 view{1.f};
    /// The camera measured from the lit pass's origin: its rotation alone.
    glm::mat4 cameraInView{1.f};
    /// The blueprint instance the camera belongs to, whose own hidden meshes it
    /// does not draw.
    ECS::InstanceId viewerInstance;
};

/// @brief What the scene's passes start from.
struct ScenePassInit
{
    nvrhi::IDevice *device = nullptr;
    ShadowDepthRenderer *shadowDepthRenderer = nullptr;
    /// The mesh pass's set 0, which a pipeline lighting surfaces as it does
    /// puts first.
    nvrhi::IBindingLayout *meshFrameLayout = nullptr;
};

/// @brief The objects SceneRenderer owns and several passes use.
APASSIMPORTS()
struct SceneImports
{
    MeshPass &meshPass;
    SkyPass &skyPass;
    LightingSystem &lightingSystem;
    LodSelector &lodSelector;
    ShadowDepthRenderer &shadowDepthRenderer;
    ShadowCasterMobility &shadowCasterMobility;
    MeshCuller &meshCuller;
    SceneDistancePass &sceneDistancePass;
    SsaoPass &ssaoPass;
    SceneColor &sceneColor;
    SceneDepth &sceneDepth;
    DrawStats &drawStats;
};

/// @brief Poses every skinned instance's vertices from its palette, for every
/// later pass to draw.
APASS(name = "skin")
struct SkinPass
{
    bool Initialize(const ScenePassInit &init);
    void Record(Recorder &rec, const SceneFrame &frame, Writes<PosedVertices> out);

    Render::MeshSkinner skinner;
    Render::SkinBatch batch;
};

/// @brief Rebuilds the grid of clusters the lights are sorted into, on a frame
/// whose projection differs from the one it was built for.
APASS(name = "cluster-rebuild")
struct ClusterRebuildPass
{
    void Record(Recorder &rec, const SceneFrame &frame, const ViewFrame &view, Writes<LightingSystem> out);

    /// The projection the grid was last built against; identity builds it on
    /// the first frame.
    glm::mat4 builtFor{1.f};
};

/// @brief Draws the clouds into their sky map and their shadow map.
APASS(name = "clouds")
struct CloudMapPass
{
    bool Initialize(const ScenePassInit &init);
    bool Wanted(const SceneFrame &frame) const;
    void Release();
    bool Record(Recorder &rec, const SceneFrame &frame, Writes<CloudMap> out);

    Render::CloudPass maps;
    /// What time has done to the clouds, and when it last did: it adds up a
    /// little each frame, so a speed sets how fast they move rather than where.
    Render::CloudMotion motion;
    std::optional<std::chrono::steady_clock::time_point> motionTime;
    /// Set when the textures failed to allocate; cleared by a new setting.
    bool failed = false;
};

/// @brief Captures the sky into the reflection probe when it has moved enough
/// to see.
APASS(name = "sky-probe")
struct SkyProbePass
{
    bool Initialize(const ScenePassInit &init);
    bool Wanted(const SceneFrame &frame) const;
    void Release();
    bool Record(Recorder &rec, const SceneFrame &frame, Reads<CloudMap> in, Writes<SkyReflections, SkyPass> out);

    Render::SkyProbe probe;
    bool failed = false;
};

/// @brief Fits the sun's cascades and redraws the ones whose depth went stale.
APASS(name = "shadow-cascades")
struct SunShadowPass
{
    bool Initialize(const ScenePassInit &init);
    bool Wanted(const SceneFrame &frame) const;
    void Release();
    bool Record(Recorder &rec, const SceneFrame &frame, Reads<PosedVertices> in,
                Writes<SunShadows, ShadowDepthRenderer, ShadowCasterMobility, LodSelector> out);

    Render::ShadowPass shadowPass;
    /// The fit the mesh pass samples with, kept because it borrows it.
    Render::CascadeFit fit;
    SunShadowCasterGather casters;
    /// Which cascades still hold the right depth, and this frame's answer.
    Render::SunShadowCadence cadence;
    Render::SunShadowCadencePlan plan;
    Render::ShadowPass::Stats stats;
    /// The allocation the kept slices belong to: a different one means they
    /// hold depth of a texture that is gone.
    std::uint32_t cascadeGeneration = 0;
    /// The clock's cut count as of the last frame drawn: a different one means
    /// the sun teleported.
    std::uint32_t jumpSerial = 0;
    bool failed = false;
};

/// @brief Chooses which spot and point lights hold atlas tiles, and draws the
/// faces that need it.
APASS(name = "shadow-atlas", after = "shadow-cascades")
struct ShadowAtlasPass
{
    bool Initialize(const ScenePassInit &init);
    bool Wanted(const SceneFrame &frame) const;
    void Release();
    bool Record(Recorder &rec, const SceneFrame &frame, Reads<PosedVertices> in,
                Writes<LocalShadows, ShadowDepthRenderer, ShadowCasterMobility, LodSelector> out);

    Render::LocalShadowPass atlas;
    Render::LocalShadowSelector selector;
    Render::LocalShadowSelection selection;
    std::vector<Render::LocalShadowCandidate> candidates;
    std::vector<Render::LocalShadowRequest> requests;
    std::vector<Geometry::BoundingSphere> lightVolumes;
    LocalShadowCasterGather casters;
    Render::LocalShadowPass::Stats stats;
    bool failed = false;
};

/// @brief Stamps each light with where its shadow views landed, uploads the
/// frame's lights and sorts them into the clusters they reach.
APASS(name = "lighting", base, after = "cluster-rebuild")
struct LightUploadPass
{
    void Record(Recorder &rec, const SceneFrame &frame, const ViewFrame &view, Reads<LocalShadows> in,
                Writes<LightingSystem> out);
};

/// @brief Binds what the earlier passes made into the mesh pass and writes its
/// frame constants.
APASS(name = "mesh-constants", base)
struct FrameConstantsPass
{
    void Record(Recorder &rec, const SceneFrame &frame, const ViewFrame &view,
                Reads<SunShadows, LocalShadows, SkyReflections, CloudMap, ShadowDepthRenderer, LightingSystem> in,
                Writes<FrameConstants, MeshPass> out);
};

/// @brief Culls and sorts the solid meshes in view and draws their depth; the
/// see-through ones are gathered for later.
APASS(name = "depth-prepass", base, timers = {"cull-dispatch", "depth-prepass"})
struct DepthPrepassPass
{
    void Record(Recorder &rec, const SceneFrame &frame, const ViewFrame &view, Reads<FrameConstants, PosedVertices> in,
                Writes<OpaqueDraws, BlendedDraws, SceneDepth, MeshPass, LodSelector, MeshCuller, DrawStats> out);

    /// The GPU cull's tables, rebuilt each frame it draws and kept for their
    /// capacity.
    Render::CullTableBuilder cullTables;
};

/// @brief Turns the prepass's depth into a distance per pixel.
APASS(name = "scene-distance")
struct DistancePass
{
    bool Wanted(const SceneFrame &frame) const;
    void Record(Recorder &rec, const SceneFrame &frame, const ViewFrame &view, Reads<SceneDepth> in,
                Writes<SceneDistance, SceneDistancePass> out);
};

/// @brief Works out how much ambient light the creases and corners on screen
/// let through, and blurs it.
APASS(name = "ssao", timers = {"ssao-occlusion", "ssao-blur"})
struct OcclusionPass
{
    bool Wanted(const SceneFrame &frame) const;
    void Record(Recorder &rec, const SceneFrame &frame, const ViewFrame &view, Reads<SceneDistance> in,
                Writes<Occlusion, SsaoPass> out);
};

/// @brief Shades every surface the prepass kept.
APASS(name = "draw-scene", base)
struct LitPass
{
    void Record(Recorder &rec, const SceneFrame &frame, const ViewFrame &view,
                Reads<OpaqueDraws, SceneDepth, Occlusion, FrameConstants, LightingSystem> in,
                Writes<SceneColor, MeshPass, DrawStats> out);
};

/// @brief Draws the sky into every pixel nothing covered; in a foggy level with
/// no sky, the fog's colour.
APASS(name = "sky", after = "draw-scene")
struct SkyBackgroundPass
{
    bool Wanted(const SceneFrame &frame) const;
    void Record(Recorder &rec, const SceneFrame &frame, const ViewFrame &view, Reads<SceneDepth, CloudMap> in,
                Writes<SceneColor, SkyPass> out);
};

/// @brief Brings every Ocean and Lake in view to this frame's time: their
/// waves' FFT and its mips.
///
/// On while the level holds water, in view or not, so a body's textures are
/// kept while the camera looks away; off, they are freed.
APASS(name = "water-waves")
struct WaterWavesPass
{
    bool Initialize(const ScenePassInit &init);
    bool OnTargetsChanged(const nvrhi::FramebufferInfo &targets);
    bool Wanted(const SceneFrame &frame) const;
    void Release();
    bool Record(Recorder &rec, const SceneFrame &frame, Writes<WaterFields> out);

    Render::WaterRenderer renderer;
};

/// @brief Draws the water over the finished scene and sky, before the other
/// see-through surfaces, which are usually in front of it.
APASS(name = "draw-water", after = "sky", before = "draw-blended")
struct WaterPass
{
    bool Wanted(const SceneFrame &frame) const;
    void Record(Recorder &rec, const SceneFrame &frame, const ViewFrame &view,
                Reads<WaterFields, SceneDistance, SceneDistancePass, FrameConstants, LightingSystem> in,
                Writes<SceneColor, MeshPass> out);
};

/// @brief Draws the see-through surfaces over the finished scene and sky,
/// farthest first.
APASS(name = "draw-blended", after = "sky")
struct BlendedPass
{
    void Record(Recorder &rec, const SceneFrame &frame, const ViewFrame &view, Reads<BlendedDraws, SceneDepth> in,
                Writes<SceneColor, MeshPass, DrawStats> out);
};

} // namespace Assisi::Runtime::Passes
