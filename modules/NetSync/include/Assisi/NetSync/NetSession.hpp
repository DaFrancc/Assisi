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
#include <Assisi/NetSync/ReplicationClient.hpp>
#include <Assisi/NetSync/ReplicationConfig.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>

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

    // Correction stream (client). Totals, not rates: a rate needs a clock and a
    // window, and whoever is drawing this already has both.
    std::uint64_t correctionsApplied = 0;
    std::uint64_t correctionBytes    = 0;
    float         divergenceMean     = 0.f; ///< Metres, averaged over every correction so far.
    float         divergenceMax      = 0.f; ///< Metres, worst since the session began.
    std::uint64_t mirrorsResurrected = 0;

    // Host.
    std::uint32_t dirtyBacklog  = 0; ///< Entities that had something to send and did not fit.
    std::uint64_t keyframeSweeps = 0;

    // Relevancy (host). Worst case across clients, like the ping: a mean hides
    // the one connection actually having a bad time.
    std::uint32_t relevantEntities = 0; ///< Largest set any client currently holds.
    std::uint64_t relevancyEnters  = 0;
    std::uint64_t relevancyExits   = 0;

    // Messages. Split by *why* rather than lumped into one number, because a
    // rate-limited client is misbehaving, a stale one has a clock problem, an
    // out-of-range one is lying, and an unhandled one means somebody forgot to
    // write a handler — four different conversations.
    std::uint64_t intentsAccepted   = 0;
    std::uint64_t intentsRejected   = 0; ///< Wrong direction, out of range, or not theirs.
    std::uint64_t intentsRateLimited = 0;
    std::uint64_t intentsStale      = 0;
    std::uint64_t intentsUnhandled  = 0;
    std::uint64_t eventsSent        = 0; ///< Host: written into snapshot sections.
    std::uint64_t announcementsSent = 0;
    std::uint32_t eventsHeld        = 0; ///< Waiting for the entity they are about.
    std::uint64_t eventsEvicted     = 0;
    std::uint64_t eventsDispatched  = 0; ///< Client: handed to a handler.
    std::uint64_t eventsUnhandled   = 0;
};

class NetSession
{
  public:
    /// @param scene The scene this session replicates *from* (host) or *into*
    ///   (client). Captured by reference and must outlive the session — which
    ///   is why an application that swaps scenes should destroy the session
    ///   first rather than try to rebind it.
    /// @param physics That scene's physics world, on the same terms. Both sides
    ///   need it: the host reads authoritative body state from it, and the
    ///   client builds, corrects, and sleeps real bodies in it. Null keeps the
    ///   pre-body behaviour — motion as replicated Transforms, rendered by
    ///   interpolation.
    explicit NetSession(ECS::Scene &scene, Physics::PhysicsWorld *physics = nullptr,
                        ReplicationConfig config = {});
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

    /// @brief The set of level and blueprint files this build holds, hashed.
    ///
    /// A **host** cannot be reached until it has one — every ServerHello is
    /// withheld until then — and a **client** cannot answer one. Both are
    /// deliberate: the two hellos are each sent exactly once, so one sent with a
    /// placeholder is a join with no correct outcome. Computing the hash is a job
    /// (App::ContentSetHashJob), so this normally lands a frame or two after
    /// hosting or joining starts, and whichever side was waiting completes then.
    void SetContentSetHash(std::uint64_t hash);

    /// @brief Whether SetContentSetHash has been called on whichever half this
    /// session is. True for an offline session, which needs no hash at all.
    [[nodiscard]] bool HasContentSetHash() const;

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

    /// @brief Write this frame's rendered pose for every mirror: interpolation
    /// for the ones nothing simulates, decaying visual offsets for the ones the
    /// local physics owns.
    ///
    /// Call once per rendered frame, **after** the physics writeback and before
    /// transforms are propagated — an offset applied before the writeback is
    /// simply overwritten by it. A no-op for a host, which is already at server
    /// time by definition.
    void SmoothView(float dt);

    /// @brief Ask the host to re-anchor this client from the empty baseline.
    /// Client only; a no-op otherwise.
    void RequestKeyframe();

    /// @brief Re-assert the server's sleep verdict on mirrored bodies. Call once
    /// per fixed step, immediately after the local physics step and before
    /// Tick(). A no-op for a host and for a session with no physics world.
    void AfterPhysicsStep();

    /// @brief Take the input command a connected client sent for @p tick, or
    /// null. Host only; call once per client per tick from the simulation.
    const InputCommand *ConsumeInput(Net::ConnectionId client, std::uint64_t tick);

    /// @brief Who *this process* is on the network: `HostClientId` when
    /// hosting, the server-assigned id when joined, InvalidClientId offline or
    /// before the handshake lands.
    ///
    /// The value to compare `ControlledBy::client` against for "is this mine?",
    /// and it answers the same question in both roles — which is the point of
    /// the host having an id at all.
    [[nodiscard]] ClientId LocalClientId() const;

    /// @brief Give @p client control of @p entity. Host only; a no-op
    /// otherwise, because control is a fact the authority establishes.
    void SetControl(ECS::Entity entity, ClientId client, bool despawnOnDisconnect = true);

    /// @brief End whatever claim @p entity carries. Host only.
    void ClearControl(ECS::Entity entity);

    /// @brief Who controls @p entity, in either role — read from the component,
    /// which is authoritative on the host and mirrored on a client.
    [[nodiscard]] ClientId ControllerOf(ECS::Entity entity) const;

    /// @brief Whether @p entity is controlled by this process. True on the host
    /// for what it gave itself, on a client for what it was given.
    [[nodiscard]] bool ControlsEntity(ECS::Entity entity) const;

    /// @brief The session id of a connected client. Host only.
    [[nodiscard]] ClientId ClientIdOf(Net::ConnectionId client) const;

    /// @brief Ask the authority for something.
    ///
    /// The same call in both roles, which is the point: a client sends it over
    /// the wire, and a host — whose player is not a connection and never will be
    /// — submits it locally, where it enters the identical dispatch site with
    /// sender = HostClientId and passes the identical checks. One door means
    /// one, including for the person hosting.
    ///
    /// Sending an event fails to compile; direction is part of the type.
    ///
    /// @return false when offline, or before a client's handshake completes.
    template <typename T>
    bool SendIntent(const T &intent)
    {
        static_assert(Core::Reflect::MessageTraits<T>::direction == Core::Reflect::MessageDirection::Intent,
                      "SendIntent takes an AMSG(intent, ...). An event is the authority's word about what "
                      "happened — send those from the server.");
        if (_server)
        {
            _server->SubmitLocalIntent(intent);
            return true;
        }
        if (_client)
        {
            // Stamped with the *clock's* command tick, not the local sim tick,
            // for the same reason input is: the clock's whole job is to run far
            // enough ahead that this lands just before the server simulates it.
            return _client->SendIntent(intent, _clock ? _clock->CommandTick() : _simTick);
        }
        return false;
    }

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

    ECS::Scene            &_scene;
    Physics::PhysicsWorld *_physics = nullptr;
    ReplicationConfig      _config;

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
