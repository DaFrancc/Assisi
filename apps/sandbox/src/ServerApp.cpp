/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "ServerApp.hpp"

#include <Assisi/App/BlueprintReplication.hpp>
#include <Assisi/App/LevelRuntime.hpp>
#include <Assisi/App/SystemCatalog.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/ContentHash.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Transform.hpp>
#if defined(ASSISI_NETWORKING)
#    include <Assisi/NetSync/NetComponents.hpp>
#endif

#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Sandbox
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

} // namespace

ServerApp::ServerApp(ServerOptions options) : _options(std::move(options))
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
    NetSync::LoadQuantizationFromConfig();
    NetSync::LoadSmoothingFromConfig();
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
            Log::Error("Server: refusing '{}' — it names a system this build does not declare.",
                       _options.level);
            _startupFailed = true;
            RequestClose();
            return;
        }

        if (!Assisi::App::LoadLevelSim(_world, _options.level))
        {
            Log::Error("Server: failed to load level '{}'.", _options.level);
            _startupFailed = true;
            RequestClose();
            return;
        }
        Log::Info("Server: loaded '{}'.", _options.level);
    }

    if (_options.role == ServerRole::Offline)
    {
        if (_options.level.empty())
            Log::Info("Server: no level requested — simulating an empty world.");
        return;
    }

#if !defined(ASSISI_NETWORKING)
    // A networked role in a build configured without networking. Refused out
    // loud rather than quietly degraded to Offline: a --host that hosts nothing
    // is a worse outcome than one that says it cannot.
    Log::Error("Server: this build was configured with ASSISI_ENABLE_NETWORKING=OFF, so --host and "
               "--connect do nothing. Reconfigure with networking on, or use --server for headless "
               "simulation.");
    _startupFailed = true;
    RequestClose();
    return;
#else
    NetSync::ReplicationConfig config;
    config.tickRateHz = static_cast<std::uint32_t>(GetConfig().physicsHz);
    _session          = std::make_unique<NetSync::NetSession>(_world.scene, &_world.physics, config);

    // Triggered by hosting or joining, never by the level load above: a process
    // that only simulates has nothing to compare with anybody.
    _contentSetHash.Start(Jobs());

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
        if (!_session->Host(_options.port, std::move(level)))
        {
            _startupFailed = true;
            RequestClose();
            return;
        }

        // A demo world that actually moves. Delta replication is only
        // interesting against change; a static level converges once and then
        // proves nothing for the rest of the run.
        for (std::uint32_t i = 0; i < _options.spawnCount; ++i)
        {
            const ECS::Entity entity = _world.scene.Create();
            ECS::Transform transform;
            transform.position = {static_cast<float>(i) * 2.f, 0.f, 0.f};
            (void)_world.scene.Add<ECS::Transform>(entity, transform);
            (void)_world.scene.Add<NetSync::Replicated>(entity, NetSync::Replicated{});
            _moving.push_back(entity);
        }

        Log::Info("Server: listening on port {} ({} replicated entities).", _options.port, _moving.size());
    }
    // Deferred, exactly like the editor's join: this process has a level to load
    // before a NetId has anywhere to land.
    else if (!_session->Join(_options.address, _options.port, /*deferHandshake=*/ true))
    {
        _startupFailed = true;
        RequestClose();
    }
#endif // ASSISI_NETWORKING
}

