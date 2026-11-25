/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/GameApp.hpp>

#include <Assisi/App/ContentSet.hpp>
#include <Assisi/App/InputSetup.hpp>
#include <Assisi/App/LevelRuntime.hpp>
#include <Assisi/App/BlueprintReplication.hpp>
#include <Assisi/App/SceneCamera.hpp>
#include <Assisi/App/SessionDriver.hpp>
#include <Assisi/App/StartupScene.hpp>
#include <Assisi/App/StepWorld.hpp>
#include <Assisi/App/SystemCatalog.hpp>
#include <Assisi/App/WorldFrame.hpp>
#include <Assisi/Chiara/Profile.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/NetSync/NetworkConfig.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>
#include <Assisi/Render/RenderSystem.hpp>
#include <Assisi/Render/Vulkan/VulkanContext.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/SceneRenderer.hpp>
#include <Assisi/Runtime/SkinnedMeshPose.hpp>
#include <Assisi/Runtime/SkyResolve.hpp>
#include <Assisi/Window/Key.hpp>
#include <Assisi/Core/Format.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
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

/// The resolution a benchmark renders at: 1080p, the size most players play at,
/// and one where the per-pixel work weighs what it does in a shipped game.
constexpr std::int32_t kBenchmarkWidth = 1920;
constexpr std::int32_t kBenchmarkHeight = 1080;

/// OptionsConfig::fpsLimit's value for no cap.
constexpr std::int16_t kUnlimitedFps = -1;

[[nodiscard]] double MonotonicSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

GameApp::GameApp(GameLaunch launch)
    : _sceneRenderer(std::make_unique<Runtime::SceneRenderer>()), _launch(std::move(launch))
{
    if (_launch.benchmark)
    {
        SetExactResolution(kBenchmarkWidth, kBenchmarkHeight);
    }
}

