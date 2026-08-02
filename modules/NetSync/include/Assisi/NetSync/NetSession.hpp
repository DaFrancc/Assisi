/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file NetSession.hpp
/// @brief One object that owns a networked session: transport, role, and
/// whichever half of the replication protocol that role needs.
///
/// Everything below `NetSession` is protocol; everything above it is a game
/// loop. An application hosts or joins, calls three methods in the right places
/// in its loop, and reads a few numbers for its debug overlay.
///
/// ## The listen server is just a host in a windowed process
///
/// The design notes describe wiring a host's own client through
/// `CreateLoopbackPair` so the local player travels the same path as a remote
/// one. That is not what this does, and deliberately: the same notes also say
/// **one scene, not two** — the host's scene *is* the server scene, and it
/// renders it directly. Given one scene there is nothing for a local loopback
/// client to do except copy state onto itself and add a frame of interpolation
/// delay to the one player who does not need any. So `Host()` from a windowed
/// process *is* the listen server. Remote clients connect to it over UDP.
///
/// (`NetTransport::CreateLoopbackPair` still earns its keep: it is how the
/// replication tests run both halves in one process, and how a soak runs under
/// simulated lag.)

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Net/NetTransport.hpp>
#include <Assisi/NetSync/InputCommand.hpp>
#include <Assisi/NetSync/NetClock.hpp>
#include <Assisi/NetSync/Replication.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::NetSync
{

/// @brief What this process is doing on the network.
enum class SessionRole : std::uint8_t
{
    Offline, ///< Not networked. The default, and the only state that costs nothing.
    Host,    ///< Authoritative. Simulates, and replicates to whoever connects.
    Client,  ///< Applies what a host sends and sends its input back.
};

/// @brief A flat snapshot of session health, for a debug overlay.
struct SessionStats
{
    SessionRole role = SessionRole::Offline;

    // Both roles.
    std::int32_t pingMs           = 0;
    float        connectionQuality = 0.f; ///< 0..1; negative means "not measured yet".
    float        inBytesPerSec    = 0.f;
    float        outBytesPerSec   = 0.f;

    // Host.
    std::size_t   clientCount        = 0;
    std::uint64_t snapshotsSent      = 0;
    std::uint64_t bytesSent          = 0;
    std::uint64_t replicatedEntities = 0;

    // Client.
    bool          synchronized      = false;
    bool          worldComplete     = false;
    std::uint64_t snapshotsApplied  = 0;
    std::uint64_t snapshotsRejected = 0;
    std::uint64_t serverTick        = 0;
    std::uint32_t inputBufferDepth  = 0;
    std::uint32_t clockCorrections  = 0;
    std::uint32_t clockLead         = 0;
};

class NetSession
{
  public:
    /// @param scene The scene this session replicates *from* (host) or *into*
    ///   (client). Captured by reference and must outlive the session — which
    ///   is why an application that swaps scenes should destroy the session
    ///   first rather than try to rebind it.
    explicit NetSession(ECS::Scene &scene, ReplicationConfig config = {});
    ~NetSession();

    NetSession(const NetSession &)            = delete;
    NetSession &operator=(const NetSession &) = delete;

    /// @brief Start hosting on @p port. In a windowed process this is the
    /// listen server: the scene being rendered is the one being replicated.
    ///
    /// @p level is what joining clients are told to load. Leaving it at its
    /// default advertises `LevelAddressing::None`, which an editor client
    /// refuses — correct for a host with no level file, since there is no way
    /// for the client to build the static half of the world.
    /// @return false with LastError() set if the port could not be bound.
    bool Host(std::uint16_t port, LevelIdentity level = {});

    /// @brief Connect to a host. @p address is an IP literal (no DNS).
    ///
    /// @p deferHandshake holds the ClientHello until ConfirmLevelReady(), so a
    /// caller that must load the host's level first can do so before any
    /// snapshot is applied. A caller with nothing to build (the headless
    /// sandbox client, the tests) leaves it false and handshakes immediately.
    bool Join(std::string_view address, std::uint16_t port, bool deferHandshake = false);

    /// @brief True when a deferred join is waiting for the local world.
    [[nodiscard]] bool IsAwaitingLevel() const;

    /// @brief The handshake the host sent — notably which level to load. Null
    /// unless this is a client that has received one.
    [[nodiscard]] const ServerHello *Handshake() const;

    /// @brief Tell a deferred join that the local world is built.
    void ConfirmLevelReady();

    /// @brief Give up on a deferred join; @p reason lands in LastError() and in
    /// the client's reject message, so the UI can name the cause.
    void AbortJoin(std::string reason);

    /// @brief Tear the session down and return to Offline. A client also drops
    /// the entities it was mirroring — they belonged to the session, not to the
    /// scene.
    void Disconnect();

    /// @brief Pump the wire: connection events and incoming messages.
    /// Call at the *top* of the fixed step, so a command that arrived for this
    /// tick is applied on this tick rather than the next one.
    void Poll();

    /// @brief Send what this tick produced. Call at the *end* of the fixed
    /// step, after the simulation: a snapshot describes the world at the end of
    /// the tick it is stamped with.
    ///
    /// @param localInput A client passes the input it sampled this tick; it is
    ///   stamped with the clock's command tick and sent redundantly. A host
    ///   ignores this — its own input needs no round trip.
    void Tick(std::uint64_t simTick, const InputCommand *localInput = nullptr);

    /// @brief Smooth remote entities into the scene for rendering. Call once
    /// per frame, after Poll/Tick. A no-op for a host, which is already at
    /// server time by definition.
    void Interpolate();

    /// @brief Take the input command a connected client sent for @p tick, or
    /// null. Host only; call once per client per tick from the simulation.
    const InputCommand *ConsumeInput(Net::ConnectionId client, std::uint64_t tick);

    [[nodiscard]] SessionRole Role() const { return _role; }
    [[nodiscard]] bool        IsActive() const { return _role != SessionRole::Offline; }
    [[nodiscard]] bool        IsHost() const { return _role == SessionRole::Host; }
    [[nodiscard]] bool        IsClient() const { return _role == SessionRole::Client; }

    /// @brief The connected clients, host only.
    [[nodiscard]] const std::vector<Net::ConnectionId> &Clients() const { return _clients; }

    /// @brief One line describing what the session is doing, for a UI that
    /// should not have to assemble it from four booleans.
    [[nodiscard]] std::string StatusText() const;

    /// @brief Why the last Host()/Join() failed, or why a client was dropped.
    [[nodiscard]] const std::string &LastError() const { return _lastError; }

    [[nodiscard]] SessionStats Stats() const;

    /// @brief The replication server, host only — for a game that needs more
    /// than Stats() exposes. Null when not hosting.
    [[nodiscard]] ReplicationServer *Server() { return _server.get(); }
    /// @brief The replication client, client only. Null when not joined.
    [[nodiscard]] ReplicationClient *Client() { return _client.get(); }

  private:
    void EnsureTransport();

    ECS::Scene       &_scene;
    ReplicationConfig _config;

    /// Created on the first Host()/Join() and destroyed on Disconnect(), so an
    /// offline process never initializes the networking library at all.
    std::unique_ptr<Net::NetTransport>  _transport;
    std::unique_ptr<ReplicationServer>  _server;
    std::unique_ptr<ReplicationClient>  _client;
    std::unique_ptr<NetClock>           _clock;

    SessionRole                    _role       = SessionRole::Offline;
    Net::ConnectionId              _connection = Net::InvalidConnection;
    std::vector<Net::ConnectionId> _clients;
    std::vector<Net::NetEvent>     _events;
    InputCommandBuffer             _inputBuffer;
    std::string                    _lastError;
    std::uint64_t                  _simTick = 0;
};

} // namespace Assisi::NetSync
