/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestControl.cpp
/// @brief Who a connection *is*, and which entities are theirs.
///
/// Two mechanisms, tested together because they only mean anything together:
/// `ClientId` — a session-scoped participant identity that survives the trip
/// across the wire, unlike the transport handle it must never be confused with
/// — and `ControlledBy`, the component that names one.
///
/// The failure modes worth pinning are all lifecycle: an id reused after a
/// disconnect makes "who did this" a lie; a claim baked into a level file binds
/// an entity to whoever draws that id next session; a client that can write the
/// component decides for itself what it controls; and an entity resurrected by
/// the editor's play/stop restore leaves the reverse index describing a world
/// that no longer exists.
///
/// See docs/replication-messaging-relevancy-plan-v1.md M0.

#include <doctest/doctest.h>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Net/NetTransport.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>

#include <cstdint>
#include <vector>

using namespace Assisi;
using namespace Assisi::NetSync;

namespace
{

/// A server with as many independently-connectable clients as a test wants.
///
/// Deliberately not the single-pair harness the convergence suite uses: every
/// question here is about *which* client, and one client cannot answer any of
/// them.
struct ControlHarness
{
    Net::NetTransport transport;
    ECS::Scene serverScene;

    ReplicationServer server;
    std::uint64_t tick = 0;

    /// One joined client: its own scene, its own end of a loopback pair.
    struct Peer
    {
        std::unique_ptr<ECS::Scene>        scene;
        std::unique_ptr<ReplicationClient> client;
        Net::ConnectionId serverSide = Net::InvalidConnection;
        Net::ConnectionId clientSide = Net::InvalidConnection;
        bool attached   = true;
    };

    std::vector<Peer> peers;

    ControlHarness() : server(transport, serverScene) {}

    /// Connect one more client and register it with the server.
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

    /// Stop routing a peer's traffic and tell the server it is gone — the
    /// transport's Disconnected event, without waiting on a real timeout.
    void DropPeer(std::size_t index)
    {
        peers[index].attached = false;
        server.RemoveConnection(peers[index].serverSide);
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
                if (!peer.attached)
                    continue;
                if (event.connection == peer.serverSide)
                    server.HandleMessage(peer.serverSide, event.payload);
                else if (event.connection == peer.clientSide)
                    peer.client->HandleMessage(event.payload);
            }
        }

        // A disconnect sweep despawns entities, and a queued destroy is not a
        // destroy until something flushes it. The application's frame loop does
        // this; here it is the step.
        serverScene.FlushDestroyed();
        server.Tick(tick++);
    }

    void Step(std::uint32_t times)
    {
        for (std::uint32_t i = 0; i < times; ++i)
            Step();
    }
};

ECS::Entity SpawnReplicated(ECS::Scene &scene, glm::vec3 position = {})
{
    const ECS::Entity entity = scene.Create();
    ECS::Transform transform;
    transform.position = position;
    (void)scene.Add<ECS::Transform>(entity, transform);
    (void)scene.Add<Replicated>(entity, Replicated{});
    return entity;
}

/// The mirror's claim, or nothing. Two lookups deep because the whole question
/// is whether the component made the trip.
const ControlledBy *MirroredClaim(const ControlHarness &harness, std::size_t peer, ECS::Entity serverEntity)
{
    const NetId netId = harness.server.NetIdOf(serverEntity);
    if (netId == InvalidNetId)
        return nullptr;
    const ECS::Entity mirror = harness.peers[peer].client->EntityOf(netId);
    if (mirror == ECS::NullEntity)
        return nullptr;
    return harness.peers[peer].scene->Get<ControlledBy>(mirror);
}

} // namespace

