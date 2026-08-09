/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestDistanceRelevancy.cpp
/// @brief A radius around what you are looking at — and the three ways that
/// simple idea goes wrong.
///
/// The provider itself is a distance compare. Everything interesting is around
/// it: hysteresis in one direction only (symmetric dwell relocates the artefact
/// rather than removing it), fail-open when there is nothing to measure from
/// (filtering here is a bandwidth tool, so a missing viewpoint must mean seeing
/// everything, never seeing nothing), and the two escape classes that let an
/// entity opt out of the whole mechanism in either direction.
///
/// See docs/replication-messaging-relevancy-plan-v1.md M2.

#include <doctest/doctest.h>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Net/NetTransport.hpp>
#include <Assisi/NetSync/DistanceRelevancy.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>
#include <Assisi/NetSync/ReplicationConfig.hpp>
#include <Assisi/NetSync/ReplicationProviders.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>

#include <cstdint>
#include <memory>
#include <vector>

using namespace Assisi;
using namespace Assisi::NetSync;

namespace
{

/// One server, one client, and a Distance provider whose tuning the test names.
struct Harness
{
    Net::NetTransport transport;
    ECS::Scene        serverScene;
    ECS::Scene        clientScene;

    std::pair<Net::ConnectionId, Net::ConnectionId> pair;

    ReplicationServer server;
    ReplicationClient client;

    std::uint64_t tick = 0;

    static ReplicationConfig With(RelevancyConfig relevancy)
    {
        ReplicationConfig config;
        config.relevancy = relevancy;
        return config;
    }

    explicit Harness(RelevancyConfig relevancy)
        : pair(transport.CreateLoopbackPair()), server(transport, serverScene, /*physics=*/nullptr, With(relevancy)),
          client(transport, clientScene, pair.second)
    {
        server.SetContentSetHash(0);
        client.SetContentSetHash(0);
        server.AddConnection(pair.first);
    }

    [[nodiscard]] Net::ConnectionId serverSide() const { return pair.first; }

    void Step()
    {
        std::vector<Net::NetEvent> events;
        transport.Poll(events);
        for (const Net::NetEvent &event : events)
        {
            if (event.type != Net::NetEvent::Type::Message)
                continue;
            if (event.connection == pair.first)
                server.HandleMessage(pair.first, event.payload);
            else if (event.connection == pair.second)
                client.HandleMessage(event.payload);
        }

        serverScene.FlushDestroyed();
        server.Tick(tick++);
    }

    void Step(std::uint32_t times)
    {
        for (std::uint32_t i = 0; i < times; ++i)
            Step();
    }
};

ECS::Entity SpawnAt(ECS::Scene &scene, glm::vec3 position, Relevance relevance = Relevance::Default)
{
    const ECS::Entity entity = scene.Create();
    ECS::Transform    transform;
    transform.position = position;
    (void)scene.Add<ECS::Transform>(entity, transform);

    Replicated marker;
    marker.relevance = relevance;
    (void)scene.Add<Replicated>(entity, marker);
    return entity;
}

void MoveTo(ECS::Scene &scene, ECS::Entity entity, glm::vec3 position)
{
    scene.GetMut<ECS::Transform>(entity)->position = position;
}

RelevancyConfig Distance(float radius, float exitRadius, std::uint32_t dwellTicks)
{
    RelevancyConfig config;
    config.provider   = RelevancyConfig::Provider::Distance;
    config.radius     = radius;
    config.exitRadius = exitRadius;
    config.dwellTicks = dwellTicks;
    return config;
}

} // namespace

TEST_CASE("no relevancy block in the config means everyone sees everything")
{
    // The default has to be the free one. A game that says nothing about
    // relevancy must not pay for it — no provider is installed at all, so there
    // is no intersection to run.
    Harness harness(RelevancyConfig{});
    CHECK(harness.server.Relevancy() == nullptr);

    (void)SpawnAt(harness.serverScene, {0.f, 0.f, 0.f});
    (void)SpawnAt(harness.serverScene, {10000.f, 0.f, 0.f});
    harness.Step(12);

    CHECK(harness.client.ReplicatedEntityCount() == 2);
}

TEST_CASE("an anchorless connection is told about everything")
{
    // Fail open, and it is a decision rather than an oversight: filtering here
    // buys bandwidth, not secrecy, so "this spectator has no viewpoint yet"
    // must resolve to seeing the world. A game whose provider is an information
    // boundary owns the opposite choice inside that provider.
    Harness harness(Distance(10.f, 12.f, 0));
    REQUIRE(harness.server.Relevancy() != nullptr);

    (void)SpawnAt(harness.serverScene, {0.f, 0.f, 0.f});
    (void)SpawnAt(harness.serverScene, {5000.f, 0.f, 0.f});
    harness.Step(12);

    CHECK(harness.server.ViewAnchors(harness.serverSide()).empty());
    CHECK(harness.client.ReplicatedEntityCount() == 2);
}

