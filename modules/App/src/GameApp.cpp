/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/GameApp.hpp>

#include <Assisi/App/InputSetup.hpp>
#include <Assisi/App/LevelRuntime.hpp>
#include <Assisi/App/SceneCamera.hpp>
#include <Assisi/App/StartupScene.hpp>
#include <Assisi/App/SystemCatalog.hpp>
#include <Assisi/Chiara/Profile.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/ConfigReader.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Mondrian/FontReader.hpp>
#include <Assisi/Mondrian/ScreenReader.hpp>
#include <Assisi/Render/RenderSystem.hpp>
#include <Assisi/Render/Vulkan/VulkanContext.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/CookedScene.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>
#include <Assisi/Window/Key.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <system_error>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

namespace Assisi::App
{
namespace
{
// Per-frame ceiling on draining decoded assets to the GPU. A publish does real
// GPU work — createTexture, writeTexture, a blocking submit — so a whole burst
// landing in one frame hitches it. The game has no background preload to protect,
// so it takes the generous tier the editor reserves for a foreground load.
constexpr double kAssetPublishBudgetMs = 10.0;
constexpr std::uint64_t kAssetPublishBudgetBytes = 128ull << 20;

/// The MSAA sample count a benchmark renders at.
constexpr std::int32_t kBenchmarkMsaaSamples = 8;

/// OptionsConfig::fpsLimit's value for no cap.
constexpr std::int16_t kUnlimitedFps = -1;

[[nodiscard]] double MonotonicSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

GameApp::GameApp(GameLaunch launch) : _launch(std::move(launch))
{
}

GameApp::~GameApp()
{
    // The readers reach members of this object; nothing may call them after it.
    if (_assetSource && Render::GetAssetSource() == &*_assetSource)
    {
        (void)Render::SetAssetSource(nullptr);
    }
    if (_pak)
    {
        (void)Runtime::SceneSerializer::SetDocumentReader({});
        (void)Core::SetConfigReader({});
        (void)Mondrian::SetFontReader({});
        (void)Mondrian::SetScreenReader({});
    }
}

bool GameApp::MountContent()
{
    std::filesystem::path pak = _launch.pak;
    if (pak.empty())
    {
        const std::optional<std::filesystem::path> executable = Core::AssetSystem::ExecutablePath();
        pak = executable ? executable->parent_path() / kDefaultPakName : std::filesystem::path{kDefaultPakName};
    }

    std::expected<Core::PakProvider, Core::AssetError> mounted = Core::PakProvider::Mount(pak);
    if (!mounted)
    {
        Core::Log::Error("Game: cannot start - the content package '{}' cannot be read ({}).", pak.string(),
                         Core::ToString(mounted.error()));
        return false;
    }
    Core::Log::Info("Game: reading content from '{}'.", pak.string());

    _pak.emplace(std::move(*mounted));
    _assetSource.emplace(*_pak);

    const Core::PakProvider &provider = *_pak;
    (void)Render::SetAssetSource(&*_assetSource);
    (void)Runtime::SceneSerializer::SetDocumentReader([&provider](std::string_view vpath)
                                                      { return Runtime::ReadCookedDocument(provider, vpath); });
    (void)Core::SetConfigReader([&provider](std::string_view vpath, std::type_index type, void *instance)
                                { return Core::ReadCookedConfig(provider, vpath, type, instance); });
    (void)Mondrian::SetFontReader([&provider](std::string_view vpath)
                                  { return Mondrian::ReadCookedFont(provider, vpath); });
    // The cooked blob and nothing else. A game has no parser to fall back to,
    // which is the point: the markup never reaches the shipped binary.
    (void)Mondrian::SetScreenReader([&provider](std::string_view vpath)
                                    { return Mondrian::ReadCookedScreen(provider, vpath); });
    return true;
}

void GameApp::OnStart()
{
    // What a travel needs to turn a level file into a running world. Captured by
    // pointer; every one of these outlives the manager. The renderer is null in a
    // headless run, and WorldManager takes the render-free path when it is.
    _worlds.SetServices({.cache = &_assetCache,
                         .renderer = HasPresentation() ? &_sceneRenderer : nullptr,
                         .jobs = &Jobs(),
                         .events = &GetEvents(),
                         .input = HasPresentation() ? &GetInput() : nullptr,
                         .actions = &GetActions(),
                         .ui = GetUi()});

    // What the shipped config asked for, before the first world starts — the
    // policy has to be installed ahead of the load it governs, not after it.
    _worlds.SetSimulateFrom(GetConfig().simulateFrom);

    // Before the level load and not after it: the load publishes meshes and
    // materials into the asset cache, which the renderer owns the bindless table
    // for. Nothing to draw with means nothing to load into.
    if (HasPresentation() && !SetupRenderer())
    {
        RefuseStart();
        return;
    }
    if (_launch.benchmark && HasPresentation())
    {
        ApplyBenchmarkSettings(*_launch.benchmark);
    }

    // The shipped config is the only thing that says what to open — a game takes
    // no level argument, and only a benchmark, which a shipped build cannot ask
    // for, names one. Each way it can fail names itself, because "the game would
    // not start" sends a player looking in the wrong place.
    // A package indexes paths by hash and holds none of their text, so an id
    // cannot be turned back into the path a level is loaded by.
    const std::string_view requested = _launch.benchmark && !_launch.benchmark->level.empty()
                                           ? std::string_view(_launch.benchmark->level)
                                           : GetConfig().startupScene.View();
    const Core::PakProvider &pak = *_pak;
    const std::expected<std::string, StartupSceneError> scene = ResolveStartupScene(
        requested, [](const Core::AssetId &) { return std::optional<std::string>{}; },
        [&pak](std::string_view vpath) { return pak.Resolve(vpath).has_value(); });
    if (!scene)
    {
        Core::Log::Error("Game: cannot start - startup scene '{}': {}.", requested, Describe(scene.error()));
        RefuseStart();
        return;
    }

    // Asked before the load rather than discovered during it: a level naming
    // behaviour this build does not carry runs as scenery, which looks like a
    // game that started and plays like one that did not.
    if (!LevelSystemsAreDeclared(*scene))
    {
        Core::Log::Error("Game: cannot start - '{}' names a system this build does not declare.", *scene);
        RefuseStart();
        return;
    }

    // Straight into the configured level, with no empty world in between. The
    // editor starts its world stopped because an author is composing it; nobody
    // is composing this one, and a game that waited for a Play button would
    // never start — LoadLevel leaves what it opens active and simulating.
    _world = _worlds.LoadLevel(*scene);
    if (_world == nullptr)
    {
        Core::Log::Error("Game: cannot start - '{}' would not load.", *scene);
        RefuseStart();
        return;
    }

    if (_launch.benchmark && HasPresentation())
    {
        std::vector<Runtime::CameraRouteLeg> route = Runtime::BuildCameraRoute(_world->scene);
        if (route.empty())
        {
            Core::Log::Error("Benchmark: cannot start - '{}' has no camera paths with a length to fly.", *scene);
            RefuseStart();
            return;
        }
        Core::Log::Info("Benchmark: '{}', {} camera paths over {} s.", *scene, route.size(),
                        _launch.benchmark->seconds);
        const std::int32_t shots = _launch.benchmark->shotsDirectory.empty() ? 0 : _launch.benchmark->shotCount;
        _benchmark.emplace(std::move(route), _launch.benchmark->seconds, shots);
    }
}

void GameApp::ApplyBenchmarkSettings(const GameBenchmark &benchmark)
{
    OptionsConfig &options = GetOptions();
    options.aaMode = Render::AaMode::MSAA_FXAA;
    options.msaaSamples = kBenchmarkMsaaSamples;
    options.frameSync = FrameSyncMode::FpsLimit;
    options.fpsLimit = kUnlimitedFps;
    ApplyDisplayOptions();

    options.shadows = Render::TierSettings(Render::ShadowTier::Ultra);
    options.environment = Render::EnvironmentSettings{};
    options.ambientOcclusion = Render::SsaoSettings{};
    _sceneRenderer.SetShadowSettings(options.shadows);
    _sceneRenderer.SetEnvironmentSettings(options.environment);
    _sceneRenderer.SetSsaoSettings(options.ambientOcclusion);

    if (Render::Vulkan::VulkanContext *vulkanContext = Render::RenderSystem::GetVulkanContext())
    {
        vulkanContext->SetPassTimingEnabled(benchmark.passTiming);
    }
}

void GameApp::AdvanceBenchmark()
{
    const BenchmarkPhase before = _benchmark->Phase();
    const BenchmarkPhase phase = _benchmark->Advance(_world->start == StartProgress::Loaded, MonotonicSeconds());
    const std::string &shotsDirectory = _launch.benchmark->shotsDirectory;

    if (const std::int32_t shot = _benchmark->ShotThisFrame(); shot >= 0)
    {
        CaptureNextFrame(std::format("{}/shot-{:02}.png", shotsDirectory, shot));
    }
    if (phase == before)
    {
        return;
    }

    switch (phase)
    {
    case BenchmarkPhase::WarmingUp:
        Core::Log::Info("Benchmark: loaded; warming up for {} s.", kBenchmarkWarmupSeconds);
        break;
    case BenchmarkPhase::Running:
        if (shotsDirectory.empty())
        {
            StartChiaraSession();
        }
        else
        {
            std::error_code error;
            std::filesystem::create_directories(shotsDirectory, error);
        }
        Core::Log::Info("Benchmark: running.");
        break;
    case BenchmarkPhase::Finished:
        if (shotsDirectory.empty())
        {
            StopChiaraSession();
            Core::Log::Info("Benchmark: finished after {} s; capture written to '{}'.",
                            _benchmark->ElapsedSeconds(), LastChiaraDump().path);
        }
        else
        {
            Core::Log::Info("Benchmark: {} shots written to '{}'.", _launch.benchmark->shotCount, shotsDirectory);
        }
        RequestClose();
        break;
    case BenchmarkPhase::Settling:
    case BenchmarkPhase::Count:
        break;
    }
}

bool GameApp::SetupRenderer()
{
    Render::Vulkan::VulkanContext *vulkanContext = Render::RenderSystem::GetVulkanContext();
    if (vulkanContext == nullptr)
    {
        Core::Log::Error("Game: there is no Vulkan context to render through.");
        return false;
    }
    nvrhi::IDevice *device = vulkanContext->GetDevice();
    const auto fbSize = GetWindow().GetFramebufferSize();

    // Before the scene renderer: the asset cache owns the bindless
    // material-texture table the mesh pipeline binds.
    _assetCache.Initialize(device, &Jobs());
    // Creating a block-compressed texture on a device that did not enable the
    // feature is invalid, so the cache is told before it loads anything.
    _assetCache.SetTextureCompressionSupported(vulkanContext->SupportsTextureCompressionBc());

    if (!_sceneRenderer.Initialize({.device = device,
                                    .framebufferInfo = GetSceneFramebufferInfo(),
                                    .width = fbSize.Width,
                                    .height = fbSize.Height,
                                    .camera = _fallbackCamera,
                                    .bindlessLayout = _assetCache.BindlessLayout(),
                                    .bindlessTable = _assetCache.BindlessTable(),
                                    .materialTable = _assetCache.MaterialTableBuffer()}))
    {
        Core::Log::Error("Game: the scene render path failed to build; there is nothing to draw with.");
        return false;
    }

    // The player's saved knobs. Nothing is allocated until content needs it — no
    // shadow map until a sun loads, no probe until a sky does.
    _sceneRenderer.SetShadowSettings(GetOptions().shadows);
    _sceneRenderer.SetEnvironmentSettings(GetOptions().environment);
    _sceneRenderer.SetSsaoSettings(GetOptions().ambientOcclusion);
    return true;
}

SystemContext GameApp::WorldStartContext(World &world)
{
    // Everything a per-frame phase gets, except dt and the tick: a one-shot runs
    // outside any frame, so there is no elapsed time and no tick it belongs to.
    return {.world = world,
            .dt = 0.f,
            .simTick = 0,
            .input = HasPresentation() ? &GetInput() : nullptr,
            .actions = &GetActions(),
            .events = GetEvents(),
            .isActiveWorld = &world == _worlds.Active(),
            .worldManager = &_worlds,
            .ui = GetUi()};
}

void GameApp::StepWorlds(float dt)
{
    // The UI reports; the world is what carries it, so a system can see that it
    // is paused rather than only noticing that it stopped being run.
    if (World *const active = _worlds.Active(); active != nullptr)
    {
        active->paused = GetUi() != nullptr && GetUi()->PausesWorld();
    }

    _worlds.ForEach(
        [this, dt](World &world)
        {
            if (world.state != WorldState::Active || !world.simulate || world.paused)
            {
                return;
            }

            // Apply forces this tick, then simulate them, then react to what the
            // step actually did. The phase decides which side of the step a
            // system lands on; ordering within a phase cannot substitute for it.
            world.systems.Run(SystemPhase::FixedUpdate,
                              {world, dt, GetSimTick(), HasPresentation() ? &GetInput() : nullptr, &GetActions(),
                               GetEvents(), /*isActiveWorld=*/ &world == _worlds.Active(), &_worlds, GetUi()});

            {
                ASSISI_PROFILE_SCOPE("physics-step");
                world.physics.Update(dt);
            }
            {
                // Snapshot the new poses for OnRender to blend between. Linear in
                // the body count and separable from the solve, so a big scene can
                // say which of the two grew.
                ASSISI_PROFILE_SCOPE("physics-capture");
                world.physics.CaptureState();
            }

            world.systems.Run(SystemPhase::PostFixedUpdate,
                              {world, dt, GetSimTick(), HasPresentation() ? &GetInput() : nullptr, &GetActions(),
                               GetEvents(), /*isActiveWorld=*/ &world == _worlds.Active(), &_worlds, GetUi()});
        });
}

double GameApp::SimulationSeconds(double frameSeconds)
{
    if (!_benchmark)
    {
        return frameSeconds;
    }
    // Only forward: a shots run can step its clock back a little between the
    // approaches to two close stops, and a simulation cannot un-happen.
    const double clock = _benchmark->RunClockSeconds();
    const double advance = std::max(clock - _simulatedRunSeconds, 0.0);
    _simulatedRunSeconds = std::max(clock, _simulatedRunSeconds);
    return advance;
}

void GameApp::OnFixedUpdate(float dt)
{
    if (_world == nullptr)
    {
        return;
    }

    StepWorlds(dt);

    // A run with a tick budget ends when it is spent. Checked after the step, so
    // `--ticks 1` means one tick actually ran rather than none.
    if (_launch.tickLimit > 0 && GetSimTick() >= _launch.tickLimit)
    {
        RequestClose();
    }
}

void GameApp::OnUpdate(float dt)
{
    if (_world == nullptr)
    {
        return;
    }

    // --- The frame's safe point ---------------------------------------------
    // Travel frees GPU assets that draws already recorded still reference, and the
    // system that asked for it runs inside the walk over the worlds. So game code
    // requests and this applies, outside the walk and before this frame's draws.
    if (_worlds.HasTravelRequest())
    {
        if (World *const arrived = _worlds.ProcessTravelRequest())
        {
            _world = arrived;
            _worlds.SweepAssetCache();
        }
    }

    // Advance a background load: once its worker has deserialized, this resolves
    // and streams the new world's assets while it stays hidden.
    _worlds.PumpPendingLoad();

    if (HasPresentation())
    {
        // Drain decoded-and-waiting uploads under a per-frame budget, batched into
        // one submit, before this frame's draws so they are visible to it.
        _assetCache.PumpPublishes(kAssetPublishBudgetMs, kAssetPublishBudgetBytes);

        // Swap billboard placeholders for the real mesh and material as each
        // finishes streaming. Every resident world, or one nobody is looking at
        // keeps its placeholders forever.
        _worlds.ForEach(
            [this](World &world)
            {
                if (world.state == WorldState::Loading)
                {
                    return;
                }
                UpgradeStreamingAssets(world.scene, _assetCache, world.streamingPending);

                // Immediately after the upgrade, so the flag being read is the one
                // that pass just wrote. A world whose assets have all settled runs
                // its Loaded systems here and starts simulating if the config told
                // it to wait for them.
                SettleWorld(WorldStartContext(world), world.streamingPending);
            });
    }
    else
    {
        // Headless: nothing streams, so a world settles the moment it has begun
        // and the two one-shot phases land back to back. A dedicated server that
        // waited for assets it never loads would never start.
        _worlds.ForEach(
            [this](World &world)
            {
                if (world.state != WorldState::Loading)
                {
                    SettleWorld(WorldStartContext(world), /*assetsPending=*/ false);
                }
            });
    }

    // Worlds that simulate but are not drawn get neither the pose write-back nor
    // the transform propagation the render path performs for the world it draws.
    _worlds.ForEach(
        [this](World &world)
        {
            if (world.simulate && world.state == WorldState::Active && &world != _world)
            {
                SyncUnrenderedWorld(world);
            }
        });

    // Game logic, out of each world's own registry. Systems that consume input
    // opt out of the non-active worlds by declaring ActiveWorldOnly(): one
    // InputContext, N worlds.
    _worlds.ForEach(
        [this, dt](World &world)
        {
            if (world.state != WorldState::Active || !world.simulate)
            {
                return;
            }

            const SystemContext ctx{world,
                                    dt,
                                    GetSimTick(),
                                    HasPresentation() ? &GetInput() : nullptr,
                                    &GetActions(),
                                    GetEvents(),
                                    /*isActiveWorld=*/ &world == _worlds.Active(),
                                    &_worlds,
                                    GetUi()};
            world.systems.Run(SystemPhase::PreUpdate, ctx);
            world.systems.Run(SystemPhase::Update, ctx);
            world.systems.Run(SystemPhase::PostUpdate, ctx);
        });

    if (_benchmark)
    {
        AdvanceBenchmark();
    }
}

void GameApp::OnRender(Render::RenderFrame &frame)
{
    if (!_sceneRenderer.IsValid() || _world == nullptr)
    {
        return;
    }

    // Not while the world is paused: the pose it stopped in is authoritative
    // then, and the accumulator goes on filling and draining behind a paused
    // world, so blending would sweep between the last two poses and shake a
    // body that has stopped.
    if (!_world->paused)
    {
        // Blend physics-driven Transforms between their last two fixed-step poses,
        // so bodies move at the display's refresh rate rather than the physics
        // rate. The resolver is required: a parented body's pose comes back in
        // world space while its Transform is an offset from its parent, so without
        // it every parented body drifts by its parent's transform once per frame.
        ASSISI_PROFILE_SCOPE("physics-interpolate");
        _world->physics.InterpolateTransforms(_world->scene, GetInterpolationAlpha(),
                                              ParentWorldResolver(_world->scene));
    }

    // Before the camera is chosen, not only inside Render(): a camera is placed
    // from its *world* matrix, and one parented to a character that just moved
    // would otherwise sit where the previous frame computed — permanently a frame
    // behind whatever it is attached to.
    _world->propagationTick = Runtime::PropagateTransforms(_world->scene, _world->propagationTick);

    // A benchmark flies its route from the scene camera's lens: the route says
    // where the camera is, the level's Camera still says what it sees.
    const std::optional<SceneView> view = ActiveSceneCamera(_world->scene);
    const Runtime::Transform benchmarkPose =
        _benchmark ? Runtime::CameraTransformFor(_benchmark->Aim()) : Runtime::Transform{};
    const Runtime::Transform &pose = _benchmark ? benchmarkPose : view ? view->pose : _fallbackPose;
    const Runtime::Camera &camera = view ? view->camera : _fallbackCamera;

    // The game's own render systems, through the world's registry. After
    // propagation and before the scene draw, so the matrices they are handed are
    // the ones the frame is actually drawn with.
    if (_world->systems.HasRenderSystems())
    {
        const float aspectRatio =
            frame.height > 0 ? static_cast<float>(frame.width) / static_cast<float>(frame.height) : 1.f;
        RenderContext renderCtx{_world->scene, GetInterpolationAlpha(), Runtime::ViewMatrix(pose),
                                Runtime::ProjectionMatrix(camera, aspectRatio)};
        _world->systems.RunRender(renderCtx);
    }

    _sceneRenderer.Render(frame, _world->scene, pose, camera, _world->propagationTick);
}

void GameApp::OnResize(int32_t width, int32_t height)
{
    _sceneRenderer.Resize(width, height, _fallbackCamera);
}

void GameApp::OnRenderTargetsChanged(const nvrhi::FramebufferInfo &framebufferInfo)
{
    if (!_sceneRenderer.OnRenderTargetsChanged(framebufferInfo))
    {
        Core::Log::Error("Game: failed to rebuild the mesh pass pipeline after a render-target change.");
    }
}

void GameApp::FlushDeferred()
{
    // Apply the entities Scene::Destroy() queued this frame. Runs after the frame
    // is recorded, so a destroyed entity lives out its final frame before its
    // pools are touched — which keeps structural changes out of any mid-frame
    // Query. A world still Loading is skipped: a worker owns its scene until
    // promotion.
    _worlds.ForEach(
        [](World &world)
        {
            if (world.state != WorldState::Loading)
            {
                world.scene.FlushDestroyed();
            }
        });
}

void GameApp::InstallQueuedSystems()
{
    // Every resident world: a blueprint can be spawned into a background one, and
    // a queue nobody drains leaves an entity holding its components and running
    // none of the code.
    _worlds.ForEach([this](World &world) { DrainSystemInstalls(WorldStartContext(world)); });
}

} // namespace Assisi::App