void ServerApp::BuildJoinedWorld()
{
#if defined(ASSISI_NETWORKING)
    const NetSync::ServerHello *hello = _session->Handshake();
    if (hello == nullptr)
        return;

    // Every way a join can be refused funnels through here, so the exit code is
    // set once rather than at each caller. A client that could not join never
    // started, whatever the reason.
    const auto fail = [this](std::string reason)
                      {
                          Log::Error("Client: join failed — {}", reason);
                          _session->AbortJoin(std::move(reason));
                          _startupFailed = true;
                          RequestClose();
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

    if (!Assisi::App::LoadLevelSim(_world, hello->level.path))
    {
        fail("'" + hello->level.path + "' failed to load.");
        return;
    }

    // The host owns these; they arrive as mirrors. The file's copies are the
    // host's authored originals, and keeping both would double the world.
    // LoadLevelSim already built their bodies, which is why the shared strip has
    // to take them out of the physics world rather than only ending the entities.
    const Assisi::App::StrippedEntities stripped =
        Assisi::App::StripReplicatedEntities(_world.scene, _world.physics);

    _session->ConfirmLevelReady();
    Log::Info("Client: built '{}' ({} replicated entities stripped, {} orphan links dropped) and answered the "
              "handshake.",
              hello->level.path, stripped.entities, stripped.orphans);
#endif // ASSISI_NETWORKING
}

void ServerApp::OnFixedUpdate(float dt)
{
    // Take input and acks before simulating, so a command that arrived for this
    // tick is applied on this tick rather than the next one.
#if defined(ASSISI_NETWORKING)
    if (_session)
    {
        // Before Poll, so a hello that has been waiting on the scan goes out on
        // the same tick the scan finished rather than the next one.
        if (Assisi::App::ContentSet content; _contentSetHash.Poll(content))
        {
            Assisi::Core::Log::Info("Content set hashed: {} ({} files).", Assisi::Core::ToHex64(content.hash),
                                    content.paths.size());
            // The same call the windowed path makes, for the same reason: a
            // dedicated server is where instances most want to arrive as
            // instances. Nothing here is presentation.
            Assisi::App::ApplyContentSet(*_session, _world, std::move(content));
        }

        _session->Poll();

        // The handshake named a level; build it before answering. Nothing here
        // is GPU-bound, so unlike the editor this needs no marshalling.
        if (_session->IsAwaitingLevel())
            BuildJoinedWorld();

        // A host that went away, or a rejected handshake: Poll turns both into
        // an Offline session, and a client with no stream has nothing to do.
        if (_options.role == ServerRole::Client && !_session->IsActive())
        {
            Log::Warn("Client: session ended ({}).",
                      _session->LastError().empty() ? "closed by the host" : _session->LastError());
            RequestClose();
            return;
        }
    }
#endif // ASSISI_NETWORKING

    _world.physics.Update(dt);

    // Immediately after the step, before the snapshot below: a mirror woken by a
    // contact the server never had has to be put back before anything reads it.
#if defined(ASSISI_NETWORKING)
    if (_session)
        _session->AfterPhysicsStep();
#endif

    // Move the demo world. Writes go through GetMut because that is what stamps
    // the change tick the delta is computed from — a write through a plain
    // Get would replicate nothing and report no error.
    const auto phase = static_cast<float>(GetSimTick()) * 0.02f;
    for (std::size_t i = 0; i < _moving.size(); ++i)
    {
        if (ECS::Transform *transform = _world.scene.GetMut<ECS::Transform>(_moving[i]))
            transform->position.y = std::sin(phase + static_cast<float>(i));
    }

    // After the simulation: a snapshot describes the world at the end of the
    // tick it is stamped with. A headless client has no devices to sample, so
    // it sends an empty command — enough to exercise the input path and give
    // the server something to measure its buffer depth against.
#if defined(ASSISI_NETWORKING)
    if (_session)
        _session->Tick(GetSimTick());
#endif

    if (_options.tickLimit > 0 && GetSimTick() >= _options.tickLimit)
        RequestClose();
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
    if (_session)
        _session->SmoothView(dt);
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
    if (!_session || !_session->IsActive())
    {
        Log::Info("Server: tick {} ({:.1f} Hz measured)", tick, tickRate);
        return;
    }

    const NetSync::SessionStats stats = _session->Stats();
    if (_session->IsHost())
    {
        Log::Info("Server: tick {} ({:.1f} Hz), {} client(s), {} snapshots, {} bytes sent", tick, tickRate,
                  stats.clientCount, stats.snapshotsSent, stats.bytesSent);
    }
    else
    {
        Log::Info("Client: tick {} ({:.1f} Hz), {}, {} entities, {} applied, {} rejected, server tick {}", tick,
                  tickRate, _session->StatusText(), stats.replicatedEntities, stats.snapshotsApplied,
                  stats.snapshotsRejected, stats.serverTick);
    }
#else
    // Offline is the only role this build has, so there is one line to print.
    Log::Info("Server: tick {} ({:.1f} Hz measured)", tick, tickRate);
#endif
}

void ServerApp::FlushDeferred()
{
    // End of frame: apply entities queued by Scene::Destroy() this tick. The
    // windowed path does this too — deferred destruction is a scene invariant,
    // not a rendering one.
    _world.scene.FlushDestroyed();
}

void ServerApp::InstallQueuedSystems()
{
    // Same reason, same frame position as the windowed path: a blueprint arriving
    // from the wire asks for the systems its file names, and they have to be
    // registered at the safe point rather than mid-walk. A headless process runs
    // that behaviour — it just does not draw it.
    Assisi::App::DrainSystemInstalls(_world);
}

void ServerApp::OnShutdown()
{
#if defined(ASSISI_NETWORKING)
    if (_session && _session->IsClient())
    {
        const NetSync::SessionStats stats = _session->Stats();
        Log::Info("Client: stopped after {} ticks — {} snapshots applied, {} rejected, {} entities mirrored.",
                  GetSimTick(), stats.snapshotsApplied, stats.snapshotsRejected, stats.replicatedEntities);
    }
    else
#endif
    {
        Log::Info("Server: stopped after {} ticks.", GetSimTick());
    }

#if defined(ASSISI_NETWORKING)
    // Before the scene and physics world it holds a reference to.
    _session.reset();
#endif
}

} // namespace Sandbox