TEST_CASE("entities inside the radius arrive and entities outside it do not")
{
    Harness harness(Distance(10.f, 12.f, 0));

    const ECS::Entity anchor = SpawnAt(harness.serverScene, {0.f, 0.f, 0.f});
    const ECS::Entity near   = SpawnAt(harness.serverScene, {5.f, 0.f, 0.f});
    const ECS::Entity far    = SpawnAt(harness.serverScene, {500.f, 0.f, 0.f});
    harness.Step(2);

    const ECS::Entity anchors[] = {anchor};
    harness.server.SetViewAnchors(harness.serverSide(), anchors);
    harness.Step(12);

    CHECK(harness.server.IsRelevant(harness.serverSide(), harness.server.NetIdOf(anchor)));
    CHECK(harness.server.IsRelevant(harness.serverSide(), harness.server.NetIdOf(near)));
    CHECK_FALSE(harness.server.IsRelevant(harness.serverSide(), harness.server.NetIdOf(far)));
    CHECK(harness.client.ReplicatedEntityCount() == 2);
}

TEST_CASE("entering is immediate, so an anchor teleport does not show an empty world")
{
    // The reason dwell gates revokes only. A symmetric dwell would make a level
    // transition or a spectator jump display nothing for its duration and then
    // pop the world in — which is the artefact hysteresis exists to prevent,
    // merely moved somewhere less obvious.
    Harness harness(Distance(10.f, 12.f, /*dwellTicks=*/600));

    const ECS::Entity anchor = SpawnAt(harness.serverScene, {0.f, 0.f, 0.f});
    const ECS::Entity distant = SpawnAt(harness.serverScene, {1000.f, 0.f, 0.f});
    harness.Step(2);

    const ECS::Entity anchors[] = {anchor};
    harness.server.SetViewAnchors(harness.serverSide(), anchors);
    harness.Step(9);
    REQUIRE_FALSE(harness.server.IsRelevant(harness.serverSide(), harness.server.NetIdOf(distant)));

    // The anchor jumps across the map. One snapshot later the world it landed
    // in is already being sent, despite a dwell of ten seconds.
    MoveTo(harness.serverScene, anchor, {1000.f, 0.f, 0.f});
    harness.Step(3);

    CHECK(harness.server.IsRelevant(harness.serverSide(), harness.server.NetIdOf(distant)));
}

TEST_CASE("an entity hovering on the boundary does not thrash")
{
    // The failure this whole mechanism exists to prevent: with a single radius,
    // an entity sitting at the cull distance leaves and re-enters on float
    // noise, and each crossing costs a despawn one way and a complete resend the
    // other.
    //
    // The dwell is zero here on purpose, so the two radii are the only thing
    // that can be doing the work: an entity oscillating across the *enter*
    // radius never once reaches the exit radius, so there is nothing to revoke
    // and no clock to run out.
    Harness harness(Distance(/*radius=*/10.f, /*exitRadius=*/15.f, /*dwellTicks=*/0));

    const ECS::Entity anchor  = SpawnAt(harness.serverScene, {0.f, 0.f, 0.f});
    const ECS::Entity hoverer = SpawnAt(harness.serverScene, {5.f, 0.f, 0.f});
    harness.Step(2);

    const ECS::Entity anchors[] = {anchor};
    harness.server.SetViewAnchors(harness.serverSide(), anchors);
    harness.Step(12);

    const NetId hovererId = harness.server.NetIdOf(hoverer);
    REQUIRE(harness.server.IsRelevant(harness.serverSide(), hovererId));

    const ConnectionDiagnostics *diagnostics = harness.server.Diagnostics(harness.serverSide());
    REQUIRE(diagnostics != nullptr);
    const std::uint64_t entersBefore = diagnostics->relevancyEnters;
    const std::uint64_t exitsBefore  = diagnostics->relevancyExits;

    // Twenty crossings of the *enter* radius, each one well inside the exit
    // radius. Under a single-radius scheme this is twenty despawn/respawn pairs.
    for (std::uint32_t i = 0; i < 20; ++i)
    {
        MoveTo(harness.serverScene, hoverer, {i % 2 == 0 ? 12.f : 8.f, 0.f, 0.f});
        harness.Step(3);
    }

    CHECK(diagnostics->relevancyEnters == entersBefore);
    CHECK(diagnostics->relevancyExits == exitsBefore);
    CHECK(harness.server.IsRelevant(harness.serverSide(), hovererId));
    CHECK(harness.client.EntityOf(hovererId) != ECS::NullEntity);
}

