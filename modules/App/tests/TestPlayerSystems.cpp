/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPlayerSystems.cpp
/// @brief What the engine does about players: which character is this
///        machine's player, its controls cleared after each step, and the
///        names of the player actions.
///
/// The controls arrive the same way whoever filled them in, so these tests write
/// PlayerInput directly. Turning the controls into movement and choosing a
/// camera is a game's, and the first-person template's tests cover those.

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/PlayerSystems.hpp>
#include <Assisi/App/ResourceStore.hpp>
#include <Assisi/App/StepWorld.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>

#    include <Assisi/NetSync/NetComponents.hpp>

using namespace Assisi::App;

namespace
{

constexpr float kStep = 1.f / 60.f;

/// The test's own game action, taken the way a game takes one.
inline constexpr PlayerAction kAim = GameAction(0);
inline constexpr PlayerActionName kTestActions[] = {
    {"Aim", kAim},
};
ASSISI_PLAYER_ACTIONS(kTestActions);

SystemContext Context(World &world, Assisi::Core::EventQueue &universal)
{
    return SystemContext{world, kStep, /*simTick=*/ 0, nullptr, nullptr, universal, true, nullptr};
}

/// A character with the components the systems read, and no body: nothing here
/// steps the physics.
Assisi::ECS::Entity SpawnCharacter(World &world, bool player)
{
    const Assisi::ECS::Entity entity = world.scene.Create();
    (void)world.scene.Add<Assisi::ECS::Transform>(entity);
    (void)world.scene.Add<Assisi::Physics::Character>(entity);
    (void)world.scene.Add<Assisi::Physics::CharacterIntent>(entity);
    if (player)
    {
        (void)world.scene.Add<Player>(entity);
    }
    return entity;
}

PlayerInput &Controls(World &world, Assisi::ECS::Entity entity)
{
    if (world.scene.Get<PlayerInput>(entity) == nullptr)
    {
        (void)world.scene.Add<PlayerInput>(entity);
    }
    return *world.scene.GetMut<PlayerInput>(entity);
}

} // namespace

TEST_CASE("A step's turning and presses are cleared once every system of the step has read them")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Players");
    Assisi::Core::EventQueue universal{Assisi::Core::EventScope::Universal};
    SystemContext ctx = Context(world, universal);

    // A player with no character to command, so nothing but the clearing
    // system uses its controls up.
    const Assisi::ECS::Entity entity = world.scene.Create();
    (void)world.scene.Add<Player>(entity);
    PlayerInput &input = Controls(world, entity);
    input.lookYaw = 0.5f;
    input.lookPitch = 0.25f;
    input.pressed = PlayerButtons::Of(PlayerAction::Jump);
    input.held = PlayerButtons::Of(PlayerAction::Sprint);

    float earlyYaw = 0.f;
    float lateYaw = 0.f;
    PlayerButtons latePressed;
    world.systems.Register(SystemPhase::FixedUpdate, "Early",
                           [&earlyYaw, entity](SystemContext &step)
                           { earlyYaw = step.world.scene.Get<PlayerInput>(entity)->lookYaw; });
    // The last system a game can place still reads them: the step clears them
    // after its systems, not as one of them.
    world.systems.Register(SystemPhase::PostFixedUpdate, "Late",
                           [&lateYaw, &latePressed, entity](SystemContext &step)
                           {
                               const PlayerInput &seen = *step.world.scene.Get<PlayerInput>(entity);
                               lateYaw = seen.lookYaw;
                               latePressed = seen.pressed;
                           })
    .Last();

    StepWorld(ctx);

    CHECK(earlyYaw == doctest::Approx(0.5f));
    CHECK(lateYaw == doctest::Approx(0.5f));
    CHECK(latePressed == PlayerButtons::Of(PlayerAction::Jump));
    const PlayerInput &after = *world.scene.Get<PlayerInput>(entity);
    CHECK(after.lookYaw == 0.f);
    CHECK(after.lookPitch == 0.f);
    CHECK(after.pressed == PlayerButtons{});
    // What is held is what the devices say now, which a step does not use up.
    CHECK(after.held == PlayerButtons::Of(PlayerAction::Sprint));
}

