/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "ServerApp.hpp"

#include <Assisi/App/LevelRuntime.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/NetComponents.hpp>

#include <chrono>
#include <cmath>
#include <format>
#include <utility>

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
    _session          = std::make_unique<NetSync::NetSession>(_scene, config);

    if (_options.role == ServerRole::Host)
    {
        if (!_session->Host(_options.port))
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
    else if (!_session->Join(_options.address, _options.port))
    {
        RequestClose();
    }
}

void ServerApp::OnFixedUpdate(float dt)
{
    // Take input and acks before simulating, so a command that arrived for this
    // tick is applied on this tick rather than the next one.
    if (_session)
        _session->Poll();

    _physics.Update(dt);

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

void ServerApp::OnUpdate(float /*dt*/)
{
    // Smooth remote entities for rendering. A headless client renders nothing,
    // but running it here keeps this loop the same shape as a windowed one —
    // and it is the only place a bug in it would show up in a soak.
    if (_session)
        _session->Interpolate();

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
