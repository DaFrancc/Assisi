/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPlayerSessionContact.cpp
/// @brief A client's own character in contact with another player: stepped again
///        where it was stepped live, kept while it presses, and left where it is
///        when the other walks into it.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>

#include "PlayerSessionTesting.hpp"

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/ECS/RenderOffset.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>
#include <Assisi/NetSync/ReplicationConfig.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>

using namespace Assisi;
using namespace Assisi::App::Testing;

namespace
{

/// A quarter turn (rad). The host's player starts further along x than the
/// client's; turned a quarter to the right, the client's faces it, and turned a
/// quarter to the left, the host's faces the client's.
constexpr float kQuarterTurn = 1.5707963f;

/// Closer than this (m), two characters' capsules are pressed together: two
/// radii and the padding between them, with a little room.
constexpr float kPressing = 0.7f;

/// Frames the sync test runs while the client presses: long enough to walk
/// the gap between the starts and press for a few seconds.
constexpr std::int32_t kSyncTestFrames = 240;

/// Snapshots the pressing is watched over.
constexpr std::int32_t kWatchedSnapshots = 200;

/// Frames those snapshots take at the default rates.
constexpr std::int32_t kWatchedFrames =
    kWatchedSnapshots * static_cast<std::int32_t>(NetSync::ReplicationConfig{}.tickRateHz /
                                                  NetSync::ReplicationConfig{}.snapshotHz);

/// How far a character pressed against may move and still count as not moved
/// (m): settling onto the floor, not a shove.
constexpr float kNotMoved = 0.05f;

/// The host's and the client's players, and the host's as the client has it.
struct Players
{
    ECS::Entity hostOwn{ECS::NullEntity};
    ECS::Entity clientOwn{ECS::NullEntity};
    ECS::Entity hostOnClient{ECS::NullEntity};
};

Players JoinedPlayers(Session &session, Machine &host, Machine &client)
{
    JoinOnFloor(session, host, client);
    Players players;
    players.hostOwn = PlayerOf(host, NetSync::HostClientId);
    players.clientOwn = LocalPlayerOf(client.world.scene);
    players.hostOnClient =
        client.play.Session().Client()->EntityOf(host.play.Session().Server()->NetIdOf(players.hostOwn));
    REQUIRE(players.hostOwn != ECS::NullEntity);
    REQUIRE(players.clientOwn != ECS::NullEntity);
    REQUIRE(players.hostOnClient != ECS::NullEntity);
    return players;
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

/// Whether the client draws its own character and the host's pressed together.
bool Pressing(ECS::Scene &scene, const Players &players)
{
    const glm::vec3 a = scene.Get<ECS::Transform>(players.hostOnClient)->position;
    const glm::vec3 b = scene.Get<ECS::Transform>(players.clientOwn)->position;
    return glm::length(glm::vec2(a.x - b.x, a.z - b.z)) < kPressing;
}

/// Where the client draws @p entity: its Transform and the correction it is
/// still easing away.
glm::vec3 Drawn(ECS::Scene &scene, ECS::Entity entity)
{
    const ECS::RenderOffset *offset = scene.Get<ECS::RenderOffset>(entity);
    return scene.Get<ECS::Transform>(entity)->position + (offset != nullptr ? offset->position : glm::vec3(0.f));
}

} // namespace

TEST_CASE("With the sync test on, a client pressing against another player steps each tick again as it did live")
{
    // Stepped again, the client's character has to meet the other player where
    // the live step met it, not where the host's word has since put it.
    const std::filesystem::path root = FreshRoot("assisi_players_contact");
    WritePlayerBlueprint(root);

    Session session;
    Machine &host = session.Add();
    Machine &client = session.Add();
    const Players players = JoinedPlayers(session, host, client);
    NetSync::ReplicationClient &replication = *client.play.Session().Client();
    replication.SetSyncTest(true);

    bool turned = false;
    std::int32_t framesPressing = 0;
    session.Run(kSyncTestFrames,
                [&]
    {
        WalkAt(client.world.scene, players.clientOwn, -kQuarterTurn, turned);
        framesPressing += Pressing(client.world.scene, players) ? 1 : 0;
    });

    REQUIRE(framesPressing > kSyncTestFrames / 2);
    CHECK(replication.SyncTestResults().replays > 0u);
    CHECK(replication.SyncTestResults().differences == 0u);
}

TEST_CASE("A client pressing against another player keeps its own prediction")
{
    // Pressed against a player it cannot shove, the client's character stands
    // where the host has it: the host's word should never disagree.
    const std::filesystem::path root = FreshRoot("assisi_players_contact");
    WritePlayerBlueprint(root);

    Session session;
    Machine &host = session.Add();
    Machine &client = session.Add();
    const Players players = JoinedPlayers(session, host, client);
    NetSync::ReplicationClient &replication = *client.play.Session().Client();
    const NetSync::NetId own = replication.NetIdOf(players.clientOwn);

    bool turned = false;
    std::int32_t framesPressing = 0;
    float largestOffset = 0.f;
    session.Run(kWatchedFrames,
                [&]
    {
        WalkAt(client.world.scene, players.clientOwn, -kQuarterTurn, turned);
        if (Pressing(client.world.scene, players))
        {
            ++framesPressing;
            if (const ECS::RenderOffset *offset = client.world.scene.Get<ECS::RenderOffset>(players.clientOwn))
            {
                largestOffset = std::max(largestOffset, glm::length(offset->position));
            }
        }
    });

    const NetSync::ReplicationClient::EntityPredictionCounts *counts = replication.PredictionCountsOf(own);
    REQUIRE(counts != nullptr);
    REQUIRE(framesPressing > kWatchedFrames / 2);
    REQUIRE(counts->physics.compares > 0u);
    MESSAGE("compares " << counts->physics.compares << ", corrections " << counts->physics.corrections
                        << ", rollbacks " << replication.Corrections().predictionsReplayed << ", largest offset "
                        << largestOffset << " m");
    CHECK(counts->physics.corrections == 0u);
    CHECK(largestOffset == 0.f);
}

TEST_CASE("The host's player walking into a client's character does not move it, on either machine")
{
    // Characters block each other, so both machines agree where the client's
    // character is, and nothing about it is corrected.
    const std::filesystem::path root = FreshRoot("assisi_players_contact");
    WritePlayerBlueprint(root);

    Session session;
    Machine &host = session.Add();
    Machine &client = session.Add();
    const Players players = JoinedPlayers(session, host, client);
    NetSync::ReplicationClient &replication = *client.play.Session().Client();
    const NetSync::NetId own = replication.NetIdOf(players.clientOwn);
    const ECS::Entity clientOnHost = PlayerOf(host, client.play.Session().LocalClientId());
    REQUIRE(clientOnHost != ECS::NullEntity);

    const glm::vec3 startOnClient = Drawn(client.world.scene, players.clientOwn);
    const glm::vec3 startOnHost = host.world.scene.Get<ECS::Transform>(clientOnHost)->position;
    bool turned = false;
    std::int32_t framesPressing = 0;
    session.Run(kWatchedFrames,
                [&]
    {
        WalkAt(host.world.scene, players.hostOwn, kQuarterTurn, turned);
        framesPressing += Pressing(client.world.scene, players) ? 1 : 0;
    });

    const NetSync::ReplicationClient::EntityPredictionCounts *counts = replication.PredictionCountsOf(own);
    REQUIRE(counts != nullptr);
    REQUIRE(framesPressing > kWatchedFrames / 2);
    MESSAGE("corrections " << counts->physics.corrections << ", rollbacks "
                           << replication.Corrections().predictionsReplayed);
    CHECK(glm::length(Drawn(client.world.scene, players.clientOwn) - startOnClient) < kNotMoved);
    CHECK(glm::length(host.world.scene.Get<ECS::Transform>(clientOnHost)->position - startOnHost) < kNotMoved);
    CHECK(counts->physics.corrections == 0u);
}