TEST_CASE("A replayed step clears the turning and presses it used, as the live step did")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Players");
    Assisi::Core::EventQueue universal{Assisi::Core::EventScope::Universal};
    SystemContext ctx = Context(world, universal);
    ctx.replaying = true;

    const Assisi::ECS::Entity entity = world.scene.Create();
    (void)world.scene.Add<Player>(entity);
    PlayerInput &input = Controls(world, entity);
    input.lookYaw = 0.5f;
    input.pressed = PlayerButtons::Of(PlayerAction::Jump);

    StepWorld(ctx);

    CHECK(world.scene.Get<PlayerInput>(entity)->lookYaw == 0.f);
    CHECK(world.scene.Get<PlayerInput>(entity)->pressed == PlayerButtons{});
}

TEST_CASE("Offline, every player and only players are this machine's")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Players");
    Assisi::Core::EventQueue universal{Assisi::Core::EventScope::Universal};
    SystemContext ctx = Context(world, universal);

    const Assisi::ECS::Entity player = SpawnCharacter(world, /*player=*/ true);
    const Assisi::ECS::Entity npc = SpawnCharacter(world, /*player=*/ false);

    LocalPlayerSystem(ctx);

    CHECK(world.scene.Get<LocalPlayer>(player) != nullptr);
    CHECK(world.scene.Get<PlayerInput>(player) != nullptr);
    CHECK(world.scene.Get<LocalPlayer>(npc) == nullptr);
    CHECK(world.scene.Get<PlayerInput>(npc) == nullptr);
}

TEST_CASE("In a session, the player this machine controls is its player")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Players");
    Assisi::Core::EventQueue universal{Assisi::Core::EventScope::Universal};
    SystemContext ctx = Context(world, universal);

    constexpr uint32_t kMine = 2;
    constexpr uint32_t kTheirs = 3;
    world.resources.Get<LocalClient>() = LocalClient{.id = kMine, .networked = true};

    const Assisi::ECS::Entity mine = SpawnCharacter(world, /*player=*/ true);
    const Assisi::ECS::Entity theirs = SpawnCharacter(world, /*player=*/ true);
    (void)world.scene.Add<Assisi::NetSync::ControlledBy>(mine, Assisi::NetSync::ControlledBy{kMine, true});
    (void)world.scene.Add<Assisi::NetSync::ControlledBy>(theirs, Assisi::NetSync::ControlledBy{kTheirs, true});

    LocalPlayerSystem(ctx);
    CHECK(world.scene.Get<LocalPlayer>(mine) != nullptr);
    CHECK(world.scene.Get<LocalPlayer>(theirs) == nullptr);

    // Control moves on: the old one stops being driven from here, and keeps no
    // controls that would walk it on by themselves.
    Controls(world, mine).move = {0.f, 1.f};
    world.scene.GetMut<Assisi::NetSync::ControlledBy>(mine)->client = kTheirs;
    LocalPlayerSystem(ctx);
    CHECK(world.scene.Get<LocalPlayer>(mine) == nullptr);
    CHECK(world.scene.Get<PlayerInput>(mine) == nullptr);
}

TEST_CASE("A character becoming this machine's player, and stopping, is announced once each")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Players");
    Assisi::Core::EventQueue universal{Assisi::Core::EventScope::Universal};
    SystemContext ctx = Context(world, universal);

    constexpr uint32_t kMine = 2;
    constexpr uint32_t kTheirs = 3;
    world.resources.Get<LocalClient>() = LocalClient{.id = kMine, .networked = true};
    const Assisi::ECS::Entity mine = SpawnCharacter(world, /*player=*/ true);
    (void)world.scene.Add<Assisi::NetSync::ControlledBy>(mine, Assisi::NetSync::ControlledBy{kMine, true});

    LocalPlayerSystem(ctx);
    REQUIRE(world.events.Read<LocalPlayerGained>().size() == 1);
    CHECK(world.events.Read<LocalPlayerGained>()[0].entity == mine);
    CHECK(world.events.Read<LocalPlayerLost>().empty());

    // Still this machine's: nothing more to say.
    world.events.Flush();
    LocalPlayerSystem(ctx);
    CHECK(world.events.Read<LocalPlayerGained>().empty());

    world.events.Flush();
    world.scene.GetMut<Assisi::NetSync::ControlledBy>(mine)->client = kTheirs;
    LocalPlayerSystem(ctx);
    REQUIRE(world.events.Read<LocalPlayerLost>().size() == 1);
    CHECK(world.events.Read<LocalPlayerLost>()[0].entity == mine);
    CHECK(world.events.Read<LocalPlayerGained>().empty());
}

