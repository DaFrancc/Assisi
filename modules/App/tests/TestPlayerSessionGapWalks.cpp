/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPlayerSessionGapWalks.cpp
/// @brief A client walking on through a gap in its commands is pulled back only when
///        the host let go of its walk.

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>

#include "PlayerSessionTesting.hpp"

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/PlayerSession.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/BodyState.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>

using namespace Assisi;
using namespace Assisi::App::Testing;

namespace
{

/// Steps in a row a client sends nothing, short enough that the host holds its
/// walk through all of them.
constexpr std::int32_t kShortGapSteps = static_cast<std::int32_t>(App::kInputHoldTicks) - 1;

/// ...and long enough that the host lets go for most of it, and its character
/// stops well short of where the client walked on to.
constexpr std::int32_t kLongGapSteps = static_cast<std::int32_t>(App::kInputHoldTicks) * 4;

/// A client walking straight on, with @p gapSteps steps in the middle that
/// send nothing; where its character was after each step it took, and where
/// the host had it after each of its own.
struct GapWalk
{
    std::map<std::uint64_t, glm::vec3> onClient;
    std::map<std::uint64_t, glm::vec3> onHost;
    std::uint64_t replaysBefore = 0;
    std::uint64_t replaysAfter = 0;
};

GapWalk WalkThroughGap(std::int32_t gapSteps)
{
    const std::filesystem::path root = FreshRoot("assisi_players_gap_walks");
    WritePlayerBlueprint(root);

    Session session;
    Machine &host = session.Add();
    Machine &client = session.Add();
    JoinOnFloor(session, host, client);

    const ECS::Entity own = LocalPlayerOf(client.world.scene);
    const ECS::Entity onHost = PlayerOf(host, client.play.Session().LocalClientId());
    REQUIRE(own != ECS::NullEntity);
    REQUIRE(onHost != ECS::NullEntity);

    GapWalk result;
    client.afterStep = [&client, &result, own](std::uint64_t tick)
                       { result.onClient[tick] = client.world.scene.Get<ECS::Transform>(own)->position; };
    host.afterStep = [&host, &result, onHost](std::uint64_t tick)
                     { result.onHost[tick] = host.world.scene.Get<ECS::Transform>(onHost)->position; };
    const std::function<void()> walk = [&client, own]
                                       {
                                           if (App::PlayerInput *controls = client.world.scene.GetMut<App::PlayerInput>(own))
                                           {
                                               controls->move = {0.f, 1.f};
                                           }
                                       };

    session.Run(kWalkBeforeGap, walk);
    result.replaysBefore = client.play.Session().Client()->Corrections().predictionsReplayed;
    client.silentSteps = gapSteps;
    session.Run(gapSteps + kCatchUpFrames, walk);
    result.replaysAfter = client.play.Session().Client()->Corrections().predictionsReplayed;
    return result;
}

} // namespace

TEST_CASE("A client walking on through a gap the host holds its controls over is never pulled back")
{
    const GapWalk walk = WalkThroughGap(kShortGapSteps);
    CHECK(walk.replaysAfter == walk.replaysBefore);
}

TEST_CASE("A client walking on through a gap the host lets go in ends where the host has it")
{
    // The host stops the character once the gap outlasts its hold; the client,
    // which walked on, is put back on the host's word and steps on from it.
    const GapWalk walk = WalkThroughGap(kLongGapSteps);
    CHECK(walk.replaysAfter > walk.replaysBefore);

    REQUIRE_FALSE(walk.onHost.empty());
    const std::uint64_t newestOnHost = walk.onHost.rbegin()->first;
    REQUIRE(walk.onClient.contains(newestOnHost));
    CHECK(glm::length(walk.onClient.at(newestOnHost) - walk.onHost.at(newestOnHost)) <
          NetSync::Smoothing().predictionTolerance);
}