GameApp::~GameApp()
{
    // The cache goes with this object; pictures released after it have no slot
    // to give back.
    ShowUiSurfacesThrough(nullptr);
    // The readers reach members of this object; nothing may call them after it.
    if (_assetSource && Render::GetAssetSource() == &*_assetSource)
    {
        (void)Render::SetAssetSource(nullptr);
    }
    if (_pak)
    {
        RemoveCookedReaders();
        if (ContentPackage() == &*_pak)
        {
            SetContentPackage(nullptr);
        }
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
    _collisionSource.Read(*_pak);

    (void)Render::SetAssetSource(&*_assetSource);
    LoadAssetsFrom(*_pak);
    InstallCookedReaders(*_pak);
    // A network session asks what content this build holds, and there is no
    // asset tree to ask.
    SetContentPackage(&*_pak);
    return true;
}

void GameApp::OnStart()
{
    // Before any session can exist: the quantization is inside the handshake
    // hash, and the config says whether this game connects directly at all.
    NetSync::LoadNetworkConfig();
    _sessions = std::make_unique<SessionDriver>(GetUniversalEvents(), MakeSessionHooks());

    // What a travel needs to turn a level file into a running world. Captured by
    // pointer; every one of these outlives the manager. The renderer is null in a
    // headless run, and WorldManager takes the render-free path when it is.
    _worlds.SetServices({.cache = &_assetCache,
                         .renderer = HasPresentation() ? _sceneRenderer.get() : nullptr,
                         .jobs = &Jobs(),
                         .universalEvents = &GetUniversalEvents(),
                         .input = HasPresentation() ? &GetInput() : nullptr,
                         .actions = &GetActions(),
                         .ui = GetUi(),
                         .mixer = GetMixer(),
                         .settings = GetPlayerSettings(),
                         .assets = &GetAssets(),
                         .steam = &GetSteam()});

    // What the shipped config asked for, before the first world starts — the
    // policy has to be installed ahead of the load it governs, not after it.
    _worlds.SetSimulateFrom(GetConfig().simulateFrom);
    _worlds.SetMaxPhysicsBodies(GetConfig().maxPhysicsBodies);

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
        Core::Log::Error("Game: cannot start - startup scene '{}': {}.", requested, ToString(scene.error()));
        RefuseStart();
        return;
    }

    // Asked before the load rather than discovered during it: a level naming
    // behaviour this build does not carry runs as scenery, which looks like a
    // game that started and plays like one that did not.
    if (!LevelSystemsAreDeclared(*scene))
    {
        // Why is logged above: a name this build does not declare, or a file
        // that could not be read at all.
        Core::Log::Error("Game: cannot start - '{}' was refused.", *scene);
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

SessionHooks GameApp::MakeSessionHooks()
{
    return SessionHooks{
        .currentLevel = [this] { return HostedLevel(); },
        .buildJoinedLevel = [this](const NetSync::LevelIdentity &level) { return BuildJoinedLevel(level); },
        .restoreAlone = [this] { PlayAloneAgain(); }};
}

NetSync::LevelIdentity GameApp::HostedLevel() const
{
    NetSync::LevelIdentity identity;
    if (_world == nullptr || _world->levelPath.empty())
    {
        return identity; // addressing None: nothing a joiner could load
    }
    // The engine's spelling of the hash, which a joiner compares its own against.
    const std::optional<std::uint64_t> hash = HashLevelFile(_world->levelPath);
    if (!hash)
    {
        return identity;
    }
    identity.addressing = NetSync::LevelAddressing::Virtual;
    identity.path = _world->levelPath;
    identity.contentHash = *hash;
    return identity;
}

std::expected<void, std::string> GameApp::BuildJoinedLevel(const NetSync::LevelIdentity &level)
{
    // Empty until this join changes the world, so a join refused before then
    // travels nowhere when the driver asks to play alone again.
    _aloneLevel.clear();
    if (const std::expected<void, JoinLevelError> joinable = CheckJoinLevel(level); !joinable)
    {
        return std::unexpected(JoinLevelErrorMessage(joinable.error(), level.path));
    }

    // Into the world the session plays, in place: the session holds that world,
    // so a travel, which makes a new one, would leave it holding nothing.
    _aloneLevel = _world->levelPath;
    Runtime::LevelHeader header;
    Runtime::LevelResult loaded;
    if (HasPresentation())
    {
        const AssetCacheReset reset = _worlds.Count() > 1 ? AssetCacheReset::Keep : AssetCacheReset::ClearFirst;
        loaded = LoadLevel(*_world, level.path, {_assetCache, *_sceneRenderer}, {.reset = reset, .header = &header});
    }
    else
    {
        loaded = LoadLevelSim(*_world, level.path);
        if (loaded)
        {
            std::expected<std::vector<std::string>, Runtime::LevelError> systems =
                Runtime::SceneSerializer::ReadLevelSystems(level.path);
            header.systems = systems ? std::move(*systems) : std::vector<std::string>{};
        }
    }
    if (!loaded)
    {
        return std::unexpected("it failed to load (" + std::string(Runtime::ToString(loaded.error())) + ")");
    }
    _world->levelPath = level.path;
    if (!WorldManager::ApplySystems(*_world, header.systems, level.path))
    {
        return std::unexpected(std::string("it names a system this build does not declare"));
    }

    // The host's own; they arrive as mirrors, and the file's copies would double them.
    const StrippedEntities stripped = StripReplicatedEntities(_world->scene);
    Core::Log::Info("Game: joining - built '{}' and stripped {} replicated entities.", level.path, stripped.entities);

    // The level starts as any level the game opens does.
    _world->start = StartProgress::NotBegun;
    BeginWorld(WorldStartContext(*_world), GetConfig().simulateFrom);
    return {};
}

void GameApp::PlayAloneAgain()
{
    // The world held the host's level, less what the host owned: a fresh copy of
    // the level played before is the only one this machine can play alone. A
    // travel the game asked for itself goes ahead instead. Used once: the next
    // join records its own.
    if (!_worlds.HasTravelRequest() && !_aloneLevel.empty())
    {
        _worlds.RequestTravel(_aloneLevel);
    }
    _aloneLevel.clear();
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
    options.clouds = Render::CloudTierSettings(Render::CloudTier::Ultra);
    _sceneRenderer->SetShadowSettings(options.shadows);
    _sceneRenderer->SetEnvironmentSettings(options.environment);
    _sceneRenderer->SetSsaoSettings(options.ambientOcclusion);
    _sceneRenderer->SetFogEnabled(true);
    _sceneRenderer->SetCloudQuality(options.clouds);
    _sceneRenderer->SetCloudsEnabled(true);
    _sceneRenderer->SetWaterEnabled(true);

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
        CaptureNextFrame(Core::Format("{}/shot-{:02}.png", shotsDirectory, shot));
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
            _world->systems.SetTiming(true);
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
            _world->systems.SetTiming(false);
            Core::Log::Info("Benchmark: finished after {} s; capture written to '{}'.", _benchmark->ElapsedSeconds(),
                            LastChiaraDump().path);
            ReportAnimationCost();
        }
        else
        {
            Core::Log::Info("Benchmark: {} shots written to '{}'.", _launch.benchmark->shotCount, shotsDirectory);
        }
        RequestClose();
        break;
    case BenchmarkPhase::Settling:
    case BenchmarkPhase::Count_:
        break;
    }
}

