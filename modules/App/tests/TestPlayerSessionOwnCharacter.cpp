/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPlayerSessionOwnCharacter.cpp
/// @brief A client's own character and camera answer it at once, whatever the host
///        sends back a round trip later.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "PlayerSessionTesting.hpp"

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>

using namespace Assisi;
using namespace Assisi::App::Testing;

namespace
{

/// The most frames a client's own character may take to start moving once
/// asked: the step that samples the controls, and one with nothing to step.
constexpr std::size_t kMostFramesToStart = 2;

/// The most a character walking straight on may move back in one frame (m):
/// rounding, not a pull towards where the host had it.
constexpr float kMostBackPerFrame = 0.001f;

/// How far counts as having started to move (m).
constexpr float kStartedMoving = 0.01f;

/// Further than a character walking for kLongWalkFrames falls short of (m).
constexpr float kLongWalkDistance = 1.f;

} // namespace

TEST_CASE("A client's own character moves the moment it is asked, and the host's answers never pull it back")
{
    // The host's answer for a tick arrives a round trip after the client
    // stepped it, from before the host had the client's latest commands. Taken
    // as it comes, it puts the character back where it stood, and it walks
    // only once the host has seen it asked to.
    const std::filesystem::path root = FreshRoot("assisi_players_own");
    WritePlayerBlueprint(root);

    Session session;
    Machine &host = session.Add();
    Machine &client = session.Add();

    JoinOnFloor(session, host, client);

    const ECS::Entity own = LocalPlayerOf(client.world.scene);
    REQUIRE(own != ECS::NullEntity);
    const std::uint64_t correctionsBefore = client.play.Session().Client()->Corrections().applied;

    // Where the client simulates it, a frame at a time: its Transform, without
    // the offset that only hides a correction from view.
    std::vector<glm::vec3> simulated;
    session.Run(kLongWalkFrames,
                [&]
    {
        simulated.push_back(client.world.scene.Get<ECS::Transform>(own)->position);
        if (App::PlayerInput *controls = client.world.scene.GetMut<App::PlayerInput>(own))
        {
            controls->move = {0.f, 1.f};
        }
    });
    simulated.push_back(client.world.scene.Get<ECS::Transform>(own)->position);

    // The host answered all along, and agreed every time.
    const NetSync::ReplicationClient::CorrectionStats &corrections = client.play.Session().Client()->Corrections();
    REQUIRE(corrections.applied > correctionsBefore);
    CHECK(corrections.predictionsKept > 0);
    CHECK(corrections.predictionsReplayed == 0);
    const glm::vec3 travelled = simulated.back() - simulated.front();
    REQUIRE(glm::length(travelled) > kLongWalkDistance);
    const glm::vec3 ahead = glm::normalize(travelled);

    std::size_t started = simulated.size();
    for (std::size_t i = 1; i < simulated.size(); ++i)
    {
        CAPTURE(i);
        CHECK(glm::dot(simulated[i] - simulated[i - 1], ahead) > -kMostBackPerFrame);
        if (started == simulated.size() && glm::dot(simulated[i] - simulated.front(), ahead) > kStartedMoving)
        {
            started = i;
        }
    }
    CHECK(started <= kMostFramesToStart);
}

TEST_CASE("A client's own camera looks up and down freely while the host sends where it looked")
{
    // The host sends every camera's pose, the client's own included, a round
    // trip after the client turned it. Written or interpolated over the client's
    // camera, each one pulls the view back towards where it was a moment ago.
    const std::filesystem::path root = FreshRoot("assisi_players_own");
    WritePlayerBlueprint(root);

    Session session;
    Machine &host = session.Add();
    Machine &client = session.Add();
    JoinOnFloor(session, host, client);

    const ECS::Entity own = LocalPlayerOf(client.world.scene);
    REQUIRE(own != ECS::NullEntity);

    // A small turn every frame, up and then down, well inside the camera's
    // limits; and the most a frame may move against it, which is none.
    constexpr float kLookPerFrame = 0.01f;
    constexpr std::int32_t kLookFrames = 40;
    constexpr float kMostAgainstTheLook = 1e-4f;

    for (const float direction : {1.f, -1.f})
    {
        CAPTURE(direction);
        float previous = PitchOfEye(client.world.scene, own);
        float worst = 0.f;
        session.Run(kLookFrames,
                    [&]
        {
            const float now = PitchOfEye(client.world.scene, own);
            worst = std::max(worst, (previous - now) * direction);
            previous = now;
            if (App::PlayerInput *controls = client.world.scene.GetMut<App::PlayerInput>(own))
            {
                controls->lookPitch = kLookPerFrame * direction;
            }
        });
        CHECK(worst < kMostAgainstTheLook);
    }
}
