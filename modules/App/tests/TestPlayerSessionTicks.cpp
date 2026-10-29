/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPlayerSessionTicks.cpp
/// @brief A client steps at the host's ticks, and steps a tick again the same way it
///        stepped it the first time.

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "PlayerSessionTesting.hpp"

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/SystemCatalog.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Components.hpp>

using namespace Assisi;
using namespace Assisi::App::Testing;

namespace
{

/// Steps a host runs alone before anyone joins, so its count of steps and a
/// client's own are well apart.
constexpr std::int32_t kHostHeadStart = 100;

/// Pushed once per step by a replayed system in the event test.
struct Stepped
{
    std::uint64_t tick = 0;
};

/// Frames between the speed the speed test's client asks for changing, as a
/// sprint key pressed and let go would change it.
constexpr std::int32_t kFramesPerSpeedChange = 7;

/// A speed the speed test's client sprints at.
constexpr float kSprintSpeedScale = 2.f;

/// One frame of the speed test: @p own walks forward, sprinting in every
/// other run of kFramesPerSpeedChange frames, its speed set the way a game's
/// own system sets it.
void WalkAtChangingSpeed(ECS::Scene &scene, ECS::Entity own, std::int32_t frame)
{
    if (App::PlayerInput *controls = scene.GetMut<App::PlayerInput>(own))
    {
        controls->move = {0.f, 1.f};
    }
    if (Physics::CharacterIntent *intent = scene.GetMut<Physics::CharacterIntent>(own))
    {
        intent->speedScale = (frame / kFramesPerSpeedChange) % 2 == 0 ? 1.f : kSprintSpeedScale;
    }
}

} // namespace

TEST_CASE("A client's systems step at the host's tick, so a system of the tick alone agrees with the host's")
{
    const std::filesystem::path root = FreshRoot("assisi_players_ticks");
    WritePlayerBlueprint(root);

    Session session;
    Machine &host = session.Add();
    Machine &client = session.Add();

    // An oscillator each, never sent: each machine moves its own from the tick.
    std::map<std::uint64_t, float> hostHeights;
    std::map<std::uint64_t, float> clientHeights;
    const std::array<std::map<std::uint64_t, float> *, 2> heights{&hostHeights, &clientHeights};
    for (std::size_t i = 0; i < session.machines.size(); ++i)
    {
        Machine &machine = *session.machines[i];
        REQUIRE(App::SystemCatalog::Instance().Install(machine.world, std::vector<std::string>{"Oscillate"}, "test"));
        const ECS::Entity bob = machine.world.scene.Create();
        (void)machine.world.scene.Add<ECS::Transform>(bob);
        (void)machine.world.scene.Add<Runtime::Oscillator>(bob);
        std::map<std::uint64_t, float> &recorded = *heights[i];
        ECS::Scene &scene = machine.world.scene;
        machine.afterStep = [&recorded, &scene, bob](std::uint64_t tick)
                            { recorded[tick] = scene.Get<ECS::Transform>(bob)->position.y; };
    }

    for (std::int32_t i = 0; i < kHostHeadStart; ++i)
    {
        Step(host, ++host.tick);
    }
    JoinOnFloor(session, host, client);
    session.Run(kCatchUpFrames);

    // Ahead of the host, by the lead its commands need, rather than behind it
    // by the steps the host ran before it joined.
    const std::uint64_t clientTick = client.stepTicks.back();
    CHECK(clientTick > host.stepTicks.back());
    CHECK(client.tick < host.tick);

    session.Run(kCatchUpFrames);
    REQUIRE(hostHeights.contains(clientTick));
    CHECK(clientHeights.at(clientTick) == hostHeights.at(clientTick));
}

TEST_CASE("The sync test finds every tick a client steps a second time the same as the first")
{
    // A level with an oscillator each machine moves from the tick, and a
    // client walking and looking around: everything a replay must put back.
    const std::filesystem::path root = FreshRoot("assisi_players_ticks");
    std::string_view movement;
    SUBCASE("moving by SourceMovement")
    {
        movement = "SourceMovement";
    }
    SUBCASE("moving by ArcadeMovement")
    {
        movement = "ArcadeMovement";
    }
    WritePlayerBlueprint(root, movement);

    Session session;
    Machine &host = session.Add();
    Machine &client = session.Add();
    for (const std::unique_ptr<Machine> &machine : session.machines)
    {
        REQUIRE(App::SystemCatalog::Instance().Install(machine->world, std::vector<std::string>{"Oscillate"}, "test"));
        const ECS::Entity bob = machine->world.scene.Create();
        (void)machine->world.scene.Add<ECS::Transform>(bob);
        (void)machine->world.scene.Add<Runtime::Oscillator>(bob);
    }
    JoinOnFloor(session, host, client);

    const ECS::Entity own = LocalPlayerOf(client.world.scene);
    REQUIRE(own != ECS::NullEntity);
    NetSync::ReplicationClient &replication = *client.play.Session().Client();
    replication.SetSyncTest(true);

    constexpr float kTurnPerFrame = 0.01f;
    session.Run(kLongWalkFrames,
                [&]
    {
        if (App::PlayerInput *controls = client.world.scene.GetMut<App::PlayerInput>(own))
        {
            controls->move = {0.5f, 1.f};
            controls->lookYaw = kTurnPerFrame;
        }
    });

    CHECK(replication.SyncTestResults().replays > static_cast<std::uint64_t>(kLongWalkFrames) / 2u);
    CHECK(replication.SyncTestResults().differences == 0);
}

TEST_CASE("With the sync test on, a speed a game's system sets on a client's character is stepped again as it was set")
{
    // A game's systems run once, live, so a step stepped again must take the
    // speed scale that step had rather than the one set since.
    const std::filesystem::path root = FreshRoot("assisi_players_ticks");
    WritePlayerBlueprint(root, "ArcadeMovement");

    Session session;
    Machine &host = session.Add();
    Machine &client = session.Add();
    JoinOnFloor(session, host, client);

    const ECS::Entity own = LocalPlayerOf(client.world.scene);
    REQUIRE(own != ECS::NullEntity);
    NetSync::ReplicationClient &replication = *client.play.Session().Client();
    replication.SetSyncTest(true);

    std::int32_t frame = 0;
    session.Run(kLongWalkFrames, [&] { WalkAtChangingSpeed(client.world.scene, own, frame++); });

    CHECK(replication.SyncTestResults().replays > static_cast<std::uint64_t>(kLongWalkFrames) / 2u);
    CHECK(replication.SyncTestResults().differences == 0);
}

TEST_CASE("An event pushed in a tick a client steps again is delivered once, by the step that ran live")
{
    const std::filesystem::path root = FreshRoot("assisi_players_ticks");
    WritePlayerBlueprint(root);

    Session session;
    Machine &host = session.Add();
    Machine &client = session.Add();
    JoinOnFloor(session, host, client);

    // Replayed, so it runs in every step the sync test takes again.
    client.world.systems.Register(App::SystemPhase::FixedUpdate, "PushStepped",
                                  [](App::SystemContext &ctx) { ctx.events.Push(Stepped{.tick = ctx.simTick}); })
    .Replayed();
    NetSync::ReplicationClient &replication = *client.play.Session().Client();
    replication.SetSyncTest(true);
    const std::size_t stepsBefore = client.stepTicks.size();

    session.Run(kCatchUpFrames);

    REQUIRE(replication.SyncTestResults().replays > 0u);
    CHECK(client.world.events.Read<Stepped>().size() == client.stepTicks.size() - stepsBefore);
}
