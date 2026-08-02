/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/NetSession.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/NetSync/NetComponents.hpp>

#include <format>
#include <utility>

namespace Assisi::NetSync
{

NetSession::NetSession(ECS::Scene &scene, Physics::PhysicsWorld *physics, ReplicationConfig config)
    : _scene(scene), _physics(physics), _config(std::move(config))
{
    // The game's never-replicate list, read here rather than inside the server:
    // the server takes its whole configuration as a value, so a test can set the
    // list without a filesystem, and the layer that owns the session is the one
    // that knows where the game config lives.
    //
    // Only when the caller has not already supplied one — an explicitly
    // configured session (every test, and any embedder with its own policy
    // source) must not have game.json silently override it.
    if (_config.neverReplicate.empty())
        _config.neverReplicate = LoadNeverReplicateFromConfig();
}

NetSession::~NetSession() { Disconnect(); }

void NetSession::EnsureTransport()
{
    if (!_transport)
        _transport = std::make_unique<Net::NetTransport>();
}

bool NetSession::Host(std::uint16_t port, LevelIdentity level)
{
    Disconnect();
    EnsureTransport();

    if (!_transport->Listen(port))
    {
        _lastError = std::format("could not listen on port {}: {}", port, _transport->LastError());
        Core::Log::Error("NetSession: {}", _lastError);
        _transport.reset();
        return false;
    }

    _server = std::make_unique<ReplicationServer>(*_transport, _scene, _physics, _config);
    // Before any connection can arrive, since it is carried in every hello.
    _server->SetLevelIdentity(std::move(level));
    _role = SessionRole::Host;
    _lastError.clear();
    Core::Log::Info("NetSession: hosting on port {} (snapshots at {} Hz, level '{}').", port,
                    _server->Config().snapshotHz,
                    _server->Level().path.empty() ? "<none>" : _server->Level().path);
    return true;
}

bool NetSession::Join(std::string_view address, std::uint16_t port, bool deferHandshake)
{
    Disconnect();
    EnsureTransport();

    _connection = _transport->Connect(address, port);
    if (_connection == Net::InvalidConnection)
    {
        _lastError = std::format("could not connect to {}:{}: {}", address, port, _transport->LastError());
        Core::Log::Error("NetSession: {}", _lastError);
        _transport.reset();
        return false;
    }

    _client = std::make_unique<ReplicationClient>(*_transport, _scene, _connection, _physics);
    // Before the first Poll: a hello that arrives and is answered immediately
    // cannot be un-answered.
    _client->SetDeferHandshake(deferHandshake);
    _clock = std::make_unique<NetClock>(_config.tickRateHz);
    _role  = SessionRole::Client;
    _lastError.clear();
    Core::Log::Info("NetSession: connecting to {}:{}...", address, port);
    return true;
}

bool NetSession::IsAwaitingLevel() const { return _client && _client->IsAwaitingLevel(); }

const ServerHello *NetSession::Handshake() const { return _client ? &_client->Handshake() : nullptr; }

void NetSession::ConfirmLevelReady()
{
    if (_client)
        _client->ConfirmLevelReady();
}

void NetSession::AbortJoin(std::string reason)
{
    _lastError = reason;
    if (_client)
        _client->AbortJoin(std::move(reason));
}

void NetSession::Disconnect()
{
    if (_client)
    {
        // A client's mirrored entities belong to the session, not the scene:
        // leaving them behind would strand a frozen copy of someone else's
        // world in a scene the player is still looking at.
        _client->Reset();
        _scene.FlushDestroyed();
    }

    // Order matters — both halves hold a reference to the transport.
    _client.reset();
    _server.reset();
    _clock.reset();
    _transport.reset();

    _clients.clear();
    _connection = Net::InvalidConnection;
    _role       = SessionRole::Offline;
    _inputBuffer.Clear();
}

void NetSession::Poll()
{
    if (!_transport)
        return;

    _transport->Poll(_events);
    for (const Net::NetEvent &event : _events)
    {
        switch (event.type)
        {
        case Net::NetEvent::Type::Connected:
            if (_server)
            {
                _clients.push_back(event.connection);
                _server->AddConnection(event.connection);
                Core::Log::Info("NetSession: client {} connected ({} total).", event.connection, _clients.size());
            }
            break;

        case Net::NetEvent::Type::Disconnected:
            if (_server)
            {
                _server->RemoveConnection(event.connection);
                std::erase(_clients, event.connection);
                Core::Log::Info("NetSession: client {} disconnected: {} ({} left).", event.connection,
                                event.closeDebug, _clients.size());
            }
            else if (_client && event.connection == _connection)
            {
                _lastError = event.closeDebug.empty() ? "connection closed by the host" : event.closeDebug;
                Core::Log::Warn("NetSession: lost the host connection: {}", _lastError);
                // Tear down rather than sit in a half-dead state. v1's reconnect
                // is a full rejoin, so there is nothing here worth preserving.
                Disconnect();
                return;
            }
            break;

        case Net::NetEvent::Type::Message:
            if (_server)
                _server->HandleMessage(event.connection, event.payload);
            else if (_client)
                _client->HandleMessage(event.payload);
            break;
        }
    }
}

void NetSession::Tick(std::uint64_t simTick, const InputCommand *localInput)
{
    _simTick = simTick;

    if (_server)
    {
        _server->Tick(simTick);
        return;
    }

    if (!_client || !_client->IsSynchronized())
        return;

    _clock->Tick();

    Net::ConnectionStats transportStats;
    const std::int32_t   pingMs =
        _transport->GetConnectionStats(_connection, transportStats) ? transportStats.pingMs : 0;
    _clock->OnSnapshot(_client->Feedback(), pingMs);

    // Input is stamped with the *clock's* tick, not the local sim tick: the
    // whole job of the clock is to run far enough ahead that this command lands
    // just before the server simulates that tick.
    InputCommand command = localInput != nullptr ? *localInput : InputCommand{};
    command.tick         = _clock->CommandTick();
    _inputBuffer.Push(command);
    _client->SendInput(_inputBuffer);
}

void NetSession::AfterPhysicsStep()
{
    if (_client)
        _client->EnforceSleep();
}

void NetSession::SmoothView(float dt)
{
    // A host is at server time by definition — it *is* the server — so there is
    // nothing to smooth and nothing to delay.
    if (!_client || !_client->IsSynchronized() || !_clock)
        return;

    _client->SmoothView(_client->RenderTimeFor(static_cast<double>(_clock->EstimatedServerTick())), dt);
}

void NetSession::RequestKeyframe()
{
    if (_client)
        _client->RequestKeyframe();
}

const InputCommand *NetSession::ConsumeInput(Net::ConnectionId client, std::uint64_t tick)
{
    return _server ? _server->ConsumeInput(client, tick) : nullptr;
}

std::string NetSession::StatusText() const
{
    switch (_role)
    {
    case SessionRole::Offline:
        return _lastError.empty() ? "Offline" : std::format("Offline — {}", _lastError);

    case SessionRole::Host:
        return std::format("Hosting — {} client{}", _clients.size(), _clients.size() == 1 ? "" : "s");

    case SessionRole::Client:
        if (!_client->RejectMessage().empty())
            return std::format("Rejected — {}", _client->RejectMessage());
        if (_client->IsAwaitingLevel())
            return std::format("Loading the host's level ({})...", _client->Handshake().level.path);
        if (!_client->IsSynchronized())
            return "Connecting...";
        if (!_client->IsWorldComplete())
            return std::format("Joining — {} entities so far", _client->ReplicatedEntityCount());
        return std::format("Connected — {} entities", _client->ReplicatedEntityCount());
    }
    return "Offline";
}

SessionStats NetSession::Stats() const
{
    SessionStats stats;
    stats.role = _role;

    if (_server)
    {
        stats.clientCount = _clients.size();
        for (const Net::ConnectionId client : _clients)
        {
            if (const ConnectionDiagnostics *diagnostics = _server->Diagnostics(client))
            {
                stats.snapshotsSent += diagnostics->snapshotsSent;
                stats.bytesSent += diagnostics->bytesSent;
                // Worst case across clients, like the ping below: a mean hides
                // the one connection that is actually struggling.
                stats.dirtyBacklog  = std::max(stats.dirtyBacklog, diagnostics->dirtyBacklog);
                stats.keyframeSweeps = std::max(stats.keyframeSweeps, diagnostics->keyframeSweeps);
            }
            Net::ConnectionStats transportStats;
            if (_transport->GetConnectionStats(client, transportStats))
            {
                // Worst case across clients, not an average: a mean hides the
                // one player having a bad time, which is the only one worth
                // knowing about.
                stats.pingMs = std::max(stats.pingMs, transportStats.pingMs);
                stats.outBytesPerSec += transportStats.outBytesPerSec;
                stats.inBytesPerSec += transportStats.inBytesPerSec;
            }
        }

        for (auto [entity, replicated] : const_cast<ECS::Scene &>(_scene).Query<Replicated>())
        {
            (void)entity;
            (void)replicated;
            ++stats.replicatedEntities;
        }
    }
    else if (_client)
    {
        stats.synchronized      = _client->IsSynchronized();
        stats.worldComplete     = _client->IsWorldComplete();
        stats.snapshotsApplied  = _client->SnapshotsApplied();
        stats.snapshotsRejected = _client->SnapshotsRejected();
        stats.serverTick        = _client->LastAppliedTick();
        stats.inputBufferDepth  = _client->Feedback().inputBufferDepth;
        stats.replicatedEntities = _client->ReplicatedEntityCount();

        const ReplicationClient::CorrectionStats &corrections = _client->Corrections();
        stats.correctionsApplied = corrections.applied;
        stats.correctionBytes    = corrections.bytesApplied;
        stats.divergenceMean     = corrections.divergenceMean();
        stats.divergenceMax      = corrections.divergenceMax;
        stats.mirrorsResurrected = _client->MirrorsResurrected();
        if (_clock)
        {
            stats.clockCorrections = _clock->CorrectionCount();
            stats.clockLead        = _clock->Lead();
        }

        Net::ConnectionStats transportStats;
        if (_transport->GetConnectionStats(_connection, transportStats))
        {
            stats.pingMs           = transportStats.pingMs;
            stats.connectionQuality = transportStats.connectionQualityLocal;
            stats.inBytesPerSec    = transportStats.inBytesPerSec;
            stats.outBytesPerSec   = transportStats.outBytesPerSec;
        }
    }

    return stats;
}

} // namespace Assisi::NetSync