bool GameApp::MeasuringBenchmark() const
{
    return _benchmark && _benchmark->Phase() == BenchmarkPhase::Running && _launch.benchmark->shotsDirectory.empty();
}

void GameApp::ReportAnimationCost() const
{
    constexpr double kMillisecondsPerSecond = 1000.0;
    if (_benchmarkFrames == 0)
    {
        return;
    }
    const double frames = static_cast<double>(_benchmarkFrames);
    double animators = 0.0;
    double players = 0.0;
    for (const SystemTime &system : _world->systems.Timings())
    {
        if (system.name == "Animators")
        {
            animators = system.seconds;
        }
        else if (system.name == "AnimationPlayers")
        {
            players = system.seconds;
        }
    }
    // What was measured, so the times can be scaled to other content: the cost
    // grows with characters, their joints and the layers each one plays.
    uint32_t characters = 0;
    std::size_t joints = 0;
    std::size_t layers = 0;
    for (auto [entity, playback, skinned] : _world->scene.Query<Runtime::AnimationPlayback, Runtime::SkinnedPose>())
    {
        if (skinned.boundMeshId == Runtime::kUnboundMesh)
        {
            continue;
        }
        ++characters;
        joints += skinned.pose.size();
        layers += 1 + playback.request.layers.size();
    }
    const PoseTimes &stage = _benchmarkPoseTimes;
    const double total = animators + players + stage.placing + stage.posing + stage.placingAgain;
    Core::Log::Info("Benchmark: animation, milliseconds per frame over {} frames:", _benchmarkFrames);
    Core::Log::Info("Benchmark:   animators               {:.3f}", animators * kMillisecondsPerSecond / frames);
    Core::Log::Info("Benchmark:   playback                {:.3f}", players * kMillisecondsPerSecond / frames);
    Core::Log::Info("Benchmark:   transforms              {:.3f}", stage.placing * kMillisecondsPerSecond / frames);
    Core::Log::Info("Benchmark:   poses                   {:.3f}", stage.posing * kMillisecondsPerSecond / frames);
    Core::Log::Info("Benchmark:   transforms after poses  {:.3f}",
                    stage.placingAgain * kMillisecondsPerSecond / frames);
    if (characters == 0)
    {
        return;
    }
    constexpr double kMicrosecondsPerSecond = 1e6;
    const double perCharacter = total * kMicrosecondsPerSecond / frames / static_cast<double>(characters);
    Core::Log::Info("Benchmark:   {} animated characters, {:.1f} joints and {:.1f} layers each on average",
                    characters, static_cast<double>(joints) / static_cast<double>(characters),
                    static_cast<double>(layers) / static_cast<double>(characters));
    Core::Log::Info("Benchmark:   {:.2f} us per character, {:.3f} us per joint", perCharacter,
                    total * kMicrosecondsPerSecond / frames / static_cast<double>(joints));
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
    // Screens shown on surfaces in the world are read by materials through it.
    ShowUiSurfacesThrough(&_assetCache);

    if (!_sceneRenderer->Initialize({.device = device,
                                     .framebufferInfo = GetSceneFramebufferInfo(),
                                     .width = fbSize.Width,
                                     .height = fbSize.Height,
                                     .camera = _fallbackCamera,
                                     .bindlessLayout = _assetCache.BindlessLayout(),
                                     .bindlessTable = _assetCache.BindlessTable(),
                                     .materialTable = _assetCache.MaterialTableBuffer(),
                                     .assetCache = &_assetCache}))
    {
        Core::Log::Error("Game: the scene render path failed to build; there is nothing to draw with.");
        return false;
    }

    // The player's saved knobs. Nothing is allocated until content needs it — no
    // shadow map until a sun loads, no probe until a sky does.
    _sceneRenderer->SetShadowSettings(GetOptions().shadows);
    _sceneRenderer->SetEnvironmentSettings(GetOptions().environment);
    _sceneRenderer->SetSsaoSettings(GetOptions().ambientOcclusion);
    _sceneRenderer->SetFogEnabled(GetOptions().FogEnabled(GetConfig()));
    _sceneRenderer->SetCloudQuality(GetOptions().clouds);
    _sceneRenderer->SetCloudsEnabled(GetOptions().CloudsEnabled(GetConfig()));
    _sceneRenderer->SetWaterEnabled(GetOptions().WaterEnabled(GetConfig()));
    return true;
}