TEST_CASE("A destroyed player is announced as lost once, when it has gone")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Players");
    Assisi::Core::EventQueue universal{Assisi::Core::EventScope::Universal};
    SystemContext ctx = Context(world, universal);

    const Assisi::ECS::Entity mine = SpawnCharacter(world, /*player=*/ true);
    LocalPlayerSystem(ctx);
    world.events.Flush();

    // Destroying is deferred: until the flush it is still the player.
    world.scene.Destroy(mine);
    LocalPlayerSystem(ctx);
    CHECK(world.events.Read<LocalPlayerLost>().empty());

    world.events.Flush();
    world.scene.FlushDestroyed();
    LocalPlayerSystem(ctx);
    REQUIRE(world.events.Read<LocalPlayerLost>().size() == 1);
    CHECK(world.events.Read<LocalPlayerLost>()[0].entity == mine);

    world.events.Flush();
    LocalPlayerSystem(ctx);
    CHECK(world.events.Read<LocalPlayerLost>().empty());
}

TEST_CASE("A player that was lost and then destroyed is announced once")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Players");
    Assisi::Core::EventQueue universal{Assisi::Core::EventScope::Universal};
    SystemContext ctx = Context(world, universal);

    constexpr uint32_t kMine = 2;
    constexpr uint32_t kTheirs = 3;
    world.resources.Get<LocalClient>() = LocalClient{.id = kMine, .networked = true};
    const Assisi::ECS::Entity mine = SpawnCharacter(world, /*player=*/ true);
    (void)world.scene.Add<Assisi::NetSync::ControlledBy>(mine, Assisi::NetSync::ControlledBy{kMine, true});
    LocalPlayerSystem(ctx);
    world.events.Flush();

    world.scene.GetMut<Assisi::NetSync::ControlledBy>(mine)->client = kTheirs;
    LocalPlayerSystem(ctx);
    CHECK(world.events.Read<LocalPlayerLost>().size() == 1);

    world.events.Flush();
    world.scene.Destroy(mine);
    world.scene.FlushDestroyed();
    LocalPlayerSystem(ctx);
    CHECK(world.events.Read<LocalPlayerLost>().empty());
}

TEST_CASE("A player's replacement is gained as the old one is lost")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Players");
    Assisi::Core::EventQueue universal{Assisi::Core::EventScope::Universal};
    SystemContext ctx = Context(world, universal);

    const Assisi::ECS::Entity first = SpawnCharacter(world, /*player=*/ true);
    LocalPlayerSystem(ctx);
    world.events.Flush();

    world.scene.Destroy(first);
    world.scene.FlushDestroyed();
    const Assisi::ECS::Entity second = SpawnCharacter(world, /*player=*/ true);
    LocalPlayerSystem(ctx);

    REQUIRE(world.events.Read<LocalPlayerLost>().size() == 1);
    CHECK(world.events.Read<LocalPlayerLost>()[0].entity == first);
    REQUIRE(world.events.Read<LocalPlayerGained>().size() == 1);
    CHECK(world.events.Read<LocalPlayerGained>()[0].entity == second);
}

TEST_CASE("A session that has not said who this machine is drives nothing")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Players");
    Assisi::Core::EventQueue universal{Assisi::Core::EventScope::Universal};
    SystemContext ctx = Context(world, universal);

    world.resources.Get<LocalClient>() = LocalClient{.id = 0, .networked = true};
    const Assisi::ECS::Entity player = SpawnCharacter(world, /*player=*/ true);

    LocalPlayerSystem(ctx);
    CHECK(world.scene.Get<LocalPlayer>(player) == nullptr);
}

TEST_CASE("The player actions are the engine's, then the game's, by slot")
{
    const std::vector<std::string> names = PlayerActionNames();
    REQUIRE(names.size() == static_cast<std::size_t>(PlayerAction::Count_));
    CHECK(names[static_cast<std::size_t>(PlayerAction::Jump)] == kActionJump);
    CHECK(names[static_cast<std::size_t>(PlayerAction::Crouch)] == kActionCrouch);
    CHECK(names[static_cast<std::size_t>(PlayerAction::Sprint)] == kActionSprint);
    CHECK(names[static_cast<std::size_t>(kAim)] == "Aim");
    CHECK(names[static_cast<std::size_t>(GameAction(1))].empty());
}

TEST_CASE("A world's resource is made once, when first asked for")
{
    ResourceStore store;
    CHECK(store.Find<LocalClient>() == nullptr);

    store.Get<LocalClient>().id = 7;
    REQUIRE(store.Find<LocalClient>() != nullptr);
    CHECK(store.Find<LocalClient>()->id == 7);
    CHECK(&store.Get<LocalClient>() == store.Find<LocalClient>());
}