TEST_CASE("the dwell delays a revoke, and then allows it")
{
    Harness harness(Distance(/*radius=*/10.f, /*exitRadius=*/15.f, /*dwellTicks=*/30));

    const ECS::Entity anchor  = SpawnAt(harness.serverScene, {0.f, 0.f, 0.f});
    const ECS::Entity leaving = SpawnAt(harness.serverScene, {5.f, 0.f, 0.f});
    harness.Step(2);

    const ECS::Entity anchors[] = {anchor};
    harness.server.SetViewAnchors(harness.serverSide(), anchors);
    harness.Step(12);

    const NetId leavingId = harness.server.NetIdOf(leaving);
    REQUIRE(harness.server.IsRelevant(harness.serverSide(), leavingId));

    // Beyond the exit radius, but only just now. The dwell is thirty ticks, so
    // for the next handful of snapshots it is still a member.
    MoveTo(harness.serverScene, leaving, {100.f, 0.f, 0.f});
    harness.Step(6);
    CHECK(harness.server.IsRelevant(harness.serverSide(), leavingId));

    harness.Step(40);
    CHECK_FALSE(harness.server.IsRelevant(harness.serverSide(), leavingId));
    CHECK(harness.client.EntityOf(leavingId) == ECS::NullEntity);
}

TEST_CASE("coming back inside the exit radius resets the dwell")
{
    Harness harness(Distance(/*radius=*/10.f, /*exitRadius=*/15.f, /*dwellTicks=*/30));

    const ECS::Entity anchor   = SpawnAt(harness.serverScene, {0.f, 0.f, 0.f});
    const ECS::Entity wanderer = SpawnAt(harness.serverScene, {5.f, 0.f, 0.f});
    harness.Step(2);

    const ECS::Entity anchors[] = {anchor};
    harness.server.SetViewAnchors(harness.serverSide(), anchors);
    harness.Step(12);

    const NetId wandererId = harness.server.NetIdOf(wanderer);

    // Out, back, out, back — never long enough to serve the dwell. The clock
    // must restart each time it comes home, not accumulate.
    for (std::uint32_t i = 0; i < 6; ++i)
    {
        MoveTo(harness.serverScene, wanderer, {100.f, 0.f, 0.f});
        harness.Step(15);
        MoveTo(harness.serverScene, wanderer, {5.f, 0.f, 0.f});
        harness.Step(15);
    }

    CHECK(harness.server.IsRelevant(harness.serverSide(), wandererId));
}

TEST_CASE("Relevance::Always survives a provider that wants nothing")
{
    // A radius is a bandwidth tool, not a correctness tool: an objective marker
    // that vanishes at sixty metres is a bug the saving does not pay for.
    Harness harness(Distance(1.f, 2.f, 0));

    const ECS::Entity anchor = SpawnAt(harness.serverScene, {0.f, 0.f, 0.f});
    const ECS::Entity beacon = SpawnAt(harness.serverScene, {9000.f, 0.f, 0.f}, Relevance::Always);
    const ECS::Entity ordinary = SpawnAt(harness.serverScene, {9000.f, 0.f, 0.f});
    harness.Step(2);

    const ECS::Entity anchors[] = {anchor};
    harness.server.SetViewAnchors(harness.serverSide(), anchors);
    harness.Step(12);

    CHECK(harness.server.IsRelevant(harness.serverSide(), harness.server.NetIdOf(beacon)));
    CHECK_FALSE(harness.server.IsRelevant(harness.serverSide(), harness.server.NetIdOf(ordinary)));
    CHECK(harness.client.EntityOf(harness.server.NetIdOf(beacon)) != ECS::NullEntity);
}

TEST_CASE("Relevance::ControllerOnly reaches its controller and nobody else")
{
    Harness harness(Distance(1000.f, 1200.f, 0));

    const ECS::Entity anchor  = SpawnAt(harness.serverScene, {0.f, 0.f, 0.f});
    const ECS::Entity private_ = SpawnAt(harness.serverScene, {1.f, 0.f, 0.f}, Relevance::ControllerOnly);
    harness.Step(4);

    const ECS::Entity anchors[] = {anchor};
    harness.server.SetViewAnchors(harness.serverSide(), anchors);
    harness.Step(10);

    const NetId privateId = harness.server.NetIdOf(private_);

    // Well inside the radius, and still withheld: uncontrolled means nobody,
    // which is the honest reading of "only the controller may see it".
    CHECK_FALSE(harness.server.IsRelevant(harness.serverSide(), privateId));
    CHECK(harness.client.EntityOf(privateId) == ECS::NullEntity);

    harness.server.SetControl(private_, harness.server.ClientIdOf(harness.serverSide()));
    harness.Step(12);

    CHECK(harness.server.IsRelevant(harness.serverSide(), privateId));
    CHECK(harness.client.EntityOf(privateId) != ECS::NullEntity);
}

