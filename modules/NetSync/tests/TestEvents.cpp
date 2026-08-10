/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestEvents.cpp
/// @brief What the authority says happened, and who is told.
///
/// Two forms, and the difference between them is where the ordering comes from.
/// An unreliable event rides the snapshot *after* the entity blocks, so a
/// message about an entity spawned in that same packet finds the entity already
/// there — ordering for free, out of the framing. A reliable announcement
/// travels on a different lane and can overtake the state it describes, so it
/// carries a tick stamp and the client holds it until its world has caught up.
///
/// Recipients are computed, never enumerated. That is a security property as
/// much as a design one: an arbitrary per-call connection list is the API
/// through which an event leaks exactly what state filtering withholds.
///
/// See docs/replication-messaging-relevancy-plan-v1.md M5.

#include <doctest/doctest.h>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Net/NetTransport.hpp>
#include <Assisi/NetSync/MessageDispatch.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>
#include <Assisi/NetSync/ReplicationConfig.hpp>
#include <Assisi/NetSync/ReplicationProviders.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>
#include <Assisi/NetSync/TestMessageHandlers.hpp>
#include <Assisi/NetSync/TestNetComponents.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

using namespace Assisi;
using namespace Assisi::NetSync;
using namespace Assisi::NetSync::Test;

namespace
{

/// A provider whose membership a test names outright, so every scoping decision
/// below is a decision the test made rather than a consequence of geometry.
class ScriptedProvider final : public RelevancyProvider
{
  public:
    void Set(std::vector<NetId> members)
    {
        _members = std::move(members);
        std::sort(_members.begin(), _members.end());
    }

    void Compute(const RelevancyQuery &query, std::vector<NetId> &out) override
    {
        (void)query;
        out.assign(_members.begin(), _members.end());
    }

  private:
    std::vector<NetId> _members;
};

/// One server and as many clients as a recipient question needs.
struct Harness
{
    Net::NetTransport transport;
    ECS::Scene        serverScene;

    ReplicationServer server;
    std::uint64_t     tick = 0;

    struct Peer
    {
        std::unique_ptr<ECS::Scene>        scene;
        std::unique_ptr<ReplicationClient> client;
        Net::ConnectionId                  serverSide = Net::InvalidConnection;
        Net::ConnectionId                  clientSide = Net::InvalidConnection;
    };

    std::vector<Peer> peers;

    explicit Harness(ReplicationConfig config = {}) : server(transport, serverScene, nullptr, config)
    {
        HandlerLog::Instance().Clear();
    }

    std::size_t AddPeer()
    {
        Peer peer;
        const auto pair = transport.CreateLoopbackPair();
        peer.serverSide = pair.first;
        peer.clientSide = pair.second;
        peer.scene      = std::make_unique<ECS::Scene>();
        peer.client     = std::make_unique<ReplicationClient>(transport, *peer.scene, peer.clientSide);
        peer.client->SetContentSetHash(0);
        peers.push_back(std::move(peer));
        server.SetContentSetHash(0);
        server.AddConnection(peers.back().serverSide);
        return peers.size() - 1;
    }

    void Step()
    {
        std::vector<Net::NetEvent> events;
        transport.Poll(events);
        for (const Net::NetEvent &event : events)
        {
            if (event.type != Net::NetEvent::Type::Message)
                continue;
            for (Peer &peer : peers)
            {
                if (event.connection == peer.serverSide)
                    server.HandleMessage(peer.serverSide, event.payload);
                else if (event.connection == peer.clientSide)
                    peer.client->HandleMessage(event.payload);
            }
        }
        serverScene.FlushDestroyed();
        server.Tick(tick++);
    }

    void Step(std::uint32_t times)
    {
        for (std::uint32_t i = 0; i < times; ++i)
            Step();
    }

    [[nodiscard]] const ConnectionDiagnostics &Diagnostics(std::size_t peer) const
    {
        return *server.Diagnostics(peers[peer].serverSide);
    }
};

ECS::Entity SpawnReplicated(ECS::Scene &scene)
{
    const ECS::Entity entity = scene.Create();
    (void)scene.Add<ECS::Transform>(entity, ECS::Transform{});
    (void)scene.Add<Replicated>(entity, Replicated{});
    return entity;
}

} // namespace

