/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/PlayerSession.hpp>

#include <Assisi/App/BlueprintVerbs.hpp>
#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/NetSync/NetSession.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>
#include <Assisi/Runtime/Blueprint.hpp>

#include <span>
#include <vector>

namespace Assisi::App
{
namespace
{

/// Takes away a character its player left behind, with the rest of its
/// blueprint: a player's camera belongs to its character, and left behind it
/// would go on being looked through.
void DespawnWhole(World &world, ECS::Entity entity)
{
    const ECS::BlueprintMember *member = world.scene.Get<ECS::BlueprintMember>(entity);
    if (member != nullptr && DestroyInstance(world, member->instanceId))
    {
        return;
    }
    world.scene.Destroy(entity);
}

/// The first Player in @p world that no client controls yet, or NullEntity.
ECS::Entity UnclaimedPlayer(ECS::Scene &scene)
{
    for (auto [entity, player] : scene.Query<Player>())
    {
        (void)player;
        if (!scene.Has<NetSync::ControlledBy>(entity))
        {
            return entity;
        }
    }
    return ECS::NullEntity;
}

/// Makes a character at the start @p client is given, and returns its Player,
/// or NullEntity when the world has no start or the blueprint makes none.
ECS::Entity SpawnAtStart(World &world, NetSync::ClientId client)
{
    std::vector<ECS::Entity> starts;
    for (auto [entity, start] : world.scene.Query<PlayerStart>())
    {
        (void)start;
        starts.push_back(entity);
    }
    if (starts.empty())
    {
        return ECS::NullEntity;
    }

    // Spread over the starts in the order the players joined: ids are handed out
    // in that order and never reused.
    const ECS::Entity start = starts[client.value % starts.size()];
    const ECS::Transform *placement = world.scene.Get<ECS::Transform>(start);
    const std::optional<ECS::InstanceId> spawned = SpawnBlueprint(
        world, world.scene.Get<PlayerStart>(start)->blueprint.View(), placement != nullptr ? *placement : ECS::Transform{});
    if (!spawned.has_value())
    {
        return ECS::NullEntity;
    }
    for (const ECS::Entity member : Runtime::MembersOf(world.scene, *spawned))
    {
        if (world.scene.Has<Player>(member))
        {
            return member;
        }
    }
    return ECS::NullEntity;
}

/// Writes the command a remote player sent into its character's controls.
void ApplyCommand(PlayerInput &controls, const NetSync::InputCommand &command)
{
    const PlayerButtons buttons{command.actionBits};
    controls.move = {command.moveX, command.moveY};
    // A press is a button down now that was not down last step. A press shorter
    // than a step still arrives: the client sends a button held or pressed since
    // its last command as down.
    controls.pressed = PlayerButtons{static_cast<std::uint32_t>(controls.pressed.bits |
                                                                (buttons.bits & ~controls.held.bits))};
    controls.held = buttons;
    controls.lookYaw += command.lookYaw;
    controls.lookPitch += command.lookPitch;
}

} // namespace

void PublishPlayerActions() { NetSync::SetInputActionNames(PlayerActionNames()); }

void RelayHeldCommand(PlayerInput &controls, const NetSync::InputCommand *command, std::uint32_t starvedTicks)
{
    if (command != nullptr)
    {
        ApplyCommand(controls, *command);
    }
    else if (starvedTicks > kInputHoldTicks)
    {
        controls.move = {0.f, 0.f};
        controls.held = PlayerButtons{};
    }
}

void PlayerSession::Attach(NetSync::NetSession &session, World &world, bool hostPlays)
{
    _hostPlays = hostPlays;
    PublishPlayerActions();
    session.SetDespawner([&world](ECS::Entity entity) { DespawnWhole(world, entity); });
    _warned.clear();
    _starved.clear();
    _local.reset();
}

void PlayerSession::BeginStep(NetSync::NetSession &session, World &world, std::uint64_t simTick)
{
    world.resources.Get<LocalClient>() = LocalClient{.id = session.LocalClientId().value, .networked = true};
    _local.reset();

    if (session.IsHost())
    {
        GivePlayers(session, world);
        RelayControls(session, world, simTick);
    }
    else if (session.IsClient())
    {
        CaptureLocalControls(world);
    }
}

void PlayerSession::Detach(World &world)
{
    world.resources.Get<LocalClient>() = LocalClient{};
    _local.reset();
}

const NetSync::InputCommand *PlayerSession::LocalCommand() const { return _local.has_value() ? &*_local : nullptr; }

void PlayerSession::GivePlayers(NetSync::NetSession &session, World &world)
{
    NetSync::ReplicationServer &server = *session.Server();

    std::vector<NetSync::ClientId> players;
    if (_hostPlays)
    {
        players.push_back(NetSync::HostClientId);
    }
    for (const NetSync::ReadyClient &ready : session.ReadyClients())
    {
        players.push_back(ready.client);
    }

    for (const NetSync::ClientId client : players)
    {
        if (!server.ControlledEntities(client).empty())
        {
            continue;
        }

        ECS::Entity character = UnclaimedPlayer(world.scene);
        if (character == ECS::NullEntity)
        {
            character = SpawnAtStart(world, client);
        }
        if (character == ECS::NullEntity)
        {
            if (_warned.insert(client.value).second)
            {
                Core::Log::Warn("Players: no character for client {}: the level has no unclaimed Player and no "
                                "PlayerStart to make one at.",
                                client.value);
            }
            continue;
        }
        server.SetControl(character, client);
    }
}

void PlayerSession::RelayControls(NetSync::NetSession &session, World &world, std::uint64_t simTick)
{
    NetSync::ReplicationServer &server = *session.Server();
    for (const NetSync::ReadyClient &ready : session.ReadyClients())
    {
        const NetSync::InputCommand *command = session.ConsumeInput(ready.connection, simTick);
        std::uint32_t &starved = _starved[ready.client.value];
        starved = command != nullptr ? 0u : starved + 1u;

        for (const ECS::Entity entity : server.ControlledEntities(ready.client))
        {
            if (!world.scene.Has<Player>(entity))
            {
                continue;
            }
            if (!world.scene.Has<PlayerInput>(entity))
            {
                (void)world.scene.Add<PlayerInput>(entity);
            }
            RelayHeldCommand(*world.scene.GetMut<PlayerInput>(entity), command, starved);
        }
    }
}

void PlayerSession::CaptureLocalControls(World &world)
{
    // begin()/end() rather than a range-for that returns on its first pass: MSVC
    // reads the loop's increment as unreachable and /WX turns C4702 into an error
    // in the debug preset. Same reason as EditorApp::BlueprintSunEntity.
    auto locals = world.scene.Query<Mut<PlayerInput>, LocalPlayer>();
    auto first = locals.begin();
    if (first == locals.end())
    {
        return;
    }
    auto [entity, controls, local] = *first;
    (void)entity;
    (void)local;
    NetSync::InputCommand command;
    command.actionBits = controls.held.bits | controls.pressed.bits;
    command.moveX = controls.move.x;
    command.moveY = controls.move.y;
    command.lookYaw = controls.lookYaw;
    command.lookPitch = controls.lookPitch;

    // Turned here by exactly what the host will turn it by, which is what
    // it accepts: a flick past the host's limit would otherwise leave the
    // two facing different ways for good.
    (void)NetSync::ClampInputCommand(command, NetSync::InputLimits{});
    controls.move = {command.moveX, command.moveY};
    controls.lookYaw = command.lookYaw;
    controls.lookPitch = command.lookPitch;

    _local = command;
}

} // namespace Assisi::App