TEST_CASE("ControllerOnly outranks an explicit grant")
{
    // The one class that is not a preference. "Only this player may know about
    // it" has to beat the provider, a grant, and Always alike, or it is not a
    // privacy statement at all.
    Harness harness(Distance(1.f, 2.f, 0));

    const ECS::Entity anchor = SpawnAt(harness.serverScene, {0.f, 0.f, 0.f});
    const ECS::Entity someoneElses =
        SpawnAt(harness.serverScene, {9000.f, 0.f, 0.f}, Relevance::ControllerOnly);
    harness.Step(4);

    const ECS::Entity anchors[] = {anchor};
    harness.server.SetViewAnchors(harness.serverSide(), anchors);

    const NetId netId = harness.server.NetIdOf(someoneElses);
    harness.server.GrantRelevance(harness.serverSide(), netId);
    // Controlled by an id this connection does not hold.
    harness.server.SetControl(someoneElses, ClientId{999});
    harness.Step(12);

    CHECK_FALSE(harness.server.IsRelevant(harness.serverSide(), netId));
    CHECK(harness.client.EntityOf(netId) == ECS::NullEntity);
}

TEST_CASE("anchors default to what the connection controls")
{
    Harness harness(Distance(10.f, 12.f, 0));
    harness.Step(4);

    const ECS::Entity pawn = SpawnAt(harness.serverScene, {100.f, 0.f, 0.f});
    const ECS::Entity near = SpawnAt(harness.serverScene, {105.f, 0.f, 0.f});
    (void)SpawnAt(harness.serverScene, {0.f, 0.f, 0.f}); // near the origin, far from the pawn
    harness.Step(2);

    harness.server.SetControl(pawn, harness.server.ClientIdOf(harness.serverSide()));
    harness.Step(12);

    // Nothing set an anchor, so the pawn is one — and the world around the pawn
    // arrives rather than the world around the origin.
    REQUIRE(harness.server.ViewAnchors(harness.serverSide()).size() == 1);
    CHECK(harness.server.ViewAnchors(harness.serverSide())[0] == pawn);
    CHECK(harness.server.IsRelevant(harness.serverSide(), harness.server.NetIdOf(near)));
    CHECK(harness.client.ReplicatedEntityCount() == 2);
}

TEST_CASE("an explicit anchor overrides the controlled default")
{
    Harness harness(Distance(10.f, 12.f, 0));
    harness.Step(4);

    const ECS::Entity pawn   = SpawnAt(harness.serverScene, {100.f, 0.f, 0.f});
    const ECS::Entity camera = SpawnAt(harness.serverScene, {0.f, 0.f, 0.f});
    const ECS::Entity nearCamera = SpawnAt(harness.serverScene, {3.f, 0.f, 0.f});
    harness.Step(2);

    harness.server.SetControl(pawn, harness.server.ClientIdOf(harness.serverSide()));

    // Watching from somewhere other than the pawn — the spectator case, and
    // exactly why the anchor is session state rather than derived from control.
    const ECS::Entity anchors[] = {camera};
    harness.server.SetViewAnchors(harness.serverSide(), anchors);
    harness.Step(12);

    CHECK(harness.server.IsRelevant(harness.serverSide(), harness.server.NetIdOf(nearCamera)));
    // ...and the pawn is still there, because a controller always holds its
    // subject regardless of where it happens to be looking.
    CHECK(harness.server.IsRelevant(harness.serverSide(), harness.server.NetIdOf(pawn)));
}

TEST_CASE("an exit radius that is not wider than the enter radius is corrected")
{
    // Silently accepting it would switch hysteresis off, and the symptom is
    // bandwidth rather than an error — the worst combination.
    const DistanceRelevancy provider(Distance(50.f, 40.f, 10));
    CHECK(provider.Config().exitRadius > provider.Config().radius);
}

TEST_CASE("the relevant-set size and the enter/exit counters are reported")
{
    Harness harness(Distance(10.f, 12.f, 0));

    const ECS::Entity anchor = SpawnAt(harness.serverScene, {0.f, 0.f, 0.f});
    const ECS::Entity mover  = SpawnAt(harness.serverScene, {5.f, 0.f, 0.f});
    harness.Step(2);

    const ECS::Entity anchors[] = {anchor};
    harness.server.SetViewAnchors(harness.serverSide(), anchors);
    harness.Step(12);

    const ConnectionDiagnostics *diagnostics = harness.server.Diagnostics(harness.serverSide());
    REQUIRE(diagnostics != nullptr);
    CHECK(diagnostics->relevantEntities == 2);
    CHECK(diagnostics->relevancyEnters == 2);
    CHECK(diagnostics->relevancyExits == 0);

    MoveTo(harness.serverScene, mover, {500.f, 0.f, 0.f});
    harness.Step(12);

    CHECK(diagnostics->relevantEntities == 1);
    CHECK(diagnostics->relevancyExits == 1);
}