TEST_CASE("the handshake tells a client who it is")
{
    ControlHarness harness;
    const std::size_t peer = harness.AddPeer();
    harness.Step(4);

    REQUIRE(harness.peers[peer].client->IsSynchronized());

    const ClientId assigned = harness.server.ClientIdOf(harness.peers[peer].serverSide);
    CHECK(assigned.IsValid());
    CHECK(assigned.value >= kFirstRemoteClientId);
    // The id the server allocated is the id the client believes it holds. If
    // these ever disagree, every ControlledBy comparison on that client is
    // wrong in a way nothing else reports.
    CHECK(harness.peers[peer].client->LocalClientId() == assigned);
    // ...and the map runs both ways.
    CHECK(harness.server.ConnectionOf(assigned) == harness.peers[peer].serverSide);
}

TEST_CASE("client ids are monotonic and never reused")
{
    ControlHarness harness;

    const std::size_t first = harness.AddPeer();
    harness.Step(4);
    const ClientId firstId = harness.server.ClientIdOf(harness.peers[first].serverSide);

    const std::size_t second = harness.AddPeer();
    harness.Step(4);
    const ClientId secondId = harness.server.ClientIdOf(harness.peers[second].serverSide);

    CHECK(secondId.value > firstId.value);

    // The first leaves, and a third arrives into the gap it left.
    harness.DropPeer(first);
    CHECK(harness.server.ClientIdOf(harness.peers[first].serverSide) == InvalidClientId);
    CHECK(harness.server.ConnectionOf(firstId) == Net::InvalidConnection);

    const std::size_t third = harness.AddPeer();
    harness.Step(4);
    const ClientId thirdId = harness.server.ClientIdOf(harness.peers[third].serverSide);

    // The freed id must not come back. A reused one makes a late-arriving
    // message from the departed client indistinguishable from the newcomer's,
    // and makes a log line about "client 2" ambiguous over the session.
    CHECK(thirdId != firstId);
    CHECK(thirdId.value > secondId.value);
}

TEST_CASE("nobody controls anything by default")
{
    ControlHarness harness;
    const std::size_t peer = harness.AddPeer();
    const ECS::Entity prop = SpawnReplicated(harness.serverScene);
    harness.Step(6);

    CHECK(harness.server.ControllerOf(prop) == InvalidClientId);
    CHECK(harness.server.ControlledEntities(HostClientId).empty());
    // Absent, not present-and-zero: an uncontrolled entity should cost nothing,
    // which it only does if the component genuinely is not there.
    CHECK(harness.serverScene.Get<ControlledBy>(prop) == nullptr);
    CHECK(MirroredClaim(harness, peer, prop) == nullptr);
}

TEST_CASE("control replicates to every client, not only to its controller")
{
    ControlHarness harness;
    const std::size_t owner     = harness.AddPeer();
    const std::size_t bystander = harness.AddPeer();
    harness.Step(4);

    const ECS::Entity pawn     = SpawnReplicated(harness.serverScene);
    const ClientId ownerId  = harness.server.ClientIdOf(harness.peers[owner].serverSide);
    harness.server.SetControl(pawn, ownerId);
    harness.Step(8);

    CHECK(harness.server.ControllerOf(pawn) == ownerId);

    const ControlledBy *onOwner = MirroredClaim(harness, owner, pawn);
    REQUIRE(onOwner != nullptr);
    CHECK(onOwner->client == ownerId.value);
    CHECK(harness.peers[owner].client->ControlsEntity(harness.peers[owner].client->EntityOf(
                                                          harness.server.NetIdOf(pawn))));

    // The bystander gets it too — name tags and team colours are ordinary
    // gameplay questions, and hiding the answer would need a whole new
    // per-connection field-condition mechanism to serve one component.
    const ControlledBy *onBystander = MirroredClaim(harness, bystander, pawn);
    REQUIRE(onBystander != nullptr);
    CHECK(onBystander->client == ownerId.value);
    CHECK_FALSE(harness.peers[bystander].client->ControlsEntity(
                    harness.peers[bystander].client->EntityOf(harness.server.NetIdOf(pawn))));
}

