/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ServerApp.hpp
/// @brief The headless modes of the sandbox: `--server` hosts a world, and
/// `--connect` joins one.
///
/// Deliberately *not* the editor running with its window switched off. A
/// dedicated server is a different program with a different job — load a level,
/// step physics at a fixed rate, replicate the result — and pretending
/// otherwise would mean making every editor panel, gizmo, and selection overlay
/// tolerate a null renderer for no benefit.
///
/// The client mode here is headless too, which makes it a *test* client rather
/// than a playable one: it proves the protocol works between two processes and
/// logs what it received. The windowed client that renders what it receives is
/// the editor/game integration, which is separate work.

#include <Assisi/App/Application.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/NetSync/NetSession.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Sandbox
{

/// @brief What a headless process is doing.
enum class ServerRole : std::uint8_t
{
    /// Simulate only. No sockets — the Stage-2 smoke test of the headless split.
    Offline,
    /// Simulate and replicate to connected clients.
    Host,
    /// Connect to a host and apply what it sends.
    Client,
};

struct ServerOptions
{
    ServerRole    role = ServerRole::Offline;
    std::string   level;                  ///< Virtual path; empty for an empty world.
    std::string   address = "127.0.0.1";  ///< Client only.
    std::uint16_t port    = 27015;
    std::uint64_t tickLimit = 0;          ///< 0 = run until interrupted.
    /// Host only: spawn this many replicated, moving entities. A world that
    /// changes every tick is what actually exercises delta replication; a static
    /// level would converge once and prove very little.
    std::uint32_t spawnCount = 0;
};

class ServerApp final : public Assisi::App::Application
{
  public:
    explicit ServerApp(ServerOptions options);
    ~ServerApp() override;

  protected:
    void OnStart() override;
    void OnFixedUpdate(float dt) override;
    void OnUpdate(float dt) override;
    void OnShutdown() override;
    void FlushDeferred() override;

  private:
    void ReportStatus();

    ServerOptions _options;

    Assisi::ECS::Scene            _scene;
    Assisi::Physics::PhysicsWorld _physics;

    /// Constructed only in a networked role, so the offline mode never
    /// initializes GameNetworkingSockets at all.
    std::unique_ptr<Assisi::NetSync::NetSession> _session;

    /// Host only: the entities it moves each tick, so the demo world is
    /// actually in motion.
    std::vector<Assisi::ECS::Entity> _moving;

    double        _lastReportSeconds = 0.0;
    std::uint64_t _lastReportTick    = 0;
};

} // namespace Sandbox
