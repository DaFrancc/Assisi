/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPlayerSessionBodies.cpp
/// @brief A crate a client's player walks up to and pushes, in real sessions:
///        the client simulates it and the host takes its reports, its player
///        is not corrected while it pushes, a step stepped again meets the
///        crate where it was, and a second player who takes over the pushing
///        takes the crate over once.

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

#include "PlayerSessionTesting.hpp"

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>

using namespace Assisi;
using namespace Assisi::App::Testing;

namespace
{

/// A light crate's half size (m) and mass (kg): one a player shoves along at a
/// walk.
constexpr float kCrateHalf = 0.4f;
constexpr float kCrateMass = 10.f;

/// How far in front of a player a crate is put (m), along the way a player
/// faces before it turns: further than the host hands a crate out from.
constexpr float kCrateAhead = 3.f;

/// Frames a player walks into its crate: up to it and a few metres on.
constexpr std::int32_t kPushFrames = 180;

/// Further than a push of kPushFrames moves the crate (m).
constexpr float kPushedFar = 2.f;

/// A quarter turn to the right (rad): from facing -z to facing +x.
constexpr float kQuarterRight = -1.5707963f;

/// Where the starts of the handover case are, in the order the players are
/// spread over them: the second client, level with the first client's crate
/// and off to one side of it; the host, out of the way; the first client.
constexpr glm::vec3 kSecondClientStart{-4.f, 0.f, -kCrateAhead};
constexpr glm::vec3 kHostStart{20.f, 0.f, 0.f};
constexpr glm::vec3 kFirstClientStart{0.f, 0.f, 0.f};

/// A crate on the host, @p ahead of where a player at @p from faces.
ECS::Entity AddCrate(Machine &host, glm::vec3 from)
{
    ECS::Scene &scene = host.world.scene;
    const ECS::Entity crate = scene.Create();
    (void)scene.Add<ECS::Transform>(crate,
                                    ECS::Transform{.position = from + glm::vec3{0.f, kCrateHalf, -kCrateAhead}});
    Physics::Collider collider;
    collider.shape = Physics::ColliderShape::Box;
    collider.halfExtents = glm::vec3{kCrateHalf};
    (void)scene.Add<Physics::Collider>(crate, collider);
    Physics::RigidBody body;
    body.mass = kCrateMass;
    (void)scene.Add<Physics::RigidBody>(crate, body);
    (void)scene.Add<NetSync::Replicated>(crate, NetSync::Replicated{});
    return crate;
}

/// Walks @p walker forward on @p scene, or stands it still.
void Walk(ECS::Scene &scene, ECS::Entity walker, bool walking = true)
{
    if (App::PlayerInput *controls = scene.GetMut<App::PlayerInput>(walker))
    {
        controls->move = {0.f, walking ? 1.f : 0.f};
    }
}

/// Walks @p walker forward, turned by @p yaw on the first call.
void WalkAt(ECS::Scene &scene, ECS::Entity walker, float yaw, bool &turned)
{
    App::PlayerInput *controls = scene.GetMut<App::PlayerInput>(walker);
    if (controls == nullptr)
    {
        return;
    }
    controls->move = {0.f, 1.f};
    if (!turned)
    {
        controls->lookYaw = yaw;
        turned = true;
    }
}

/// Every client a crate is simulated by, in order, each once in a row.
struct Simulators
{
    std::vector<std::uint32_t> seen;

    void Watch(NetSync::ReplicationServer &server, ECS::Entity crate)
    {
        const std::uint32_t now = server.SimulatorOf(crate).value;
        if (now != 0 && (seen.empty() || seen.back() != now))
        {
            seen.push_back(now);
        }
    }
};

/// A host and its client on a floor, the client's player with a crate ahead
/// of it, and the client holding the crate's mirror.
struct Pushing
{
    Session session;
    Machine *host = nullptr;
    Machine *client = nullptr;
    ECS::Entity crate{ECS::NullEntity};
    ECS::Entity crateMirror{ECS::NullEntity};
    ECS::Entity player{ECS::NullEntity};

    Pushing()
    {
        const std::filesystem::path root = FreshRoot("assisi_players_bodies");
        WritePlayerBlueprint(root);
        host = &session.Add();
        client = &session.Add();
        JoinOnFloor(session, *host, *client);
        player = LocalPlayerOf(client->world.scene);
        REQUIRE(player != ECS::NullEntity);
        crate = AddCrate(*host, client->world.scene.Get<ECS::Transform>(player)->position);
        session.Run(kCatchUpFrames);
        crateMirror = client->play.Session().Client()->EntityOf(host->play.Session().Server()->NetIdOf(crate));
        REQUIRE(crateMirror != ECS::NullEntity);
    }

    [[nodiscard]] NetSync::ClientId ClientId() const { return client->play.Session().LocalClientId(); }
};

} // namespace