TEST_CASE("an all-relevant event reaches every client that can see its subject")
{
    Harness harness;
    const std::size_t watcher  = harness.AddPeer();
    const std::size_t bystander = harness.AddPeer();
    harness.Step(4);

    const ECS::Entity subject = SpawnReplicated(harness.serverScene);
    harness.Step(8);

    harness.server.Send(TestBurst{subject, /*intensity=*/12});
    harness.Step(6);

    // Both clients hold the entity, so both are told.
    CHECK(harness.peers[watcher].client->EventsDispatched() == 1);
    CHECK(harness.peers[bystander].client->EventsDispatched() == 1);
    CHECK(HandlerLog::Instance().burstCalls == 3); // two clients, plus the host
    CHECK(harness.Diagnostics(watcher).eventsSent == 1);
}

TEST_CASE("the host is told about its own world's events")
{
    // The authority sees everything, and without a local queue the person
    // hosting would be the one participant who never hears about anything —
    // chat, this design's own example, would be invisible to them.
    Harness harness;
    harness.AddPeer();
    harness.Step(4);

    const ECS::Entity subject = SpawnReplicated(harness.serverScene);
    harness.Step(4);

    harness.server.Send(TestBurst{subject, /*intensity=*/3});
    harness.Step(2);

    CHECK(HandlerLog::Instance().burstCalls >= 1);
    CHECK(HandlerLog::Instance().lastBurst.intensity == 3);
}

TEST_CASE("an event about an entity a connection cannot see never reaches it")
{
    // The zero-bytes guarantee covering messages and not only state. Without
    // this, filtering would withhold an entity's position while announcing
    // everything that happens to it.
    Harness harness;
    const std::size_t seeing = harness.AddPeer();
    const std::size_t blind  = harness.AddPeer();

    auto *provider = new ScriptedProvider();
    harness.server.SetRelevancyProvider(std::unique_ptr<RelevancyProvider>(provider));
    harness.Step(4);

    const ECS::Entity subject = SpawnReplicated(harness.serverScene);
    harness.Step(2);

    // Only the first connection is told about it at all.
    provider->Set({harness.server.NetIdOf(subject)});
    harness.Step(10);

    // ...but the provider is global here, so both would see it. Revoke for the
    // second by granting explicitly to the first only.
    provider->Set({});
    harness.server.GrantRelevance(harness.peers[seeing].serverSide, harness.server.NetIdOf(subject));
    harness.Step(10);

    REQUIRE(harness.peers[seeing].client->EntityOf(harness.server.NetIdOf(subject)) != ECS::NullEntity);
    REQUIRE(harness.peers[blind].client->EntityOf(harness.server.NetIdOf(subject)) == ECS::NullEntity);

    const std::uint64_t before = harness.peers[blind].client->EventsDispatched();
    harness.server.Send(TestBurst{subject, /*intensity=*/1});
    harness.Step(8);

    CHECK(harness.peers[seeing].client->EventsDispatched() == 1);
    CHECK(harness.peers[blind].client->EventsDispatched() == before);
    CHECK(harness.Diagnostics(blind).eventsSent == 0);
}

TEST_CASE("an independent event goes to everyone, because there is nothing to scope it by")
{
    Harness harness;
    const std::size_t first  = harness.AddPeer();
    const std::size_t second = harness.AddPeer();

    // A provider that says nothing at all is relevant — which changes nothing
    // for a message that names no entity.
    harness.server.SetRelevancyProvider(std::make_unique<ScriptedProvider>());
    harness.Step(6);

    harness.server.Send(TestAnnounce{/*round=*/4});
    harness.Step(6);

    CHECK(harness.peers[first].client->EventsDispatched() == 1);
    CHECK(harness.peers[second].client->EventsDispatched() == 1);
    CHECK(HandlerLog::Instance().lastAnnounce.round == 4);
}

