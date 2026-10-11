/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ServerApp.hpp
/// @brief The headless modes: `--server` hosts a world, and
/// `--connect` joins one.
///
/// Deliberately *not* the editor running with its window switched off. A
/// dedicated server is a different program with a different job — load a level,
/// step physics at a fixed rate, replicate the result — and pretending
/// otherwise would mean making every editor panel, gizmo, and selection overlay
/// tolerate a null renderer for no benefit.
///
/// A different *program*, not different behaviour. Everything below the drawing
/// is shared code rather than a headless retelling of it: the world is an
/// App::World, the frame hooks are Application's, and joining goes through the
/// same App:: calls the windowed path uses.
///
/// The client mode here is headless too, which makes it a *test* client rather
/// than a playable one: it proves the protocol works between two processes and
/// logs what it received. The windowed client that renders what it receives is
/// the editor/game integration, which is separate work.

#include <Assisi/App/Application.hpp>
#include <Assisi/App/ContentSet.hpp>
#include <Assisi/App/World.hpp>
#if defined(ASSISI_NETWORKING)
#    include <Assisi/NetSync/NetSession.hpp>
#endif

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace Game
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
    ServerRole role = ServerRole::Offline;
    std::string level;                    ///< Virtual path; empty for an empty world.
    std::string address = "127.0.0.1";    ///< Client only.
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
    /// @param collision  Where the world's physics reads collision models from.
    ///                   Must outlive the server.
    ServerApp(ServerOptions options, const Assisi::Physics::CollisionSource &collision);
    ~ServerApp() override;

protected:
    void OnStart() override;
    void OnFixedUpdate(float dt) override;
    /// Real time, or a client's clock's rate of it, as a joined editor runs.
    [[nodiscard]] double SimulationSeconds(double frameSeconds) override;
    void OnUpdate(float dt) override;
    void OnShutdown() override;
    void FlushDeferred() override;
    void InstallQueuedSystems() override;

private:
    void ReportStatus();

    /// @brief The context the one-shot phases run under. No input, no clock —
    /// see Assisi::App::SystemPhase::Begin.
    [[nodiscard]] Assisi::App::SystemContext WorldStartContext();

    /// @brief The context a fixed step of @p dt at @p tick runs under.
    [[nodiscard]] Assisi::App::SystemContext StepContext(float dt, std::uint64_t tick);

    /// Installs the systems the level at @p level names, and those its
    /// blueprints need, into the world it was just loaded into. False, having
    /// logged why, when the file cannot be read or names one this build lacks.
    [[nodiscard]] bool InstallLevelSystems(std::string_view level);

    /// Host only: `spawnCount` replicated entities bobbing in place, so the
    /// world is in motion with no level at all. Delta replication is only
    /// interesting against change; a static world converges once and then
    /// proves nothing for the rest of the run.
    void SpawnDemoMovers();

    /// Client only: build the world the host's handshake names — resolve the
    /// level, check its content hash, load it, and strip the entities the host
    /// owns — then answer the handshake. The headless twin of the editor's
    /// join (EditorNet.cpp), and the reason a headless client is worth having:
    /// it exercises the whole hello-gated sequence with no window in the way.
    void BuildJoinedWorld();

    ServerOptions _options;

    /// Scene, physics, instance table and system registry, in the one aggregate
    /// the rest of App is written against.
    ///
    /// The whole World, not a headless subset: blueprint replication's provider
    /// and expander need the instance table, the scene and the pending system
    /// installs *together*, and a dedicated server is exactly the case where
    /// instances should arrive as instances rather than as loose entities.
    Assisi::App::World _world;

    /// The content-set scan, kicked when this process starts hosting or joining.
    /// Until it lands a host sends no ServerHello and a client sends no
    /// ClientHello — see NetSession::SetContentSetHash.
    Assisi::App::ContentSetHashJob _contentSetHash;

    /// Constructed only in a networked role, so offline mode never initializes
    /// GameNetworkingSockets — and without networking built there is no such
    /// role, so the member goes too. Offline headless simulation still works.
#if defined(ASSISI_NETWORKING)
    std::unique_ptr<Assisi::NetSync::NetSession> _session;
#endif

    double _lastReportSeconds = 0.0;
    std::uint64_t _lastReportTick    = 0;
};

} // namespace Game