SystemContext GameApp::WorldContext(World &world, float dt, std::uint64_t simTick)
{
    return {.world = world,
            .dt = dt,
            .simTick = simTick,
            .input = HasPresentation() ? &GetInput() : nullptr,
            .actions = &GetActions(),
            .universalEvents = GetUniversalEvents(),
            .isActiveWorld = &world == _worlds.Active(),
            .worldManager = &_worlds,
            .ui = GetUi(),
            .mixer = GetMixer(),
            .settings = GetPlayerSettings(),
            .assets = &GetAssets(),
            .steam = &GetSteam(),
            // A headless run is a host with nobody watching: it evaluates no animation.
            .animates = HasPresentation()};
}

SystemContext GameApp::WorldStartContext(World &world)
{
    // A one-shot runs outside any frame, so there is no elapsed time and no tick
    // it belongs to.
    return WorldContext(world, 0.f, 0);
}

void GameApp::StepWorlds(float dt)
{
    // The UI reports; the world is what carries it, so a system can see that it
    // is paused rather than only noticing that it stopped being run. Never in a
    // session: a host that stopped would stop everyone's game, and a client
    // that stopped would fall behind a host that did not.
    if (World *const active = _worlds.Active(); active != nullptr)
    {
        active->paused = GetUi() != nullptr && GetUi()->PausesWorld() && !_sessions->InSession();
    }

    _worlds.ForEach(
        [this, dt](World &world)
        {
            if (world.state != WorldState::Active || !world.simulate || world.paused)
            {
                return;
            }

            _sessions->StepWorld(WorldContext(world, dt, _stepTick));
        });
}

double GameApp::SimulationSeconds(double frameSeconds)
{
    if (!_benchmark)
    {
        // A client's clock steers its lead on the host by running a little fast
        // or slow; 1 otherwise.
        return _sessions ? frameSeconds * _sessions->TimeScale() : frameSeconds;
    }
    // Only forward: a shots run can step its clock back a little between the
    // approaches to two close stops, and a simulation cannot un-happen.
    const double clock = _benchmark->RunClockSeconds();
    const double advance = std::max(clock - _simulatedRunSeconds, 0.0);
    _simulatedRunSeconds = std::max(clock, _simulatedRunSeconds);
    return advance;
}

void GameApp::OnPreUpdate(float dt)
{
    if (_world == nullptr)
    {
        return;
    }

    // In every world whose Update runs this frame.
    _worlds.ForEach(
        [this, dt](World &world)
        {
            if (world.state != WorldState::Active || !world.simulate)
            {
                return;
            }
            BeginWorldFrame(WorldContext(world, dt, _stepTick));
        });
}

void GameApp::OnFixedUpdate(float dt)
{
    if (_world == nullptr)
    {
        return;
    }

    // The wire is read first, so a command that arrived for this tick is
    // applied on it, and the session says which tick this step is.
    _stepTick = _sessions->BeginStep(GetSimTick(), /*stepping=*/ true);
    StepWorlds(dt);
    // Last: a snapshot describes the world at the end of the tick it is stamped with.
    _sessions->EndStep();

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
    // What systems asked of ctx.net, applied here for the reason travel is: a
    // join loads the host's level, freeing assets this frame's draws reference.
    _sessions->Update(*_world, dt);

    // Travel frees GPU assets that draws already recorded still reference, and the
    // system that asked for it runs inside the walk over the worlds. So game code
    // requests and this applies, outside the walk and before this frame's draws.
    if (_worlds.HasTravelRequest())
    {
        // A travel replaces the world a session plays, so it ends the session
        // first; changing level inside one is not built yet.
        if (_sessions->InSession())
        {
            _sessions->Leave();
        }
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

    // How far between its last two fixed steps each world is drawn. A world
    // that is not stepping is drawn exactly: the accumulator goes on cycling
    // behind a paused world, and blending would shake what stopped.
    _worlds.ForEach(
        [this](World &world)
        {
            const bool stepping = world.state == WorldState::Active && world.simulate && !world.paused;
            BlendWorld(world, stepping, GetInterpolationAlpha());
        });

    // After the blend writes each body's pose and before anything places from
    // it: a mirrored body's visual offset goes on top of that pose.
    _sessions->SmoothView(dt);

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

            const SystemContext ctx = WorldContext(world, dt, _stepTick);
            UpdateWorld(ctx);
            // The drawn world keeps its characters' posed vertices in the same pass.
            const bool drawn = &world == _world;
            PoseWorld(world, drawn ? &_assetCache : nullptr,
                      drawn && MeasuringBenchmark() ? &_benchmarkPoseTimes : nullptr);
            PresentWorld(ctx);
        });

    if (_benchmark)
    {
        AdvanceBenchmark();
    }
}

