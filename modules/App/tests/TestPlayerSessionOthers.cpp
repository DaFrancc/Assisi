/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPlayerSessionOthers.cpp
/// @brief Another player is drawn where it stands: at its feet on the floor, still
///        while it presses against this one.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>

#include "PlayerSessionTesting.hpp"

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/ECS/RenderOffset.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>

using namespace Assisi;
using namespace Assisi::App::Testing;

namespace
{

/// How far off the floor another player walking on it may be drawn (m): a
/// position crosses the wire rounded to about two millimetres.
constexpr float kMostDrawnOffFloor = 0.005f;

/// A quarter turn to the left (rad), which faces a character at the start
/// further along x from one at the start nearer the origin.
constexpr float kQuarterTurn = 1.5707963f;

/// Closer than this (m), two characters' capsules are pressed together: two
/// radii and the padding between them, with a little room.
constexpr float kPressing = 0.7f;

/// The most another player pressing against this one may be drawn above or
/// below where this machine has it (m): well under the two millimetres a
/// position is rounded to on the wire.
constexpr float kMostShakeWhilePressing = 0.0005f;

/// Frames another player stands still after walking, for its last snapshots to
/// arrive and the corrections they make to be eased away.
constexpr std::int32_t kStandFrames = 60;

/// How far from where the host has it another player standing still may be
/// drawn (m): a few wire steps.
constexpr float kMostDrawnOffStanding = 0.01f;

/// Where @p scene draws @p entity: its Transform and the correction it is still
/// easing away.
glm::vec3 Drawn(ECS::Scene &scene, ECS::Entity entity)
{
    const ECS::RenderOffset *offset = scene.Get<ECS::RenderOffset>(entity);
    return scene.Get<ECS::Transform>(entity)->position + (offset != nullptr ? offset->position : glm::vec3(0.f));
}

} // namespace

TEST_CASE("Another player pressing against this one does not tremble up and down with the wire's rounding")
{
    // Standing on the floor, the other player's height differs from the host's
    // only by how a position is rounded to cross the wire. Taken as a correction
    // every snapshot, that rounding pulled it a millimetre down and the floor
    // stood it back up: a tremble, most visible while it pushes and barely moves.
    // The shove itself, sideways, is the host's to resolve and is not this.
    const std::filesystem::path root = FreshRoot("assisi_players_others");
    WritePlayerBlueprint(root);

    Session session;
    Machine &host = session.Add();
    Machine &client = session.Add();
    JoinOnFloor(session, host, client);

    const ECS::Entity hostOwn = PlayerOf(host, NetSync::HostClientId);
    const ECS::Entity clientOwn = LocalPlayerOf(client.world.scene);
    const ECS::Entity hostOnClient =
        client.play.Session().Client()->EntityOf(host.play.Session().Server()->NetIdOf(hostOwn));
    REQUIRE(hostOnClient != ECS::NullEntity);
    REQUIRE(clientOwn != ECS::NullEntity);

    // The host starts beside the client, further along x, and turns to walk at it.
    bool turned = false;
    float largestWhilePressing = 0.f;
    std::int32_t framesPressing = 0;
    session.Run(kLongWalkFrames,
                [&]
    {
        if (App::PlayerInput *controls = host.world.scene.GetMut<App::PlayerInput>(hostOwn))
        {
            controls->move = {0.f, 1.f};
            if (!turned)
            {
                controls->lookYaw = kQuarterTurn;
                turned = true;
            }
        }
        const glm::vec3 pusher = client.world.scene.Get<ECS::Transform>(hostOnClient)->position;
        const glm::vec3 pushed = client.world.scene.Get<ECS::Transform>(clientOwn)->position;
        if (glm::length(glm::vec2(pusher.x - pushed.x, pusher.z - pushed.z)) < kPressing)
        {
            ++framesPressing;
            if (const ECS::RenderOffset *offset = client.world.scene.Get<ECS::RenderOffset>(hostOnClient))
            {
                largestWhilePressing = std::max(largestWhilePressing, std::abs(offset->position.y));
            }
        }
    });

    REQUIRE(framesPressing > kLongWalkFrames / 2);
    CHECK(largestWhilePressing < kMostShakeWhilePressing);
}

TEST_CASE("Another player walking is drawn at its feet, without bobbing above the floor")
{
    // A correction is measured against where this machine had the character.
    // Taken from the capsule Jolt moves on its own rather than from the feet the
    // host sends, every correction read a few centimetres up or down as an error
    // and drew it eased away: the other player bobbed while it walked, and most
    // visibly while it pushed against someone and barely moved.
    const std::filesystem::path root = FreshRoot("assisi_players_others");
    WritePlayerBlueprint(root);

    Session session;
    Machine &host = session.Add();
    Machine &client = session.Add();
    JoinOnFloor(session, host, client);

    const ECS::Entity hostOwn = PlayerOf(host, NetSync::HostClientId);
    const ECS::Entity hostOnClient =
        client.play.Session().Client()->EntityOf(host.play.Session().Server()->NetIdOf(hostOwn));
    REQUIRE(hostOnClient != ECS::NullEntity);

    float highestDrawn = 0.f;
    session.Run(kLongWalkFrames,
                [&]
    {
        if (App::PlayerInput *controls = host.world.scene.GetMut<App::PlayerInput>(hostOwn))
        {
            controls->move = {0.f, 1.f};
        }
        const ECS::RenderOffset *offset = client.world.scene.Get<ECS::RenderOffset>(hostOnClient);
        const float drawn = client.world.scene.Get<ECS::Transform>(hostOnClient)->position.y +
                            (offset != nullptr ? offset->position.y : 0.f);
        highestDrawn = std::max(highestDrawn, std::abs(drawn));
    });

    CHECK(highestDrawn < kMostDrawnOffFloor);
}

TEST_CASE("With the sync test on, another player that walked and stopped is drawn where it stands")
{
    // The sync test steps the newest tick again with the other players placed
    // where its live step left them. Recorded again from there, a correction
    // the host's word made since was lost and the next tick put back: the
    // other player stayed where it started, drawn metres off and shaking.
    const std::filesystem::path root = FreshRoot("assisi_players_others");
    WritePlayerBlueprint(root);

    Session session;
    Machine &host = session.Add();
    Machine &client = session.Add();
    JoinOnFloor(session, host, client);

    const ECS::Entity hostOwn = PlayerOf(host, NetSync::HostClientId);
    NetSync::ReplicationClient &replication = *client.play.Session().Client();
    const ECS::Entity hostOnClient = replication.EntityOf(host.play.Session().Server()->NetIdOf(hostOwn));
    REQUIRE(hostOnClient != ECS::NullEntity);
    replication.SetSyncTest(true);

    // Controls hold until they are changed, so it walks and then stands.
    REQUIRE(host.world.scene.Has<App::PlayerInput>(hostOwn));
    host.world.scene.GetMut<App::PlayerInput>(hostOwn)->move = {0.f, 1.f};
    session.Run(kLongWalkFrames);
    host.world.scene.GetMut<App::PlayerInput>(hostOwn)->move = {0.f, 0.f};
    session.Run(kStandFrames);

    const glm::vec3 standing = host.world.scene.Get<ECS::Transform>(hostOwn)->position;
    REQUIRE(replication.SyncTestResults().replays > 0u);
    CHECK(glm::length(Drawn(client.world.scene, hostOnClient) - standing) < kMostDrawnOffStanding);
}
