/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPlayerSessionReach.cpp
/// @brief How far an entity is sent, in a real session with the distance
///        filter on: a player hundreds of kilometres from the host is sent
///        what is always relevant and what reaches that far, where the host
///        has it, and nothing else, while a player beside the host is sent all
///        of it.

#include <doctest/doctest.h>

#include <array>
#include <filesystem>

#include "PlayerSessionTesting.hpp"

#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>
#include <Assisi/NetSync/ReplicationConfig.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>

using namespace Assisi;
using namespace Assisi::App::Testing;

namespace
{

/// Where the far player stands (m): about 424 km from the origin.
constexpr glm::dvec3 kFarAway{300000.0, 0.0, -300000.0};

/// Where the host and the near player stand (m).
constexpr glm::dvec3 kHostStart{0.0, 0.0, 0.0};
constexpr glm::dvec3 kNearStart{4.0, 0.0, 0.0};

/// Where the entities under test stand (m): beside the host and the near
/// player, well inside the game's radius of both.
constexpr glm::dvec3 kBeside{2.0, 1.0, -10.0};

/// An entity radius that reaches the far player from beside the host (m).
constexpr float kPastFarPlayer = 500000.f;

/// How far a far player's copy may be from the host's (m).
constexpr double kSamePlace = 0.02;

/// The game's own radii, with the distance filter on.
NetSync::ReplicationConfig DistanceFiltered()
{
    NetSync::ReplicationConfig config;
    config.relevancy.provider = NetSync::RelevancyConfig::Provider::Distance;
    return config;
}

/// A replicated entity on @p host, kBeside, carrying @p marker.
ECS::Entity AddMarked(Machine &host, const NetSync::Replicated &marker)
{
    ECS::Scene &scene = host.world.scene;
    const ECS::Entity entity = scene.Create();
    (void)scene.Add<ECS::Transform>(entity, ECS::Transform{.position = kBeside});
    (void)scene.Add<NetSync::Replicated>(entity, marker);
    return entity;
}

} // namespace

TEST_CASE("A player 424 km away is sent what is always relevant and what reaches it, and nothing else")
{
    const std::filesystem::path root = FreshRoot("assisi_players_reach");
    WritePlayerBlueprint(root);
    Session session;
    Machine &host = session.Add(DistanceFiltered());
    Machine &nearby = session.Add();
    Machine &far = session.Add();
    AddFloors(session, kHostStart);
    AddFloors(session, kFarAway);
    const std::array<glm::dvec3, 3> starts{kFarAway, kHostStart, kNearStart};
    JoinTwo(session, host, nearby, far, starts);
    const ECS::Entity farPlayer = LocalPlayerOf(far.world.scene);
    REQUIRE(farPlayer != ECS::NullEntity);
    REQUIRE(glm::length(far.world.scene.Get<ECS::Transform>(farPlayer)->position - kFarAway) < 1.0);

    NetSync::Replicated always;
    always.relevance = NetSync::Relevance::Always;
    NetSync::Replicated reaching;
    reaching.relevanceRadius = kPastFarPlayer;
    const ECS::Entity beacon = AddMarked(host, always);
    const ECS::Entity flare = AddMarked(host, reaching);
    const ECS::Entity ordinary = AddMarked(host, NetSync::Replicated{});
    session.Run(kCatchUpFrames);

    const NetSync::ReplicationServer &server = *host.play.Session().Server();
    const NetSync::ReplicationClient &nearReplication = *nearby.play.Session().Client();
    const NetSync::ReplicationClient &farReplication = *far.play.Session().Client();

    CHECK(nearReplication.EntityOf(server.NetIdOf(beacon)) != ECS::NullEntity);
    CHECK(nearReplication.EntityOf(server.NetIdOf(flare)) != ECS::NullEntity);
    CHECK(nearReplication.EntityOf(server.NetIdOf(ordinary)) != ECS::NullEntity);

    CHECK(farReplication.EntityOf(server.NetIdOf(ordinary)) == ECS::NullEntity);
    for (const ECS::Entity sent : {beacon, flare})
    {
        const ECS::Entity mirror = farReplication.EntityOf(server.NetIdOf(sent));
        REQUIRE(mirror != ECS::NullEntity);
        CHECK(glm::length(far.world.scene.Get<ECS::Transform>(mirror)->position - kBeside) < kSamePlace);
    }
}
