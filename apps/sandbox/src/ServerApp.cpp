/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "ServerApp.hpp"

#include <Assisi/App/LevelRuntime.hpp>
#include <Assisi/Core/Logger.hpp>

#include <chrono>
#include <utility>

namespace Sandbox
{
namespace
{

/// Seconds since the process's first call. Used only for the status line, so a
/// monotonic clock with an arbitrary epoch is exactly right.
double NowSeconds()
{
    using Clock                            = std::chrono::steady_clock;
    static const Clock::time_point started  = Clock::now();
    return std::chrono::duration<double>(Clock::now() - started).count();
}

/// How often the server prints a "still alive, here is the tick rate" line.
constexpr double kReportIntervalSeconds = 5.0;

} // namespace

ServerApp::ServerApp(std::string startupLevel, std::uint64_t tickLimit)
    : _startupLevel(std::move(startupLevel)), _tickLimit(tickLimit)
{
    // Set before Initialize(), which is what the headless split requires: by the
    // time Initialize() returns, the decision to skip window/renderer bring-up
    // has already been acted on.
    SetHeadless(true);
}

void ServerApp::OnStart()
{
    Assisi::Core::Log::Info("Server: headless, {} Hz fixed step{}.", GetConfig().physicsHz,
                            _tickLimit > 0 ? std::format(", stopping after {} ticks", _tickLimit) : std::string{});

    if (_startupLevel.empty())
    {
        Assisi::Core::Log::Info("Server: no level requested — simulating an empty world.");
        return;
    }

    // LoadLevelSim, not LoadLevel: no asset cache, no scene renderer, nothing
    // GPU-owned to evict. Mesh and material GUIDs stay in the scene as authored
    // data for replication; the server never resolves them.
    if (!Assisi::App::LoadLevelSim(_scene, _startupLevel, _physics))
    {
        Assisi::Core::Log::Error("Server: failed to load level '{}'.", _startupLevel);
        RequestClose();
        return;
    }

    Assisi::Core::Log::Info("Server: loaded '{}'.", _startupLevel);
}

void ServerApp::OnFixedUpdate(float dt)
{
    _physics.Update(dt);

    if (_tickLimit > 0 && GetSimTick() >= _tickLimit)
    {
        RequestClose();
    }
}

void ServerApp::OnUpdate(float /*dt*/)
{
    const double now = NowSeconds();
    if (now - _lastReportSeconds < kReportIntervalSeconds)
        return;

    // Measured, not configured: this line exists to show whether the loop is
    // actually holding its tick rate.
    const std::uint64_t tick     = GetSimTick();
    const double        elapsed  = now - _lastReportSeconds;
    const double        tickRate = elapsed > 0.0 ? static_cast<double>(tick - _lastReportTick) / elapsed : 0.0;

    Assisi::Core::Log::Info("Server: tick {} ({:.1f} Hz measured)", tick, tickRate);
    _lastReportSeconds = now;
    _lastReportTick    = tick;
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
    Assisi::Core::Log::Info("Server: stopped after {} ticks.", GetSimTick());
}

} // namespace Sandbox
