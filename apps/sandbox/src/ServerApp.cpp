/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "ServerApp.hpp"

#include <Assisi/App/LevelRuntime.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/ContentHash.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/NetComponents.hpp>

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

namespace Net     = Assisi::Net;
namespace NetSync = Assisi::NetSync;
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

/// Resolves @p virtualPath and hashes it the way every peer must, or nullopt if
/// it cannot be resolved or read.
///
/// The normalisation lives in Core, not here. This function used to hash raw
/// bytes while the editor's copy folded CRLF, so an editor host and this server
/// refused each other over the same CRLF-checked-out level — on the same
/// machine. Two spellings of a hash that peers compare is the bug, not the
/// duplication.
std::optional<std::uint64_t> HashLevelFile(const std::string &virtualPath)
{
    const auto resolved = Assisi::Core::AssetSystem::Resolve(virtualPath);
    if (!resolved)
        return std::nullopt;

    return Assisi::Core::HashTextFileNormalized(*resolved);
}

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

    // Before any session can exist: the quantization is inside the handshake
    // hash, so it has to be settled before the first hello is written.
    NetSync::LoadQuantizationFromConfig();
    NetSync::LoadSmoothingFromConfig();

    if (!_options.level.empty())
    {
        // LoadLevelSim, not LoadLevel: no asset cache, no scene renderer,
        // nothing GPU-owned to evict. Mesh and material GUIDs stay in the scene
        // as authored data for replication; the server never resolves them.
        if (!Assisi::App::LoadLevelSim(_scene, _options.level, _physics))
        {
            Log::Error("Server: failed to load level '{}'.", _options.level);
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

    NetSync::ReplicationConfig config;
    config.tickRateHz = static_cast<std::uint32_t>(GetConfig().physicsHz);
    _session          = std::make_unique<NetSync::NetSession>(_scene, &_physics, config);

    if (_options.role == ServerRole::Host)
    {
        // What joining clients are told to load. A host with no level file
        // advertises None, which a client treats as a clean abort — it has no
        // way to build the static half of the world.
        NetSync::LevelIdentity level;
        if (!_options.level.empty())
        {
            if (const std::optional<std::uint64_t> hash = HashLevelFile(_options.level))
            {
                level.addressing  = NetSync::LevelAddressing::Virtual;
                level.path        = _options.level;
                level.contentHash = *hash;
            }
        }

        if (!_session->Host(_options.port, std::move(level)))
        {
            RequestClose();
            return;
        }

        // A demo world that actually moves. Delta replication is only
        // interesting against change; a static level converges once and then
        // proves nothing for the rest of the run.
        for (std::uint32_t i = 0; i < _options.spawnCount; ++i)
        {
            const ECS::Entity entity = _scene.Create();
            ECS::Transform    transform;
            transform.position = {static_cast<float>(i) * 2.f, 0.f, 0.f};
            (void)_scene.Add<ECS::Transform>(entity, transform);
            (void)_scene.Add<NetSync::Replicated>(entity, NetSync::Replicated{});
            _moving.push_back(entity);
        }

        Log::Info("Server: listening on port {} ({} replicated entities).", _options.port, _moving.size());
    }
    // Deferred, exactly like the editor's join: this process has a level to load
    // before a NetId has anywhere to land.
    else if (!_session->Join(_options.address, _options.port, /*deferHandshake=*/true))
    {
        RequestClose();
    }
}

void ServerApp::BuildJoinedWorld()
{
    const NetSync::ServerHello *hello = _session->Handshake();
    if (hello == nullptr)
        return;

    const auto fail = [this](std::string reason)
    {
        Log::Error("Client: join failed — {}", reason);
        _session->AbortJoin(std::move(reason));
        RequestClose();
    };

    if (hello->level.addressing == NetSync::LevelAddressing::None)
    {
        fail("the host is not running a level file, so there is no world to build here.");
        return;
    }
    // The headless client only speaks virtual paths: an absolute one is a
    // play-in-editor temp snapshot, which belongs to the process that wrote it.
    if (hello->level.addressing != NetSync::LevelAddressing::Virtual)
    {
        fail("the host advertised a path this process cannot resolve.");
        return;
    }

    const std::optional<std::uint64_t> localHash = HashLevelFile(hello->level.path);
    if (!localHash)
    {
        fail("this build has no '" + hello->level.path + "'.");
        return;
    }
    if (*localHash != hello->level.contentHash)
    {
        Log::Error("Client: level content hash mismatch for '{}' — host {}, local {}.", hello->level.path,
                   Assisi::Core::ToHex64(hello->level.contentHash), Assisi::Core::ToHex64(*localHash));
        fail("your copy of '" + hello->level.path + "' differs from the host's; sync it and retry.");
        return;
    }

    if (!Assisi::App::LoadLevelSim(_scene, hello->level.path, _physics))
    {
        fail("'" + hello->level.path + "' failed to load.");
        return;
    }

    // The host owns these; they arrive as mirrors. The file's copies are the
    // host's authored originals, and keeping both would double the world.
    std::vector<ECS::Entity> doomed;
    _scene.ForEachEntity(
        [&](ECS::Entity entity)
        {
            if (_scene.Has<NetSync::Replicated>(entity))
                doomed.push_back(entity);
        });
    for (const ECS::Entity entity : doomed)
        _scene.Destroy(entity);
    _scene.FlushDestroyed();
    (void)Assisi::App::BuildSceneBodies(_scene, _physics);

    _session->ConfirmLevelReady();
    Log::Info("Client: built '{}' ({} replicated entities stripped) and answered the handshake.",
              hello->level.path, doomed.size());
}

void ServerApp::OnFixedUpdate(float dt)
{
    // Take input and acks before simulating, so a command that arrived for this
    // tick is applied on this tick rather than the next one.
    if (_session)
    {
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

    _physics.Update(dt);

    // Immediately after the step, before the snapshot below: a mirror woken by a
    // contact the server never had has to be put back before anything reads it.
    if (_session)
        _session->AfterPhysicsStep();

    // Move the demo world. Writes go through GetMut because that is what stamps
    // the change tick the delta is computed from — a write through a plain
    // Get would replicate nothing and report no error.
    const auto phase = static_cast<float>(GetSimTick()) * 0.02f;
    for (std::size_t i = 0; i < _moving.size(); ++i)
    {
        if (ECS::Transform *transform = _scene.GetMut<ECS::Transform>(_moving[i]))
            transform->position.y = std::sin(phase + static_cast<float>(i));
    }

    // After the simulation: a snapshot describes the world at the end of the
    // tick it is stamped with. A headless client has no devices to sample, so
    // it sends an empty command — enough to exercise the input path and give
    // the server something to measure its buffer depth against.
    if (_session)
        _session->Tick(GetSimTick());

    if (_options.tickLimit > 0 && GetSimTick() >= _options.tickLimit)
        RequestClose();
}

void ServerApp::OnUpdate(float dt)
{
    // Write the render pose for remote entities. A headless client renders
    // nothing, but running it here keeps this loop the same shape as a windowed
    // one — and it is the only place a bug in it would show up in a soak.
    //
    // Its *convergence* assertions must never read these Transforms, though:
    // for a bodied mirror this adds a decaying cosmetic offset on top of the
    // physics pose, and the physics pose is the one that is authoritative.
    if (_session)
        _session->SmoothView(dt);

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
    const double        elapsed  = now - _lastReportSeconds;
    const double        tickRate = elapsed > 0.0 ? static_cast<double>(tick - _lastReportTick) / elapsed : 0.0;
    _lastReportSeconds           = now;
    _lastReportTick              = tick;

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
}

void ServerApp::FlushDeferred()
{
    // End of frame: apply entities queued by Scene::Destroy() this tick. The
    // windowed path does this too — deferred destruction is a scene invariant,
    // not a rendering one.
    _scene.FlushDestroyed();
}

void ServerApp::OnShutdown()
{
    if (_session && _session->IsClient())
    {
        const NetSync::SessionStats stats = _session->Stats();
        Log::Info("Client: stopped after {} ticks — {} snapshots applied, {} rejected, {} entities mirrored.",
                  GetSimTick(), stats.snapshotsApplied, stats.snapshotsRejected, stats.replicatedEntities);
    }
    else
    {
        Log::Info("Server: stopped after {} ticks.", GetSimTick());
    }

    // Before the scene and physics world it holds a reference to.
    _session.reset();
}

} // namespace Sandbox