TEST_CASE("a transfer is an ordinary component delta")
{
    ControlHarness harness;
    const std::size_t first  = harness.AddPeer();
    const std::size_t second = harness.AddPeer();
    harness.Step(4);

    const ECS::Entity pawn     = SpawnReplicated(harness.serverScene);
    const ClientId firstId  = harness.server.ClientIdOf(harness.peers[first].serverSide);
    const ClientId secondId = harness.server.ClientIdOf(harness.peers[second].serverSide);

    harness.server.SetControl(pawn, firstId);
    harness.Step(8);
    REQUIRE(MirroredClaim(harness, second, pawn) != nullptr);
    CHECK(MirroredClaim(harness, second, pawn)->client == firstId.value);

    // One write on the server. No handover protocol, no acknowledgement, no
    // five-changes-at-once — which is precisely why there is no transfer race
    // to design around.
    harness.server.SetControl(pawn, secondId);
    harness.Step(8);

    CHECK(harness.server.ControllerOf(pawn) == secondId);
    REQUIRE(MirroredClaim(harness, first, pawn) != nullptr);
    CHECK(MirroredClaim(harness, first, pawn)->client == secondId.value);
    CHECK(MirroredClaim(harness, second, pawn)->client == secondId.value);

    // The index moved with it, rather than leaving the entity claimed twice.
    CHECK(harness.server.ControlledEntities(firstId).empty());
    REQUIRE(harness.server.ControlledEntities(secondId).size() == 1);
    CHECK(harness.server.ControlledEntities(secondId)[0] == pawn);
}

TEST_CASE("clearing control leaves the entity and drops only the claim")
{
    ControlHarness harness;
    const std::size_t peer = harness.AddPeer();
    harness.Step(4);

    const ECS::Entity pawn = SpawnReplicated(harness.serverScene);
    const ClientId id   = harness.server.ClientIdOf(harness.peers[peer].serverSide);
    harness.server.SetControl(pawn, id);
    harness.Step(8);
    REQUIRE(MirroredClaim(harness, peer, pawn) != nullptr);

    harness.server.ClearControl(pawn);
    harness.Step(8);

    CHECK(harness.serverScene.IsAlive(pawn));
    CHECK(harness.server.ControllerOf(pawn) == InvalidClientId);
    CHECK(harness.server.ControlledEntities(id).empty());
    // The removal rides the presence diff, like any other component removal.
    CHECK(MirroredClaim(harness, peer, pawn) == nullptr);
}

TEST_CASE("a disconnect despawns what its client owned, and only that")
{
    ControlHarness harness;
    const std::size_t leaver    = harness.AddPeer();
    const std::size_t bystander = harness.AddPeer();
    harness.Step(4);

    const ECS::Entity pawn    = SpawnReplicated(harness.serverScene, {1.f, 0.f, 0.f});
    const ECS::Entity vehicle = SpawnReplicated(harness.serverScene, {2.f, 0.f, 0.f});
    const ECS::Entity prop    = SpawnReplicated(harness.serverScene, {3.f, 0.f, 0.f});

    const ClientId leaverId = harness.server.ClientIdOf(harness.peers[leaver].serverSide);
    harness.server.SetControl(pawn, leaverId, /*despawnOnDisconnect=*/ true);
    harness.server.SetControl(vehicle, leaverId, /*despawnOnDisconnect=*/ false);
    harness.Step(8);

    const NetId pawnId    = harness.server.NetIdOf(pawn);
    const NetId vehicleId = harness.server.NetIdOf(vehicle);
    REQUIRE(pawnId != InvalidNetId);
    REQUIRE(harness.peers[bystander].client->EntityOf(pawnId) != ECS::NullEntity);

    harness.DropPeer(leaver);
    harness.Step(8);

    // The pawn was theirs and goes with them.
    CHECK_FALSE(harness.serverScene.IsAlive(pawn));
    // The vehicle was only borrowed: it stays, minus the claim.
    CHECK(harness.serverScene.IsAlive(vehicle));
    CHECK(harness.serverScene.Get<ControlledBy>(vehicle) == nullptr);
    // And the world nobody claimed is untouched.
    CHECK(harness.serverScene.IsAlive(prop));

    // End to end: the bystander sees the despawn, not just the server.
    CHECK(harness.peers[bystander].client->EntityOf(pawnId) == ECS::NullEntity);
    CHECK(harness.peers[bystander].client->EntityOf(vehicleId) != ECS::NullEntity);
    CHECK(MirroredClaim(harness, bystander, vehicle) == nullptr);

    CHECK(harness.server.ControlledEntities(leaverId).empty());
}

