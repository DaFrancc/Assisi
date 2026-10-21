/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPlayerSessionGaps.cpp
/// @brief The host steps a gap in a client's commands on its last controls held, then
///        let go, and tells the client which.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>

#include "PlayerSessionTesting.hpp"

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/PlayerSession.hpp>
#include <Assisi/NetSync/InputCommand.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>

using namespace Assisi;
using namespace Assisi::App::Testing;

namespace
{

/// Steps in a row a client sends nothing: past the host's hold, so the host
/// holds some and lets go of the rest.
constexpr std::int32_t kSilentSteps = static_cast<std::int32_t>(App::kInputHoldTicks) + 2;

} // namespace

TEST_CASE("A remote player's controls are held through a short gap in commands and released after it")
{
    App::PlayerInput controls;
    NetSync::InputCommand command;
    command.moveY = 1.f;
    command.actionBits = 0b1u;
    App::RelayHeldCommand(controls, &command, 0);
    REQUIRE(controls.move.y == 1.f);
    REQUIRE(controls.pressed.bits == 0b1u);

    App::RelayHeldCommand(controls, nullptr, App::kInputHoldTicks);
    CHECK(controls.move.y == 1.f);
    CHECK(controls.held.bits == 0b1u);

    App::RelayHeldCommand(controls, nullptr, App::kInputHoldTicks + 1);
    CHECK(controls.move.y == 0.f);
    CHECK(controls.held.bits == 0u);
    // A press the step has not used yet still counts: it happened.
    CHECK(controls.pressed.bits == 0b1u);
}

TEST_CASE("The host steps a gap in a client's commands on held then released controls, and tells the client which")
{
    const std::filesystem::path root = FreshRoot("assisi_players_gaps");
    WritePlayerBlueprint(root);

    Session session;
    Machine &host = session.Add();
    Machine &client = session.Add();
    JoinOnFloor(session, host, client);

    const ECS::Entity own = LocalPlayerOf(client.world.scene);
    const ECS::Entity onHost = PlayerOf(host, client.play.Session().LocalClientId());
    REQUIRE(own != ECS::NullEntity);
    REQUIRE(onHost != ECS::NullEntity);

    std::map<std::uint64_t, float> hostWalk;
    host.afterStep = [&host, &hostWalk, onHost](std::uint64_t tick)
                     {
                         if (const App::PlayerInput *controls = host.world.scene.Get<App::PlayerInput>(onHost))
                         {
                             hostWalk[tick] = controls->move.y;
                         }
                     };
    const std::function<void()> walk = [&client, own]
                                       {
                                           if (App::PlayerInput *controls = client.world.scene.GetMut<App::PlayerInput>(own))
                                           {
                                               controls->move = {0.f, 1.f};
                                           }
                                       };

    session.Run(kWalkBeforeGap, walk);
    const std::size_t gapFrom = client.stepTicks.size();
    client.silentSteps = kSilentSteps;
    session.Run(kSilentSteps + kCatchUpFrames, walk);
    REQUIRE(client.stepTicks.size() > gapFrom + static_cast<std::size_t>(kSilentSteps));

    const NetSync::CommandEcho &echo = client.play.Session().Client()->Echo();
    CHECK(echo.Applied(client.stepTicks[gapFrom - 1]));
    CHECK(echo.Applied(client.stepTicks[gapFrom + static_cast<std::size_t>(kSilentSteps)]));
    for (std::int32_t k = 0; k < kSilentSteps; ++k)
    {
        const std::uint64_t tick = client.stepTicks[gapFrom + static_cast<std::size_t>(k)];
        CAPTURE(k);
        REQUIRE(hostWalk.contains(tick));
        // Held while the gap is short, let go once it is not.
        const bool held = k < static_cast<std::int32_t>(App::kInputHoldTicks);
        CHECK(hostWalk.at(tick) == (held ? 1.f : 0.f));
        CHECK_FALSE(echo.Applied(tick));
    }
}
