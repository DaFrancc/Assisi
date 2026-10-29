/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/NetworkedPlay.hpp>

#include <Assisi/App/BlueprintReplication.hpp>
#include <Assisi/App/ContentSet.hpp>
#include <Assisi/App/StepWorld.hpp>
#include <Assisi/Core/EventQueue.hpp>

#include <optional>
#include <utility>

namespace Assisi::App
{

NetworkedPlay::NetworkedPlay(World &world, NetSync::ReplicationConfig config)
    : _world(world), _session(world.scene, &world.physics, std::move(config))
{
    world.net.Attach(_session);
}

NetworkedPlay::~NetworkedPlay()
{
    _world.net.Detach();
    // A client's mirrors belong to the session, not the scene; flushed so they
    // are gone before anything walks the scene again.
    _session.Disconnect();
    _world.scene.FlushDestroyed();
    _players.Detach(_world);
}

bool NetworkedPlay::Host(std::uint16_t port, NetSync::LevelIdentity level, HostRole role)
{
    _players.Attach(_session, _world, role == HostRole::Player);
    if (!_session.Host(port, std::move(level)))
    {
        return false;
    }
    // Here, before any step: until a host holds the content set it cannot name a
    // blueprint on the wire, and one spawned in that window goes out as loose
    // entities for good. Scanned on every Host or Join, because the content on
    // disk may have moved on since the last session.
    ApplyContentSet(_session, _world, BuildContentSet());
    return true;
}

bool NetworkedPlay::Join(std::string_view address, std::uint16_t port)
{
    _players.Attach(_session, _world, /*hostPlays=*/ false);
    // Deferred: the ClientHello waits until the host's level is built, because a
    // snapshot applied against a world that does not exist yet maps NetIds onto
    // whatever is in those slots.
    if (!_session.Join(address, port, /*deferHandshake=*/ true))
    {
        return false;
    }
    ApplyContentSet(_session, _world, BuildContentSet());
    return true;
}

std::uint64_t NetworkedPlay::BeginStep(std::uint64_t localTick, bool stepping)
{
    // First, so a command that arrived for this tick is applied on this tick.
    _session.Poll();
    const std::uint64_t tick = _session.BeginStep(localTick);

    // Only once the session can say who this machine is: before then a client
    // has no player, and a world told it is networked drives nothing.
    if (stepping && _session.IsActive())
    {
        _players.BeginStep(_session, _world, tick);
    }
    _tick = tick;
    return tick;
}

void NetworkedPlay::StepWorld(const SystemContext &ctx)
{
    if (&ctx.world == &_world)
    {
        ReplayMispredicted(ctx);
    }
    App::StepWorld(ctx);
}

void NetworkedPlay::ReplayMispredicted(const SystemContext &live)
{
    NetSync::ReplicationClient *client = _session.Client();
    if (client == nullptr)
    {
        return;
    }
    std::optional<NetSync::ReplicationClient::Replay> replay = client->BeginRollback();
    const bool syncTest = !replay.has_value();
    if (syncTest)
    {
        replay = client->BeginSyncTest();
    }
    if (!replay.has_value())
    {
        return;
    }

    // What the replayed ticks push was delivered when they ran live.
    const Core::EventRoute::DropScope dropping(live.events);
    _players.BeginReplay(_world, replay->from);
    SystemContext replayed = live;
    replayed.replaying = true;
    for (const std::uint64_t tick : replay->ticks)
    {
        const NetSync::ReplicationClient::AppliedCommand applied = client->CommandAppliedAt(tick);
        client->RestoreIntentsAt(tick);
        _players.ReplayCommand(_world, applied.command, applied.starvedTicks);
        replayed.simTick = tick;
        App::StepWorld(replayed, replay->characters, client->FollowersAt(tick), replay->bodies);
        client->RecordHistory(tick);
        _players.RecordControls(_world, tick);
    }
    client->EndRollback();
    _players.EndReplay(_world);
    if (syncTest)
    {
        client->EndSyncTest();
    }
}

void NetworkedPlay::EndStep()
{
    // Between the step and the send, and it has to stay there: a mirrored body
    // woken by a contact the host never had must be put back before the send
    // reads it.
    _session.AfterPhysicsStep();
    if (_session.IsClient())
    {
        _players.RecordControls(_world, _tick);
    }
    _session.Tick(_players.LocalCommand());
}

void NetworkedPlay::ConfirmLevelReady() { _session.ConfirmLevelReady(); }

} // namespace Assisi::App