void GameApp::OnRender(Render::RenderFrame &frame)
{
    if (!_sceneRenderer->IsValid() || _world == nullptr)
    {
        return;
    }

    // A world that ran no Update this frame, paused, is posed here instead.
    EnsurePosed(*_world, &_assetCache);

    // Before the camera is chosen, not only inside Render(): a camera is placed
    // from its *world* matrix, and one parented to a character that just moved
    // would otherwise sit where the previous frame computed — permanently a frame
    // behind whatever it is attached to. After the pose stage it places only
    // what the Ui systems moved.
    _world->propagationTick = ECS::PropagateTransforms(_world->scene, _world->propagationTick);
    if (MeasuringBenchmark())
    {
        ++_benchmarkFrames;
    }
    ASSISI_PROFILE_COUNTER("propagate-resolved", ECS::LastPropagationResolved(_world->scene));

    // A benchmark flies its route from the scene camera's lens: the route says
    // where the camera is, the level's Camera still says what it sees.
    const std::optional<SceneView> view = ActiveSceneCamera(_world->scene);
    const ECS::WorldMatrix benchmarkWorld = _benchmark ? Runtime::CameraWorldFor(_benchmark->Aim()) : ECS::WorldMatrix{};
    const ECS::WorldMatrix &cameraWorld = _benchmark ? benchmarkWorld : view ? view->world : _fallbackWorld;
    // The fallback is held to the level's haze as the scene's own camera is.
    Runtime::Camera camera = view ? view->camera : _fallbackCamera;
    camera.farZ = Runtime::SceneDrawDistance(_world->scene, camera.farZ);

    // The game's own render systems, through the world's registry. After
    // propagation and before the scene draw, so the matrices they are handed are
    // the ones the frame is actually drawn with.
    if (_world->systems.HasRenderSystems())
    {
        const float aspectRatio =
            Runtime::AspectRatio(static_cast<int32_t>(frame.width), static_cast<int32_t>(frame.height));
        const glm::dvec3 origin = ECS::WorldPositionOf(cameraWorld);
        RenderContext renderCtx{_world->scene, GetInterpolationAlpha(),
                                Runtime::ViewMatrix(ECS::MatrixFrom(cameraWorld, origin)),
                                Runtime::ProjectionMatrix(camera, aspectRatio), origin};
        _world->systems.RunRender(renderCtx);
    }

    _sceneRenderer->SetSimulationSeconds(SimulatedSeconds());
    _sceneRenderer->SetWaterSeconds(PresentedSimulatedSeconds());
    _sceneRenderer->SetViewingCamera(view && !_benchmark ? view->entity : ECS::NullEntity);
    _sceneRenderer->Render(frame, _world->scene, cameraWorld, camera, _world->propagationTick);
    SetScenePostProcess(_sceneRenderer->ScenePostProcess());
}

void GameApp::OnRenderTargetsChanged(const nvrhi::FramebufferInfo &framebufferInfo)
{
    if (!_sceneRenderer->OnRenderTargetsChanged(framebufferInfo))
    {
        Core::Log::Error("Game: failed to rebuild the mesh pass pipeline after a render-target change.");
    }
}

void GameApp::FlushDeferred()
{
    // Every world's events end with the frame, as the universal ones just did.
    // Then apply the entities Scene::Destroy() queued this frame. Runs after the
    // frame is recorded, so a destroyed entity lives out its final frame before
    // its pools are touched — which keeps structural changes out of any
    // mid-frame Query. A world still Loading keeps its scene: a worker owns it
    // until promotion.
    _worlds.ForEach(
        [this](World &world)
        {
            const bool ticking = world.state == WorldState::Active && world.simulate && !world.paused;
            EndWorldFrame(world, GetUniversalEvents(), ticking);
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
    _worlds.ForEach([this](World &world) { DrainSystemInstallsOrAbort(WorldStartContext(world)); });
}

} // namespace Assisi::App