TEST_CASE("a level file's authored control is stripped when the session starts")
{
    Net::NetTransport transport;
    ECS::Scene scene;

    // What a level saved mid-session would contain: a claim on an id from a
    // session that is over.
    const ECS::Entity loaded = SpawnReplicated(scene);
    (void)scene.Add<ControlledBy>(loaded, ControlledBy{ /*client=*/ 7, /*despawnOnDisconnect=*/ true});
    REQUIRE(scene.Get<ControlledBy>(loaded) != nullptr);

    // Hosting is what starts a session.
    ReplicationServer server(transport, scene);

    // Left alone, client 7 would eventually connect and silently inherit an
    // entity nobody gave them.
    CHECK(scene.Get<ControlledBy>(loaded) == nullptr);
    CHECK(server.ControllerOf(loaded) == InvalidClientId);
}

TEST_CASE("a client cannot give itself control by writing the component")
{
    ControlHarness harness;
    const std::size_t peer = harness.AddPeer();
    harness.Step(4);

    const ECS::Entity pawn = SpawnReplicated(harness.serverScene);
    harness.Step(8);

    const NetId netId  = harness.server.NetIdOf(pawn);
    const ECS::Entity mirror = harness.peers[peer].client->EntityOf(netId);
    REQUIRE(mirror != ECS::NullEntity);

    // The client forges a claim on its own copy. There is no receive path for
    // component state on the server — authority is architectural, not a
    // permission bit — so this is a local fabrication and nothing more.
    const ClientId id = harness.peers[peer].client->LocalClientId();
    (void)harness.peers[peer].scene->Add<ControlledBy>(mirror, ControlledBy{id.value, true});
    harness.Step(8);

    CHECK(harness.server.ControllerOf(pawn) == InvalidClientId);
    CHECK(harness.server.ControlledEntities(id).empty());
    // ...and the server's own view never moved: the forgery exists nowhere but
    // the client's copy.
    CHECK(harness.serverScene.Get<ControlledBy>(pawn) == nullptr);
}

TEST_CASE("the control index survives an entity coming back from the dead")
{
    ControlHarness harness;
    const std::size_t peer = harness.AddPeer();
    harness.Step(4);

    const ECS::Entity pawn = SpawnReplicated(harness.serverScene);
    const ClientId id   = harness.server.ClientIdOf(harness.peers[peer].serverSide);
    harness.server.SetControl(pawn, id);
    harness.Step(6);
    REQUIRE(harness.server.ControlledEntities(id).size() == 1);

    // The editor's play/stop restore and undo-revive both do exactly this:
    // destroy the entity, then bring the same handle back and repopulate it —
    // outside every incremental hook a maintained index could hang off.
    harness.serverScene.Destroy(pawn);
    harness.serverScene.FlushDestroyed();
    harness.Step(2);
    CHECK(harness.server.ControlledEntities(id).empty());

    harness.serverScene.ReviveAt(pawn);
    (void)harness.serverScene.Add<ECS::Transform>(pawn, ECS::Transform{});
    (void)harness.serverScene.Add<Replicated>(pawn, Replicated{});
    (void)harness.serverScene.Add<ControlledBy>(pawn, ControlledBy{id.value, true});
    harness.Step(2);

    // Rebuilt from the scene, so the resurrection is simply seen.
    REQUIRE(harness.server.ControlledEntities(id).size() == 1);
    CHECK(harness.server.ControlledEntities(id)[0] == pawn);
    CHECK(harness.server.ControllerOf(pawn) == id);
}
