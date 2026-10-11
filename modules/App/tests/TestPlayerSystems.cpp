/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPlayerSystems.cpp
/// @brief A player's controls turned into a character's movement, and which
///        character is this machine's player.
///
/// The controls arrive the same way whoever filled them in, so these tests write
/// PlayerInput directly. What is checked is the translation into the
/// character's own frame, that turning and presses are used once and not every
/// step after, that only players are driven, and that only this machine's
/// player looks through its camera.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/PlayerSystems.hpp>
#include <Assisi/App/ResourceStore.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Components.hpp>

#if defined(ASSISI_NETWORKING)
#    include <Assisi/NetSync/NetComponents.hpp>
#endif

using namespace Assisi::App;

namespace
{

constexpr float kStep = 1.f / 60.f;

/// Close enough for a direction or an angle that went through a quaternion.
constexpr double kTolerance = 1e-4;

/// The test's own game action, taken the way a game takes one.
inline constexpr PlayerAction kAim = GameAction(0);
inline constexpr PlayerActionName kTestActions[] = {
    {"Aim", kAim},
};
ASSISI_PLAYER_ACTIONS(kTestActions);

SystemContext Context(World &world, Assisi::Core::EventQueue &events)
{
    return SystemContext{world, kStep, /*simTick=*/ 0, nullptr, nullptr, events, true, nullptr};
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

/// A camera parented to @p character, active the way the player blueprint
/// authors it.
Assisi::ECS::Entity SpawnEye(World &world, Assisi::ECS::Entity character)
{
    const Assisi::ECS::Entity eye = world.scene.Create();
    (void)world.scene.Add<Assisi::ECS::Transform>(eye);
    Assisi::Runtime::Camera camera;
    camera.isActive = true;
    (void)world.scene.Add<Assisi::Runtime::Camera>(eye, camera);
    (void)world.scene.Add<Assisi::ECS::Parent>(eye, Assisi::ECS::Parent{character});
    return eye;
}

PlayerInput &Controls(World &world, Assisi::ECS::Entity entity)
{
    if (world.scene.Get<PlayerInput>(entity) == nullptr)
    {
        (void)world.scene.Add<PlayerInput>(entity);
    }
    return *world.scene.GetMut<PlayerInput>(entity);
}

/// Which way @p rotation faces, as the yaw in radians from -Z towards -X.
float YawOf(const glm::quat &rotation)
{
    const glm::vec3 forward = rotation * glm::vec3(0.f, 0.f, -1.f);
    return std::atan2(-forward.x, -forward.z);
}

} // namespace

TEST_CASE("A player's move is taken in the character's own frame")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Players");
    Assisi::Core::EventQueue events;
    SystemContext ctx = Context(world, events);

    const Assisi::ECS::Entity entity = SpawnCharacter(world, /*player=*/ true);
    // Turned a quarter left: forward is -X.
    world.scene.GetMut<Assisi::ECS::Transform>(entity)->rotation =
        glm::angleAxis(glm::radians(90.f), glm::vec3(0.f, 1.f, 0.f));
    Controls(world, entity).move = {0.f, 1.f};

    CharacterCommandSystem(ctx);

    const glm::vec3 move = world.scene.Get<Assisi::Physics::CharacterIntent>(entity)->move;
    CHECK(move.x == doctest::Approx(-1.f).epsilon(kTolerance));
    CHECK(move.y == doctest::Approx(0.f).epsilon(kTolerance));
    CHECK(move.z == doctest::Approx(0.f).epsilon(kTolerance));
}

TEST_CASE("A character with no controls is left to whatever else moves it")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Players");
    Assisi::Core::EventQueue events;
    SystemContext ctx = Context(world, events);

    const Assisi::ECS::Entity npc = SpawnCharacter(world, /*player=*/ false);
    world.scene.GetMut<Assisi::Physics::CharacterIntent>(npc)->move = {0.f, 0.f, 1.f};

    CharacterCommandSystem(ctx);

    // An AI's intent survives: only a character with controls is commanded.
    CHECK(world.scene.Get<Assisi::Physics::CharacterIntent>(npc)->move.z == doctest::Approx(1.f));
}

TEST_CASE("A jump press asks for one jump, however long it is held")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Players");
    Assisi::Core::EventQueue events;
    SystemContext ctx = Context(world, events);

    const Assisi::ECS::Entity entity = SpawnCharacter(world, /*player=*/ true);
    PlayerInput &input = Controls(world, entity);
    input.held = PlayerButtons::Of(PlayerAction::Jump);
    input.pressed = PlayerButtons::Of(PlayerAction::Jump);

    CharacterCommandSystem(ctx);
    CHECK(world.scene.Get<Assisi::Physics::CharacterIntent>(entity)->jump);
    CHECK(world.scene.Get<PlayerInput>(entity)->pressed == PlayerButtons{});

    // The physics step takes the request. Still held, nothing new was pressed.
    world.scene.GetMut<Assisi::Physics::CharacterIntent>(entity)->jump = false;
    CharacterCommandSystem(ctx);
    CHECK_FALSE(world.scene.Get<Assisi::Physics::CharacterIntent>(entity)->jump);
}

