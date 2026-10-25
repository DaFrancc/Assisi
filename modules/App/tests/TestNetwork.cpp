/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestNetwork.cpp
/// @brief ctx.net in a world played alone: a host nobody joined, so the game
///        code that sends and reads messages online works single-player too.

#include <doctest/doctest.h>

#include <Assisi/App/Network.hpp>
#include <Assisi/App/StepWorld.hpp>
#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/TestMessages.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/App/WorldFrame.hpp>
#include <Assisi/Core/ClientId.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/Received.hpp>

#include <functional>
#include <span>
#include <utility>
#include <vector>

using namespace Assisi::App;
using Assisi::App::Test::TestAskBell;
using Assisi::App::Test::TestBellRang;
namespace Core = Assisi::Core;
namespace ECS = Assisi::ECS;

namespace
{

constexpr float kStep = 1.f / 60.f;

/// Every @p T one run of a phase read, by run.
template <typename T>
using Reads = std::vector<std::vector<Core::Received<T>>>;

template <typename T>
std::vector<Core::Received<T>> ReadAll(SystemContext &ctx)
{
    const std::span<const Core::Received<T>> read = ctx.events.Read<Core::Received<T>>();
    return {read.begin(), read.end()};
}

/// Notes in @p reads every @p T @p phase reads each time it runs; last in the
/// phase.
template <typename T>
void RecordReads(World &world, SystemPhase phase, const char *name, Reads<T> &reads)
{
    world.systems.Register(phase, name, [&reads](SystemContext &ctx) { reads.push_back(ReadAll<T>(ctx)); }).Last();
}

/// A world played alone, ticked by hand, whose next tick can be made to send.
struct Alone
{
    Core::EventQueue universal{Core::EventScope::Universal};
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Alone");
    /// Called with ctx.net by the next tick's FixedUpdate, first, and then
    /// forgotten.
    std::function<void(Network &)> send;

    Alone()
    {
        world.systems.Register(SystemPhase::FixedUpdate, "Send", [this](SystemContext &ctx) { SendOnce(ctx); })
        .First();
    }

    SystemContext Context()
    {
        return {.world = world,
                .dt = kStep,
                .simTick = 0,
                .input = nullptr,
                .actions = nullptr,
                .universalEvents = universal,
                .isActiveWorld = true,
                .worldManager = &worlds};
    }

    void Tick() { StepWorld(Context()); }

private:
    void SendOnce(SystemContext &ctx)
    {
        if (send)
        {
            std::function<void(Network &)> now = std::move(send);
            send = nullptr;
            now(ctx.net);
        }
    }
};

/// Sends an event to this machine each way one can be addressed to it, bells 1
/// to 4 in order.
void SendEveryWayToSelf(Network &net, ECS::Entity bell)
{
    CHECK(net.Send(TestBellRang{.bell = 1}));
    CHECK(net.SendTo(Core::HostClientId, TestBellRang{.bell = 2}));
    CHECK(net.SendExcept(Core::ClientId{9}, TestBellRang{.bell = 3}));
    CHECK(net.SendToController(bell, TestBellRang{.bell = 4}));
}

/// Sends an event to a player who is not here, and one to everyone but this
/// machine's player.
void SendToOthers(Network &net)
{
    CHECK(net.SendTo(Core::ClientId{9}, TestBellRang{.bell = 1}));
    CHECK(net.SendExcept(Core::HostClientId, TestBellRang{.bell = 2}));
}

} // namespace

TEST_CASE("A world played alone is the authority, and its one player is the host")
{
    Alone alone;
    const Network &net = alone.world.net;
    CHECK(net.Role() == NetRole::Offline);
    CHECK(net.IsAuthority());
    CHECK_FALSE(net.IsHost());
    CHECK_FALSE(net.IsClient());
    CHECK(net.LocalClient() == Core::HostClientId);
}

TEST_CASE("Played alone, an intent one tick sends is read by the next tick and its frame, from the host, once")
{
    Alone alone;
    Reads<TestAskBell> ticks;
    Reads<TestAskBell> updates;
    RecordReads(alone.world, SystemPhase::FixedUpdate, "TickReads", ticks);
    RecordReads(alone.world, SystemPhase::Update, "UpdateReads", updates);

    alone.send = [](Network &net) { CHECK(net.SendIntent(TestAskBell{.ringer = ECS::NullEntity, .bell = 4})); };
    alone.Tick();
    alone.Tick();
    UpdateWorld(alone.Context());
    alone.Tick();

    REQUIRE(ticks.size() == 3);
    CHECK(ticks[0].empty());
    REQUIRE(ticks[1].size() == 1);
    const auto &[sender, ask] = ticks[1].front();
    CHECK(sender == Core::HostClientId);
    CHECK(ask.bell == 4);
    CHECK(ticks[2].empty());

    REQUIRE(updates.size() == 1);
    CHECK(updates[0].size() == 1);
}

TEST_CASE("Played alone, an event reaches the machine itself, however it is addressed to it")
{
    Alone alone;
    Reads<TestBellRang> ticks;
    RecordReads(alone.world, SystemPhase::FixedUpdate, "TickReads", ticks);

    const ECS::Entity bell = alone.world.scene.Create();
    alone.send = [bell](Network &net) { SendEveryWayToSelf(net, bell); };
    alone.Tick();
    alone.Tick();

    REQUIRE(ticks.size() == 2);
    REQUIRE(ticks[1].size() == 4);
    for (std::size_t i = 0; i < ticks[1].size(); ++i)
    {
        CHECK(ticks[1][i].sender == Core::HostClientId);
        CHECK(ticks[1][i].message.bell == static_cast<int32_t>(i) + 1);
    }
}

TEST_CASE("Played alone, an event for another player reaches nobody")
{
    Alone alone;
    Reads<TestBellRang> ticks;
    RecordReads(alone.world, SystemPhase::FixedUpdate, "TickReads", ticks);

    alone.send = &SendToOthers;
    alone.Tick();
    alone.Tick();

    REQUIRE(ticks.size() == 2);
    CHECK(ticks[1].empty());
}
