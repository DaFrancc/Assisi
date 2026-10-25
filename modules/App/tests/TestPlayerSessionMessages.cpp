/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPlayerSessionMessages.cpp
/// @brief A message a system sends through ctx.net on one machine is read as
///        Received<T> by the systems of another, from whoever sent it, once.

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "PlayerSessionTesting.hpp"

#include <Assisi/App/Network.hpp>
#include <Assisi/App/TestMessages.hpp>
#include <Assisi/Core/ClientId.hpp>
#include <Assisi/Core/Received.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>

using namespace Assisi;
using namespace Assisi::App::Testing;
using App::Test::TestAskBell;
using App::Test::TestBellRang;

namespace
{

/// Frames for an intent to reach the host and its answer to come back.
constexpr std::int32_t kAnswerFrames = 60;

/// The bell a test asks for.
constexpr std::int32_t kBell = 7;

/// A machine's systems for a bell: one that asks once when told to, one that
/// answers every ask by telling everyone, and one that notes every answer heard.
struct Bells
{
    /// Asked for on the next tick, through this character, then cleared.
    ECS::Entity askThrough = ECS::NullEntity;
    bool ask = false;

    std::vector<Core::Received<TestAskBell>> asks;
    std::vector<Core::Received<TestBellRang>> rangs;

    explicit Bells(App::World &world)
    {
        world.systems.Register(App::SystemPhase::FixedUpdate, "Ask", [this](App::SystemContext &ctx) { Ask(ctx); });
        world.systems.Register(App::SystemPhase::FixedUpdate, "Answer",
                               [this](App::SystemContext &ctx) { Answer(ctx); });
        world.systems.Register(App::SystemPhase::FixedUpdate, "Hear", [this](App::SystemContext &ctx) { Hear(ctx); });
    }

private:
    void Ask(App::SystemContext &ctx)
    {
        if (ask)
        {
            ask = false;
            CHECK(ctx.net.SendIntent(TestAskBell{.ringer = askThrough, .bell = kBell}));
        }
    }

    void Answer(App::SystemContext &ctx)
    {
        for (const Core::Received<TestAskBell> &received : ctx.events.Read<Core::Received<TestAskBell>>())
        {
            asks.push_back(received);
            CHECK(ctx.net.Send(TestBellRang{.bell = received.message.bell}));
        }
    }

    void Hear(App::SystemContext &ctx)
    {
        const std::span<const Core::Received<TestBellRang>> heard = ctx.events.Read<Core::Received<TestBellRang>>();
        rangs.insert(rangs.end(), heard.begin(), heard.end());
    }
};

} // namespace

TEST_CASE("ctx.net says which machine is the authority: a host, played or dedicated, and not a client")
{
    const std::filesystem::path root = FreshRoot("assisi_players_messages_roles");
    WritePlayerBlueprint(root);

    Session session;
    Machine &host = session.Add();
    Machine &client = session.Add();
    JoinOnFloor(session, host, client);

    const App::Network &hostNet = host.world.net;
    CHECK(hostNet.Role() == App::NetRole::Host);
    CHECK(hostNet.IsHost());
    CHECK(hostNet.IsAuthority());
    CHECK(hostNet.LocalClient() == Core::HostClientId);

    const App::Network &clientNet = client.world.net;
    CHECK(clientNet.Role() == App::NetRole::Client);
    CHECK(clientNet.IsClient());
    CHECK_FALSE(clientNet.IsAuthority());
    CHECK(clientNet.LocalClient() == client.play.Session().LocalClientId());
    CHECK(clientNet.LocalClient().value >= NetSync::kFirstRemoteClientId);

    // A client cannot speak for the host.
    CHECK_FALSE(client.world.net.Send(TestBellRang{.bell = 1}));

    Machine dedicated;
    REQUIRE(HostOnFreePort(dedicated.play, App::HostRole::Dedicated) != 0);
    CHECK(dedicated.world.net.Role() == App::NetRole::Host);
    CHECK(dedicated.world.net.IsAuthority());
}

TEST_CASE("A client's intent reaches the host's systems from that client, and the answer reaches every machine once")
{
    const std::filesystem::path root = FreshRoot("assisi_players_messages_ask");
    WritePlayerBlueprint(root);

    Session session;
    Machine &host = session.Add();
    Machine &client = session.Add();
    Bells hostBells(host.world);
    Bells clientBells(client.world);
    JoinOnFloor(session, host, client);

    // Every tick the client steps again would read the answer again if a rerun
    // could.
    client.play.Session().Client()->SetSyncTest(true);

    clientBells.askThrough = LocalPlayerOf(client.world.scene);
    REQUIRE(clientBells.askThrough != ECS::NullEntity);
    clientBells.ask = true;
    session.Run(kAnswerFrames);

    REQUIRE(hostBells.asks.size() == 1);
    const auto &[sender, ask] = hostBells.asks.front();
    CHECK(sender == client.play.Session().LocalClientId());
    CHECK(ask.ringer == PlayerOf(host, sender));
    CHECK(ask.bell == kBell);
    CHECK(clientBells.asks.empty());

    REQUIRE(clientBells.rangs.size() == 1);
    CHECK(clientBells.rangs.front().sender == Core::HostClientId);
    CHECK(clientBells.rangs.front().message.bell == kBell);
    REQUIRE(hostBells.rangs.size() == 1);
    CHECK(hostBells.rangs.front().message.bell == kBell);
}

TEST_CASE("The host's own player passes the same check, and is refused through a character it does not control")
{
    const std::filesystem::path root = FreshRoot("assisi_players_messages_host");
    WritePlayerBlueprint(root);

    Session session;
    Machine &host = session.Add();
    Machine &client = session.Add();
    Bells hostBells(host.world);
    JoinOnFloor(session, host, client);

    const ECS::Entity own = PlayerOf(host, Core::HostClientId);
    const ECS::Entity theirs = PlayerOf(host, client.play.Session().LocalClientId());
    REQUIRE(own != ECS::NullEntity);
    REQUIRE(theirs != ECS::NullEntity);

    hostBells.askThrough = theirs;
    hostBells.ask = true;
    session.Run(kCatchUpFrames);
    CHECK(hostBells.asks.empty());
    CHECK(host.play.Session().Server()->HostDiagnostics().intentsNotYours == 1);

    hostBells.askThrough = own;
    hostBells.ask = true;
    session.Run(kCatchUpFrames);
    REQUIRE(hostBells.asks.size() == 1);
    CHECK(hostBells.asks.front().sender == Core::HostClientId);
}