TEST_CASE("Holding crouch asks the character to crouch")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Players");
    Assisi::Core::EventQueue events;
    SystemContext ctx = Context(world, events);

    const Assisi::ECS::Entity entity = SpawnCharacter(world, /*player=*/ true);
    Controls(world, entity).held = PlayerButtons::Of(PlayerAction::Crouch);

    CharacterCommandSystem(ctx);
    CHECK(world.scene.Get<Assisi::Physics::CharacterIntent>(entity)->stance == Assisi::Physics::Stance::Crouching);

    world.scene.GetMut<PlayerInput>(entity)->held = PlayerButtons{};
    CharacterCommandSystem(ctx);
    CHECK(world.scene.Get<Assisi::Physics::CharacterIntent>(entity)->stance == Assisi::Physics::Stance::Standing);
}

TEST_CASE("Turning is applied once, on the body, and looking up on its camera")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Players");
    Assisi::Core::EventQueue events;
    SystemContext ctx = Context(world, events);

    const Assisi::ECS::Entity entity = SpawnCharacter(world, /*player=*/ true);
    const Assisi::ECS::Entity eye = SpawnEye(world, entity);
    const float turn = glm::radians(30.f);
    const float lookUp = glm::radians(20.f);
    PlayerInput &input = Controls(world, entity);
    input.lookYaw = turn;
    input.lookPitch = lookUp;

    CharacterCommandSystem(ctx);
    CHECK(YawOf(world.scene.Get<Assisi::ECS::Transform>(entity)->rotation) ==
          doctest::Approx(turn).epsilon(kTolerance));
    const glm::vec3 eyeForward = world.scene.Get<Assisi::ECS::Transform>(eye)->rotation * glm::vec3(0.f, 0.f, -1.f);
    CHECK(std::asin(eyeForward.y) == doctest::Approx(lookUp).epsilon(kTolerance));
    CHECK(world.scene.Get<PlayerInput>(entity)->lookYaw == 0.f);
    CHECK(world.scene.Get<PlayerInput>(entity)->lookPitch == 0.f);

    // A second step with nothing new turns nothing.
    CharacterCommandSystem(ctx);
    CHECK(YawOf(world.scene.Get<Assisi::ECS::Transform>(entity)->rotation) ==
          doctest::Approx(turn).epsilon(kTolerance));

    // Looking past straight up stops short of it.
    world.scene.GetMut<PlayerInput>(entity)->lookPitch = glm::radians(180.f);
    CharacterCommandSystem(ctx);
    const glm::vec3 clamped = world.scene.Get<Assisi::ECS::Transform>(eye)->rotation * glm::vec3(0.f, 0.f, -1.f);
    CHECK(glm::degrees(std::asin(clamped.y)) == doctest::Approx(kMaxPitchDegrees).epsilon(kTolerance));
}

TEST_CASE("Offline, every player and only players are this machine's")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Players");
    Assisi::Core::EventQueue events;
    SystemContext ctx = Context(world, events);

    const Assisi::ECS::Entity player = SpawnCharacter(world, /*player=*/ true);
    const Assisi::ECS::Entity npc = SpawnCharacter(world, /*player=*/ false);

    LocalPlayerSystem(ctx);

    CHECK(world.scene.Get<LocalPlayer>(player) != nullptr);
    CHECK(world.scene.Get<PlayerInput>(player) != nullptr);
    CHECK(world.scene.Get<LocalPlayer>(npc) == nullptr);
    CHECK(world.scene.Get<PlayerInput>(npc) == nullptr);
}

#if defined(ASSISI_NETWORKING)
TEST_CASE("In a session, the player this machine controls is its player")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Players");
    Assisi::Core::EventQueue events;
    SystemContext ctx = Context(world, events);

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

TEST_CASE("A session that has not said who this machine is drives nothing")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Players");
    Assisi::Core::EventQueue events;
    SystemContext ctx = Context(world, events);

    world.resources.Get<LocalClient>() = LocalClient{.id = 0, .networked = true};
    const Assisi::ECS::Entity player = SpawnCharacter(world, /*player=*/ true);

    LocalPlayerSystem(ctx);
    CHECK(world.scene.Get<LocalPlayer>(player) == nullptr);
}
#endif

TEST_CASE("Only this machine's player looks through its camera")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Players");
    Assisi::Core::EventQueue events;
    SystemContext ctx = Context(world, events);

    const Assisi::ECS::Entity mine = SpawnCharacter(world, /*player=*/ true);
    const Assisi::ECS::Entity theirs = SpawnCharacter(world, /*player=*/ true);
    const Assisi::ECS::Entity myEye = SpawnEye(world, mine);
    const Assisi::ECS::Entity theirEye = SpawnEye(world, theirs);
    (void)world.scene.Add<LocalPlayer>(mine);

    PlayerCameraSystem(ctx);

    CHECK(world.scene.Get<Assisi::Runtime::Camera>(myEye)->isActive);
    CHECK_FALSE(world.scene.Get<Assisi::Runtime::Camera>(theirEye)->isActive);
}

TEST_CASE("The player actions are the engine's, then the game's, by slot")
{
    const std::vector<std::string> names = PlayerActionNames();
    REQUIRE(names.size() == static_cast<std::size_t>(PlayerAction::Count_));
    CHECK(names[static_cast<std::size_t>(PlayerAction::Jump)] == kActionJump);
    CHECK(names[static_cast<std::size_t>(PlayerAction::Crouch)] == kActionCrouch);
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