TEST_CASE("a directed event reaches exactly its recipient")
{
    Harness harness;
    const std::size_t owner   = harness.AddPeer();
    const std::size_t someone = harness.AddPeer();
    harness.Step(4);

    const ECS::Entity pawn = SpawnReplicated(harness.serverScene);
    harness.server.SetControl(pawn, harness.server.ClientIdOf(harness.peers[owner].serverSide));
    harness.Step(8);

    harness.server.SendToController(pawn, TestBurst{pawn, /*intensity=*/7});
    harness.Step(6);

    CHECK(harness.peers[owner].client->EventsDispatched() == 1);
    CHECK(harness.peers[someone].client->EventsDispatched() == 0);
}

TEST_CASE("a directed event with nobody to direct it at is dropped and counted")
{
    Harness harness;
    harness.AddPeer();
    harness.Step(4);

    const ECS::Entity orphan = SpawnReplicated(harness.serverScene);
    harness.Step(4);

    // Uncontrolled: there is no controller to address, and guessing would mean
    // picking somebody.
    harness.server.SendToController(orphan, TestBurst{orphan, /*intensity=*/1});
    harness.Step(4);

    CHECK(harness.server.HostDiagnostics().eventsUndeliverable == 1);
    CHECK(harness.peers[0].client->EventsDispatched() == 0);
}

TEST_CASE("except-instigator excludes exactly the instigator")
{
    // For events the instigator has already shown itself locally. Everyone else
    // still hears about it.
    Harness harness;
    const std::size_t actor    = harness.AddPeer();
    const std::size_t observer = harness.AddPeer();
    harness.Step(4);

    const ECS::Entity subject = SpawnReplicated(harness.serverScene);
    harness.Step(8);

    const ClientId instigator = harness.server.ClientIdOf(harness.peers[actor].serverSide);
    harness.server.SendExcept(instigator, TestBurst{subject, /*intensity=*/2});
    harness.Step(6);

    CHECK(harness.peers[actor].client->EventsDispatched() == 0);
    CHECK(harness.peers[observer].client->EventsDispatched() == 1);
}

TEST_CASE("the host can be the excluded instigator like anyone else")
{
    Harness harness;
    const std::size_t observer = harness.AddPeer();
    harness.Step(4);

    const ECS::Entity subject = SpawnReplicated(harness.serverScene);
    harness.Step(8);

    const std::uint32_t before = HandlerLog::Instance().burstCalls;
    harness.server.SendExcept(HostClientId, TestBurst{subject, /*intensity=*/5});
    harness.Step(6);

    // The remote observer heard it; the host — which is the only other listener
    // — did not, so the total moved by exactly one.
    CHECK(harness.peers[observer].client->EventsDispatched() == 1);
    CHECK(HandlerLog::Instance().burstCalls == before + 1);
}

TEST_CASE("an event about an entity spawned in the same packet arrives after the spawn")
{
    // The ordering guarantee, and it costs nothing: the section is written after
    // the entity blocks, so the framing already puts them in the right order.
    Harness harness;
    const std::size_t peer = harness.AddPeer();
    harness.Step(4);

    const ECS::Entity subject = SpawnReplicated(harness.serverScene);
    // Sent before the client has ever heard of the entity — the spawn and the
    // event will be in the very same snapshot.
    harness.server.Send(TestBurst{subject, /*intensity=*/8});
    harness.Step(6);

    const NetId netId = harness.server.NetIdOf(subject);
    REQUIRE(harness.peers[peer].client->EntityOf(netId) != ECS::NullEntity);
    CHECK(harness.peers[peer].client->EventsDispatched() == 1);
    // The handler saw a resolved local mirror, not a null handle — which is what
    // "after the spawn" actually means.
    CHECK(HandlerLog::Instance().lastBurst.source != ECS::NullEntity);
}