TEST_CASE("A client's player pushing a crate simulates it, and the host takes its reports without correcting it")
{
    Pushing rig;
    NetSync::ReplicationServer &server = *rig.host->play.Session().Server();
    NetSync::ReplicationClient &replication = *rig.client->play.Session().Client();
    const NetSync::NetId own = replication.NetIdOf(rig.player);
    const float start = rig.host->world.scene.Get<ECS::Transform>(rig.crate)->position.z;
    const std::uint64_t correctionsBefore = replication.PredictionCountsOf(own)->physics.corrections;

    rig.session.Run(kPushFrames, [&] { Walk(rig.client->world.scene, rig.player); });

    const NetSync::ConnectionDiagnostics *diagnostics = server.Diagnostics(server.ConnectionOf(rig.ClientId()));
    REQUIRE(diagnostics != nullptr);
    REQUIRE(rig.host->world.scene.Get<ECS::Transform>(rig.crate)->position.z < start - kPushedFar);
    CHECK(server.SimulatorOf(rig.crate) == rig.ClientId());
    CHECK(replication.Simulates(rig.crateMirror));
    CHECK(diagnostics->reportsAccepted > 0u);
    CHECK(diagnostics->reportsRefused == 0u);
    MESSAGE("corrections while pushing " << replication.PredictionCountsOf(own)->physics.corrections - correctionsBefore
                                         << ", rollbacks " << replication.Corrections().predictionsReplayed);
    CHECK(replication.PredictionCountsOf(own)->physics.corrections == correctionsBefore);
}

TEST_CASE("With the sync test on, a client pushing a crate steps each tick again as it did live")
{
    Pushing rig;
    NetSync::ReplicationClient &replication = *rig.client->play.Session().Client();
    replication.SetSyncTest(true);

    rig.session.Run(kPushFrames, [&] { Walk(rig.client->world.scene, rig.player); });

    REQUIRE(replication.Simulates(rig.crateMirror));
    CHECK(replication.SyncTestResults().replays > 0u);
    CHECK(replication.SyncTestResults().differences == 0u);
}

TEST_CASE("A crate a second player takes over pushing while the first stands by changes hands once")
{
    const std::filesystem::path root = FreshRoot("assisi_players_bodies_handover");
    WritePlayerBlueprint(root);
    Session session;
    Machine &host = session.Add();
    Machine &first = session.Add();
    Machine &second = session.Add();
    for (const std::unique_ptr<Machine> &machine : session.machines)
    {
        const ECS::Entity floor = machine->world.scene.Create();
        (void)machine->world.scene.Add<ECS::Transform>(floor, ECS::Transform{.position = {0.f, -1.f, 0.f}});
        Physics::Collider collider;
        collider.shape = Physics::ColliderShape::Box;
        collider.halfExtents = {50.f, 1.f, 50.f};
        (void)machine->world.scene.Add<Physics::Collider>(floor, collider);
    }
    App::PlayerStart startAt;
    startAt.blueprint = Core::AssetPath{std::string_view{"player.abp"}};
    for (const glm::vec3 at : {kSecondClientStart, kHostStart, kFirstClientStart})
    {
        const ECS::Entity start = host.world.scene.Create();
        (void)host.world.scene.Add<ECS::Transform>(start, ECS::Transform{.position = at});
        (void)host.world.scene.Add<App::PlayerStart>(start, startAt);
    }
    const std::uint16_t port = HostOnFreePort(host.play);
    REQUIRE(port != 0);
    REQUIRE(first.play.Join("127.0.0.1", port));
    session.Run(kJoinSteps);
    REQUIRE(second.play.Join("127.0.0.1", port));
    session.Run(kJoinSteps);

    const ECS::Entity firstPlayer = LocalPlayerOf(first.world.scene);
    const ECS::Entity secondPlayer = LocalPlayerOf(second.world.scene);
    REQUIRE(firstPlayer != ECS::NullEntity);
    REQUIRE(secondPlayer != ECS::NullEntity);
    const NetSync::ClientId firstId = first.play.Session().LocalClientId();
    const NetSync::ClientId secondId = second.play.Session().LocalClientId();
    const ECS::Entity crate = AddCrate(host, kFirstClientStart);
    NetSync::ReplicationServer &server = *host.play.Session().Server();

    Simulators simulators;

    // The first player walks towards the crate until it is given it, before
    // reaching it, and stands there.
    session.Run(kPushFrames / 2,
                [&]
    {
        Walk(first.world.scene, firstPlayer, server.SimulatorOf(crate) != firstId);
        simulators.Watch(server, crate);
    });
    REQUIRE(server.SimulatorOf(crate) == firstId);

    // The second turns to face it and pushes it away sideways.
    bool turned = false;
    session.Run(kPushFrames,
                [&]
    {
        Walk(first.world.scene, firstPlayer, /*walking=*/ false);
        WalkAt(second.world.scene, secondPlayer, kQuarterRight, turned);
        simulators.Watch(server, crate);
    });

    REQUIRE(!simulators.seen.empty());
    CHECK(simulators.seen.front() == firstId.value);
    CHECK(simulators.seen.back() == secondId.value);
    CHECK(simulators.seen.size() == 2u);
    NetSync::ReplicationClient &secondReplication = *second.play.Session().Client();
    CHECK(secondReplication.Simulates(secondReplication.EntityOf(server.NetIdOf(crate))));
}
