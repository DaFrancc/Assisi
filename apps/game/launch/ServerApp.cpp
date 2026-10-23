/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "ServerApp.hpp"

#include <Assisi/App/BlueprintReplication.hpp>
#include <Assisi/App/LevelRuntime.hpp>
#include <Assisi/App/StepWorld.hpp>
#include <Assisi/App/SystemCatalog.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>
#if defined(ASSISI_NETWORKING)
#    include <Assisi/App/PlayerSession.hpp>
#    include <Assisi/NetSync/NetComponents.hpp>
#    include <Assisi/NetSync/NetworkConfig.hpp>
#endif

#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Game
{
namespace
{

#if defined(ASSISI_NETWORKING)
namespace Net     = Assisi::Net;
namespace NetSync = Assisi::NetSync;
#endif
namespace ECS     = Assisi::ECS;
namespace Log     = Assisi::Core::Log;

/// Seconds since the process's first call. Only the status line uses it, so a
/// monotonic clock with an arbitrary epoch is exactly right.
double NowSeconds()
{
    using Clock                            = std::chrono::steady_clock;
    static const Clock::time_point started = Clock::now();
    return std::chrono::duration<double>(Clock::now() - started).count();
}

/// How often a headless process prints a "still alive, here is the rate" line.
constexpr double kReportIntervalSeconds = 5.0;

/// The `--spawn` demo movers: a row this far apart (m), each bobbing up and
/// down once per period (s), each a fixed part of a cycle (turns) behind the
/// one before, so they never move in step.
constexpr float kDemoMoverSpacing = 2.f;
constexpr float kDemoMoverPeriodSeconds = 5.f;
constexpr float kDemoMoverPhaseStep = 0.16f;

} // namespace

ServerApp::ServerApp(ServerOptions options, const Assisi::Physics::CollisionSource &collision)
    : _options(std::move(options)), _world(collision)
{
    // Set before Initialize(), which is what the headless split requires: by the
    // time Initialize() returns, the decision to skip window/renderer bring-up
    // has already been acted on.
    SetHeadless(true);
}

ServerApp::~ServerApp() = default;

void ServerApp::OnStart()
{
    const char *roleName = _options.role == ServerRole::Host     ? "host"
                           : _options.role == ServerRole::Client ? "client"
                                                                 : "offline";
    Log::Info("Server: headless {}, {} Hz fixed step{}.", roleName, GetConfig().physicsHz,
              _options.tickLimit > 0 ? std::format(", stopping after {} ticks", _options.tickLimit) : std::string{});

#if defined(ASSISI_NETWORKING)
    // Before any session can exist: the quantization is inside the handshake
    // hash, so it has to be settled before the first hello is written.
    NetSync::LoadNetworkConfig();
    // Same reason: which bit of a player's controls is which button is in the
    // hash too, and a windowed peer has published it.
    Assisi::App::PublishPlayerActions();
#endif

    if (!_options.level.empty())
    {
        // LoadLevelSim, not LoadLevel: no asset cache, no scene renderer,
        // nothing GPU-owned to evict. Mesh and material GUIDs stay in the scene
        // as authored data for replication; the server never resolves them.
        //
        // The declaration check runs even though a headless server installs no
        // systems: a level naming a system this build does not declare is a
        // broken file, and serving it hands every client a level the host
        // itself is not running — worse than refusing, because it looks like it
        // worked.
        if (!Assisi::App::LevelSystemsAreDeclared(_options.level))
        {
            Log::Error("Server: refusing '{}' - it names a system this build does not declare.",
                       _options.level);
            RefuseStart();
            return;
        }

        if (!Assisi::App::LoadLevelSim(_world, _options.level) || !InstallLevelSystems(_options.level))
        {
            Log::Error("Server: failed to load level '{}'.", _options.level);
            RefuseStart();
            return;
        }
        Log::Info("Server: loaded '{}'.", _options.level);
    }

    // Begin and settle in one breath. A headless process streams nothing, so
    // there is no second moment to wait for — and a server that honoured a
    // config asking it to wait for assets it never loads would never start at
    // all. The policy is a presentation choice; the authoritative clock is not
    // one, since the fixed tick is what every client's simulation is pinned to.
    BeginWorld(WorldStartContext(), Assisi::App::SimulateFrom::Begin);
    SettleWorld(WorldStartContext(), /*assetsPending=*/ false);

    if (_options.role == ServerRole::Offline)
    {
        if (_options.level.empty())
            Log::Info("Server: no level requested - simulating an empty world.");
        return;
    }

#if !defined(ASSISI_NETWORKING)
    // A networked role in a build configured without networking. Refused out
    // loud rather than quietly degraded to Offline: a --host that hosts nothing
    // is a worse outcome than one that says it cannot.
    Log::Error("Server: this build was configured with ASSISI_ENABLE_NETWORKING=OFF, so --host and "
               "--connect do nothing. Reconfigure with networking on, or use --server for headless "
               "simulation.");
    RefuseStart();
    return;
#else
    NetSync::ReplicationConfig config;
    config.tickRateHz = static_cast<std::uint32_t>(GetConfig().physicsHz);
    _play             = std::make_unique<Assisi::App::NetworkedPlay>(_world, config);

    if (_options.role == ServerRole::Host)
    {
        // What joining clients are told to load. A host with no level file
        // advertises None, which a client treats as a clean abort — it has no
        // way to build the static half of the world.
        NetSync::LevelIdentity level;
        if (!_options.level.empty())
        {
            if (const std::optional<std::uint64_t> hash = Assisi::App::HashLevelFile(_options.level))
            {
                level.addressing  = NetSync::LevelAddressing::Virtual;
                level.path        = _options.level;
                level.contentHash = *hash;
            }
        }

        // Host logs its own reason (the port is taken, the transport would not come
        // up). What it cannot do is reach the exit code, and a supervisor restarting
        // a server that cannot bind is watching for exactly this.
        // Dedicated: the players who join get characters, and the server none.
        if (!_play->Host(_options.port, std::move(level), Assisi::App::HostRole::Dedicated))
        {
            RefuseStart();
            return;
        }

        SpawnDemoMovers();
        Log::Info("Server: listening on port {} ({} replicated entities).", _options.port, _options.spawnCount);
    }
    else if (!_play->Join(_options.address, _options.port))
    {
        RefuseStart();
    }
#endif // ASSISI_NETWORKING
}

void ServerApp::BuildJoinedWorld()
{
#if defined(ASSISI_NETWORKING)
    const NetSync::ServerHello *hello = _play->Session().Handshake();
    if (hello == nullptr)
        return;

    // Every way a join can be refused funnels through here, so the exit code is
    // set once rather than at each caller. A client that could not join never
    // started, whatever the reason.
    const auto fail = [this](std::string reason)
                      {
                          Log::Error("Client: join failed - {}", reason);
                          _play->Session().AbortJoin(std::move(reason));
                          RefuseStart();
                      };

    // Every question a join has to answer before touching the scene, asked in the
    // one place every target asks it.
    const std::expected<std::filesystem::path, Assisi::App::JoinLevelError> file =
        Assisi::App::ResolveJoinLevel(hello->level);
    if (!file)
    {
        fail(Assisi::App::JoinLevelErrorMessage(file.error(), hello->level.path));
        return;
    }

    if (!Assisi::App::LoadLevelSim(_world, hello->level.path) || !InstallLevelSystems(hello->level.path))
    {
        fail("'" + hello->level.path + "' failed to load.");
        return;
    }

    // The host owns these; they arrive as mirrors. The file's copies are the
    // host's authored originals, and keeping both would double the world. Their
    // bodies go with them on the world's next reconcile.
    const Assisi::App::StrippedEntities stripped =
        Assisi::App::StripReplicatedEntities(_world.scene);

    // The world was begun empty, before the join; this is its level's start.
    BeginWorld(WorldStartContext(), Assisi::App::SimulateFrom::Begin);
    SettleWorld(WorldStartContext(), /*assetsPending=*/ false);

    _play->ConfirmLevelReady();
    Log::Info("Client: built '{}' ({} replicated entities stripped, {} orphan links dropped) and answered the "
              "handshake.",
              hello->level.path, stripped.entities, stripped.orphans);
#endif // ASSISI_NETWORKING
}

bool ServerApp::InstallLevelSystems(std::string_view level)
{
    const std::expected<std::vector<std::string>, Assisi::Runtime::LevelError> names =
        Assisi::Runtime::SceneSerializer::ReadLevelSystems(level);
    return names.has_value() && Assisi::App::WorldManager::ApplySystems(_world, *names, level);
}

void ServerApp::SpawnDemoMovers()
{
    if (_options.spawnCount == 0)
    {
        return;
    }

    // Moved by the Oscillate system, inside the step like everything else that
    // moves, so the snapshot after it sees this tick's pose.
    if (!Assisi::App::SystemCatalog::Instance().Install(_world, std::vector<std::string>{"Oscillate"}, "demo movers"))
    {
        return;
    }
    for (std::uint32_t i = 0; i < _options.spawnCount; ++i)
    {
        const ECS::Entity entity = _world.scene.Create();
        const glm::vec3 origin{static_cast<float>(i) * kDemoMoverSpacing, 0.f, 0.f};
        (void)_world.scene.Add<ECS::Transform>(entity, ECS::Transform{.position = origin});
        (void)_world.scene.Add<Assisi::Runtime::Oscillator>(
            entity, Assisi::Runtime::Oscillator{.origin = origin,
                                                .axis = {0.f, 1.f, 0.f},
                                                .amplitude = 1.f,
                                                .periodSeconds = kDemoMoverPeriodSeconds,
                                                .phase = static_cast<float>(i) * kDemoMoverPhaseStep});
#if defined(ASSISI_NETWORKING)
        (void)_world.scene.Add<NetSync::Replicated>(entity, NetSync::Replicated{});
#endif
    }
}

void ServerApp::OnFixedUpdate(float dt)
{
    std::uint64_t stepTick = GetSimTick();

    // Take input and acks before simulating, so a command that arrived for this
    // tick is applied on this tick rather than the next one.
#if defined(ASSISI_NETWORKING)
    if (_play)
    {
        stepTick = _play->BeginStep(stepTick, /*stepping=*/ true);

        // The handshake named a level; build it before answering. Nothing here
        // is GPU-bound, so unlike the editor this needs no marshalling.
        if (_play->Session().IsAwaitingLevel())
            BuildJoinedWorld();

        // A host that went away, or a rejected handshake: the read turns both
        // into an Offline session, and a client with no stream has nothing to do.
        if (_options.role == ServerRole::Client && !_play->Session().IsActive())
        {
            Log::Warn("Client: session ended ({}).",
                      _play->Session().LastError().empty() ? "closed by the host" : _play->Session().LastError());
            RequestClose();
            return;
        }
    }
#endif // ASSISI_NETWORKING

#if defined(ASSISI_NETWORKING)
    if (_play)
    {
        _play->StepWorld(StepContext(dt, stepTick));
    }
    else
    {
        Assisi::App::StepWorld(StepContext(dt, stepTick));
    }
#else
    Assisi::App::StepWorld(StepContext(dt, stepTick));
#endif

    // After the simulation: a snapshot describes the world at the end of the
    // tick it is stamped with. A headless client with no player sends an empty
    // command, enough to give the host something to measure its buffer by.
#if defined(ASSISI_NETWORKING)
    if (_play)
        _play->EndStep();
#endif

    if (_options.tickLimit > 0 && GetSimTick() >= _options.tickLimit)
        RequestClose();
}

double ServerApp::SimulationSeconds(double frameSeconds)
{
#if defined(ASSISI_NETWORKING)
    if (_play)
        return frameSeconds * _play->TimeScale();
#endif
    return frameSeconds;
}

void ServerApp::OnUpdate([[maybe_unused]] float dt)
{
    // Write the render pose for remote entities. A headless client renders
    // nothing, but running it here keeps this loop the same shape as a windowed
    // one — and it is the only place a bug in it would show up in a soak.
    //
    // Its *convergence* assertions must never read these Transforms, though:
    // for a bodied mirror this adds a decaying cosmetic offset on top of the
    // physics pose, and the physics pose is the one that is authoritative.
#if defined(ASSISI_NETWORKING)
    if (_play)
        _play->Session().SmoothView(dt);
#endif

    ReportStatus();
}

void ServerApp::ReportStatus()
{
    const double now = NowSeconds();
    if (now - _lastReportSeconds < kReportIntervalSeconds)
        return;

    // Measured, not configured: this line exists to show whether the loop is
    // actually holding its tick rate.
    const std::uint64_t tick     = GetSimTick();
    const double elapsed  = now - _lastReportSeconds;
    const double tickRate = elapsed > 0.0 ? static_cast<double>(tick - _lastReportTick) / elapsed : 0.0;
    _lastReportSeconds           = now;
    _lastReportTick              = tick;

#if defined(ASSISI_NETWORKING)
    if (!_play || !_play->Session().IsActive())
    {
        Log::Info("Server: tick {} ({:.1f} Hz measured)", tick, tickRate);
        return;
    }

    const NetSync::SessionStats stats = _play->Session().Stats();
    if (_play->Session().IsHost())
    {
        Log::Info("Server: tick {} ({:.1f} Hz), {} client(s), {} snapshots, {} bytes sent", tick, tickRate,
                  stats.clientCount, stats.snapshotsSent, stats.bytesSent);
    }
    else
    {
        Log::Info("Client: tick {} ({:.1f} Hz), {}, {} entities, {} applied, {} rejected, server tick {}", tick,
                  tickRate, _play->Session().StatusText(), stats.replicatedEntities, stats.snapshotsApplied,
                  stats.snapshotsRejected, stats.serverTick);
    }
#else
    // Offline is the only role this build has, so there is one line to print.
    Log::Info("Server: tick {} ({:.1f} Hz measured)", tick, tickRate);
#endif
}

void ServerApp::FlushDeferred()
{
    // End of frame: the tick's events are over, and the entities queued by
    // Scene::Destroy() this tick are applied. The windowed path does this too —
    // deferred destruction is a scene invariant, not a rendering one.
    _world.events.Flush();
    _world.scene.FlushDestroyed();
}

void ServerApp::InstallQueuedSystems()
{
    // Same reason, same frame position as the windowed path: a blueprint arriving
    // from the wire asks for the systems its file names, and they have to be
    // registered at the safe point rather than mid-walk. A headless process runs
    // that behaviour — it just does not draw it.
    Assisi::App::DrainSystemInstalls(WorldStartContext());
}

Assisi::App::SystemContext ServerApp::WorldStartContext()
{
    // dt and the tick are zero: a one-shot runs outside any frame.
    return StepContext(0.f, 0);
}

Assisi::App::SystemContext ServerApp::StepContext(float dt, std::uint64_t tick)
{
    // No window, so no input, no action map and no UI, in every phase.
    return {.world         = _world,
            .dt            = dt,
            .simTick       = tick,
            .input         = nullptr,
            .actions       = nullptr,
            .universalEvents = GetUniversalEvents(),
            .isActiveWorld = true,
            .worldManager  = nullptr,
            .ui            = nullptr,
            .steam         = &GetSteam()};
}

void ServerApp::OnShutdown()
{
#if defined(ASSISI_NETWORKING)
    if (_play && _play->Session().IsClient())
    {
        const NetSync::SessionStats stats = _play->Session().Stats();
        Log::Info("Client: stopped after {} ticks - {} snapshots applied, {} rejected, {} entities mirrored.",
                  GetSimTick(), stats.snapshotsApplied, stats.snapshotsRejected, stats.replicatedEntities);
    }
    else
#endif
    {
        Log::Info("Server: stopped after {} ticks.", GetSimTick());
    }

#if defined(ASSISI_NETWORKING)
    // Before the scene and physics world it holds a reference to.
    _play.reset();
#endif
}

} // namespace Game