TEST_CASE("an event about an entity the connection cannot hold yet is held, then delivered once")
{
    // Held, not dropped: a state can wait for the next tick because the next
    // tick restates it, while an event cannot be regenerated.
    ReplicationConfig config;
    config.maxSnapshotBytes = 120; // small enough that a big world pages in

    Harness harness(config);
    const std::size_t peer = harness.AddPeer();
    harness.Step(4);

    // A crowd, so the entity we care about is well down the queue.
    for (std::int32_t i = 0; i < 40; ++i)
        (void)SpawnReplicated(harness.serverScene);
    const ECS::Entity late = SpawnReplicated(harness.serverScene);
    harness.Step(2);

    harness.server.Send(TestBurst{late, /*intensity=*/11});
    harness.Step(2);

    // Not yet: the client has not been told the entity exists.
    CHECK(harness.peers[peer].client->EventsDispatched() == 0);
    CHECK(harness.Diagnostics(peer).eventsHeld == 1);

    harness.Step(200);

    // Exactly once, when the entity finally lands.
    CHECK(harness.peers[peer].client->EventsDispatched() == 1);
    CHECK(harness.Diagnostics(peer).eventsHeld == 0);
    CHECK(harness.Diagnostics(peer).eventsSent == 1);
}

TEST_CASE("a held event whose subject despawns is evicted and counted")
{
    ReplicationConfig config;
    config.maxSnapshotBytes = 120;

    Harness harness(config);
    const std::size_t peer = harness.AddPeer();
    harness.Step(4);

    for (std::int32_t i = 0; i < 40; ++i)
        (void)SpawnReplicated(harness.serverScene);
    const ECS::Entity doomed = SpawnReplicated(harness.serverScene);
    harness.Step(2);

    harness.server.Send(TestBurst{doomed, /*intensity=*/1});
    harness.Step(2);
    REQUIRE(harness.Diagnostics(peer).eventsHeld == 1);

    // The entity dies before the client ever hears of it, so the event is now
    // about nothing.
    harness.serverScene.Destroy(doomed);
    harness.Step(6);

    CHECK(harness.Diagnostics(peer).eventsEvicted == 1);
    CHECK(harness.Diagnostics(peer).eventsHeld == 0);
    CHECK(harness.peers[peer].client->EventsDispatched() == 0);
}

TEST_CASE("a held queue at its cap drops the oldest and counts it")
{
    ReplicationConfig config;
    config.maxSnapshotBytes            = 120;
    config.maxHeldEventsPerConnection = 4;

    Harness harness(config);
    const std::size_t peer = harness.AddPeer();
    harness.Step(4);

    for (std::int32_t i = 0; i < 40; ++i)
        (void)SpawnReplicated(harness.serverScene);
    const ECS::Entity late = SpawnReplicated(harness.serverScene);
    harness.Step(2);

    for (std::int32_t i = 0; i < 10; ++i)
        harness.server.Send(TestBurst{late, /*intensity=*/i});
    harness.Step(2);

    CHECK(harness.Diagnostics(peer).eventsHeld <= 4);
    CHECK(harness.Diagnostics(peer).eventsOverflowed == 6);
}

TEST_CASE("a reliable announcement waits for the world it describes")
{
    // The control lane is not the snapshot lane, so an announcement can and does
    // overtake the state it is about. The tick stamp is what stops a handler
    // from acting on a world that has not happened yet.
    Harness harness;
    const std::size_t peer = harness.AddPeer();
    harness.Step(6);

    harness.server.Send(TestAnnounce{/*round=*/9});
    harness.Step(6);

    CHECK(harness.peers[peer].client->EventsDispatched() == 1);
    CHECK(harness.peers[peer].client->DeferredAnnouncementCount() == 0);
    CHECK(HandlerLog::Instance().lastAnnounce.round == 9);
    CHECK(harness.Diagnostics(peer).announcementsSent == 1);
    // ...and it went out reliably rather than riding a snapshot.
    CHECK(harness.Diagnostics(peer).eventsSent == 0);
}

TEST_CASE("events cost nothing when nothing is queued")
{
    // The message section reserves bytes only when there is something to put in
    // them. A game that sends no events must not pay for the feature — the same
    // rule that makes relevancy free without a provider.
    Harness harness;
    const std::size_t peer = harness.AddPeer();
    harness.Step(4);

    for (std::int32_t i = 0; i < 5; ++i)
        (void)SpawnReplicated(harness.serverScene);
    harness.Step(20);

    const ConnectionDiagnostics &diagnostics = harness.Diagnostics(peer);
    CHECK(diagnostics.eventsSent == 0);
    CHECK(diagnostics.eventsHeld == 0);
    CHECK(harness.peers[peer].client->ReplicatedEntityCount() == 5);
}
