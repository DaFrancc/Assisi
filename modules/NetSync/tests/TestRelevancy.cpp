/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestRelevancy.cpp
/// @brief Who is told about what — and the promise that everyone else is told
/// nothing at all.
///
/// Relevancy is one set per connection, intersected with the live set before
/// anything else runs. Three properties are only reachable in the window
/// between a revoke and its acknowledgement:
///
///  - the body-state pass is an independent walk with its own acked-based gate,
///    and acked does not imply relevant, so an entity that has left the set
///    keeps shipping motion for a whole round trip unless that pass filters too;
///  - an entity that leaves and comes back inside one round trip has an unacked
///    despawn in flight, so the ordinary path would send it a delta against a
///    baseline the client destroyed;
///  - a late ack for a pre-revoke snapshot can put that entity straight back
///    into the acked set unless the in-flight ring is cleaned too.
///
/// The zero-cost contract is separate: with no provider installed the wire
/// bytes must be *identical* to an identity-filter run.

#include <doctest/doctest.h>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Net/NetTransport.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>
#include <Assisi/NetSync/ReplicationConfig.hpp>
#include <Assisi/NetSync/ReplicationProviders.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

using namespace Assisi;
using namespace Assisi::NetSync;

namespace
{

/// A provider that returns exactly what a test tells it to, so every membership
/// change is a decision the test made on a named tick rather than a consequence
/// of geometry.
class ScriptedProvider final : public RelevancyProvider
{
public:
    /// Replaces the membership set. Nothing is relevant until a test grants it,
    /// so a test that forgets sees an empty world rather than a full one.
    void Set(std::vector<NetId> members)
    {
        _members = std::move(members);
        std::sort(_members.begin(), _members.end());
    }

    void Add(NetId netId)
    {
        _members.push_back(netId);
        std::sort(_members.begin(), _members.end());
        _members.erase(std::unique(_members.begin(), _members.end()), _members.end());
    }

    void Remove(NetId netId) { std::erase(_members, netId); }

    void Compute(const RelevancyQuery &query, std::vector<NetId> &out) override
    {
        (void)query;
        ++calls;
        out.assign(_members.begin(), _members.end());
    }

    void ForgetClient(ClientId client) override { forgotten.push_back(client); }

    std::uint32_t calls = 0;
    std::vector<ClientId> forgotten;

private:
    std::vector<NetId> _members;
};

/// The reference the zero-cost path is measured against: a provider that names
/// the live set verbatim, so its only effect is to run the machinery.
class IdentityProvider final : public RelevancyProvider
{
public:
    void Compute(const RelevancyQuery &query, std::vector<NetId> &out) override
    {
        out.assign(query.live.begin(), query.live.end());
    }
};

/// One server, one client, and a record of every byte the server sent.
struct Harness
{
    Net::NetTransport transport;
    ECS::Scene serverScene;
    ECS::Scene clientScene;

    std::pair<Net::ConnectionId, Net::ConnectionId> pair;

    ReplicationServer server;
    ReplicationClient client;

    std::uint64_t tick = 0;

    /// Every snapshot payload the server sent, in order — the subject of the
    /// byte-identical test.
    std::vector<std::vector<std::byte>> sent;

    /// While set, server→client traffic is recorded but not delivered.
    bool dropServerMessages = false;

    /// While set, client→server traffic is dropped, keeping acknowledgements out
    /// of the server's hands while the client carries on applying what arrives.
    /// That asymmetry is the exit window: the server still believes the client
    /// holds an entity the client has already destroyed.
    bool dropClientMessages = false;

    explicit Harness(ReplicationConfig config = {})
        : pair(transport.CreateLoopbackPair()), server(transport, serverScene, /*physics=*/ nullptr, config),
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
            {
                if (!dropClientMessages)
                    server.HandleMessage(pair.first, event.payload);
            }
            else if (event.connection == pair.second)
            {
                sent.emplace_back(event.payload.begin(), event.payload.end());
                if (!dropServerMessages)
                    client.HandleMessage(event.payload);
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

/// Bytes and snapshots together — the oracle for every zero-bytes claim below.
///
/// Payloads are bit-packed, so scanning them for an id finds coincidences
/// rather than references. "Contributes zero bytes" therefore means the byte
/// counter does not move, measured differentially against a phase of the same
/// connection where the entity is known to be silent.
///
/// Snapshots go out every third tick, so a fixed-length window of ticks holds
/// five or six of them depending on where it started — a difference in
/// alignment, not in cost, which comparing raw totals would report as one.
struct SendCost
{
    std::uint64_t bytes     = 0;
    std::uint64_t snapshots = 0;

    /// Exact rate comparison without dividing: a == b iff their cross products
    /// match. Integer arithmetic throughout, so there is nothing to round.
    friend bool SameRate(const SendCost &lhs, const SendCost &rhs)
    {
        return lhs.bytes * rhs.snapshots == rhs.bytes * lhs.snapshots;
    }
};

SendCost Measure(const ReplicationServer &server, Net::ConnectionId connection)
{
    const ConnectionDiagnostics *diagnostics = server.Diagnostics(connection);
    if (diagnostics == nullptr)
        return {};
    return SendCost{diagnostics->bytesSent, diagnostics->snapshotsSent};
}

SendCost Since(const SendCost &start, const SendCost &end)
{
    return SendCost{end.bytes - start.bytes, end.snapshots - start.snapshots};
}

} // namespace

TEST_CASE("no provider means no cost: the bytes are identical to an identity filter")
{
    // Two servers, built and driven identically, differing only in whether the
    // relevancy machinery runs at all. If the identity case is genuinely the
    // pre-relevancy path, these two byte streams cannot differ.
    Harness plain;
    Harness filtered;
    filtered.server.SetRelevancyProvider(std::make_unique<IdentityProvider>());

    for (std::uint32_t i = 0; i < 5; ++i)
    {
        (void)SpawnReplicated(plain.serverScene, {static_cast<float>(i), 0.f, 0.f});
        (void)SpawnReplicated(filtered.serverScene, {static_cast<float>(i), 0.f, 0.f});
    }

    plain.Step(20);
    filtered.Step(20);

    REQUIRE(plain.sent.size() == filtered.sent.size());
    REQUIRE_FALSE(plain.sent.empty());
    for (std::size_t i = 0; i < plain.sent.size(); ++i)
        CHECK(plain.sent[i] == filtered.sent[i]);

    CHECK(plain.client.ReplicatedEntityCount() == 5);
    CHECK(filtered.client.ReplicatedEntityCount() == 5);
}

TEST_CASE("a filtered connection is told about its set and nothing else")
{
    Harness harness;
    auto *provider = new ScriptedProvider();
    harness.server.SetRelevancyProvider(std::unique_ptr<RelevancyProvider>(provider));

    const ECS::Entity seen   = SpawnReplicated(harness.serverScene, {1.f, 0.f, 0.f});
    const ECS::Entity unseen = SpawnReplicated(harness.serverScene, {2.f, 0.f, 0.f});
    harness.Step(2);

    const NetId seenId   = harness.server.NetIdOf(seen);
    const NetId unseenId = harness.server.NetIdOf(unseen);
    REQUIRE(seenId != InvalidNetId);
    REQUIRE(unseenId != InvalidNetId);

    provider->Set({seenId});
    harness.Step(10);

    CHECK(harness.server.IsRelevant(harness.serverSide(), seenId));
    CHECK_FALSE(harness.server.IsRelevant(harness.serverSide(), unseenId));

    CHECK(harness.client.EntityOf(seenId) != ECS::NullEntity);
    CHECK(harness.client.EntityOf(unseenId) == ECS::NullEntity);
    CHECK(harness.client.ReplicatedEntityCount() == 1);
}

TEST_CASE("world completeness is per connection, against the set it can see")
{
    Harness harness;
    auto *provider = new ScriptedProvider();
    harness.server.SetRelevancyProvider(std::unique_ptr<RelevancyProvider>(provider));

    const ECS::Entity seen = SpawnReplicated(harness.serverScene, {1.f, 0.f, 0.f});
    (void)SpawnReplicated(harness.serverScene, {2.f, 0.f, 0.f});
    harness.Step(2);

    provider->Set({harness.server.NetIdOf(seen)});
    harness.Step(12);

    // One of two entities has arrived, and the client is nonetheless complete —
    // because completeness answers "have I got everything I am going to get",
    // which is the only version of the question a filtered client can act on.
    CHECK(harness.client.IsWorldComplete());
    CHECK(harness.client.ReplicatedEntityCount() == 1);
}

TEST_CASE("leaving the set is a despawn, and it heals like one")
{
    Harness harness;
    auto *provider = new ScriptedProvider();
    harness.server.SetRelevancyProvider(std::unique_ptr<RelevancyProvider>(provider));

    const ECS::Entity entity = SpawnReplicated(harness.serverScene, {1.f, 0.f, 0.f});
    harness.Step(2);
    const NetId netId = harness.server.NetIdOf(entity);
    REQUIRE(netId != InvalidNetId);

    provider->Set({netId});
    harness.Step(10);
    REQUIRE(harness.client.EntityOf(netId) != ECS::NullEntity);

    provider->Remove(netId);
    harness.Step(10);

    // The mirror is gone and the connection no longer holds it relevant — the
    // ordinary despawn path, reached by a different route.
    CHECK(harness.client.EntityOf(netId) == ECS::NullEntity);
    CHECK_FALSE(harness.server.IsRelevant(harness.serverSide(), netId));
    CHECK(harness.serverScene.IsAlive(entity)); // the entity itself is untouched

    // ...and re-entering brings it back through the spawn path.
    provider->Add(netId);
    harness.Step(10);
    CHECK(harness.client.EntityOf(netId) != ECS::NullEntity);
}

TEST_CASE("an entity outside the set costs zero bytes, and the mover proves it")
{
    Harness harness;
    auto *provider = new ScriptedProvider();
    harness.server.SetRelevancyProvider(std::unique_ptr<RelevancyProvider>(provider));

    const ECS::Entity kept    = SpawnReplicated(harness.serverScene, {1.f, 0.f, 0.f});
    const ECS::Entity revoked = SpawnReplicated(harness.serverScene, {2.f, 0.f, 0.f});
    harness.Step(2);

    const NetId keptId    = harness.server.NetIdOf(kept);
    const NetId revokedId = harness.server.NetIdOf(revoked);
    provider->Set({keptId, revokedId});
    harness.Step(12);
    REQUIRE(harness.client.EntityOf(revokedId) != ECS::NullEntity);

    // Revoke, and let the despawn round-trip fully. Everything after this point
    // is the steady state the guarantee is about.
    provider->Remove(revokedId);
    harness.Step(12);
    REQUIRE(harness.client.EntityOf(revokedId) == ECS::NullEntity);

    constexpr std::uint32_t kWindow = 16;

    // Baseline: nothing changes at all. Whatever a snapshot costs to say
    // "nothing happened", this is it.
    const SendCost idleStart = Measure(harness.server, harness.serverSide());
    harness.Step(kWindow);
    const SendCost idle = Since(idleStart, Measure(harness.server, harness.serverSide()));
    REQUIRE(idle.snapshots > 0);

    // Now the revoked entity moves, every single tick, and nothing else does.
    const SendCost revokedStart = Measure(harness.server, harness.serverSide());
    for (std::uint32_t i = 0; i < kWindow; ++i)
    {
        harness.serverScene.GetMut<ECS::Transform>(revoked)->position.x += 1.f;
        harness.Step();
    }
    const SendCost revokedMoving = Since(revokedStart, Measure(harness.server, harness.serverSide()));

    // Exactly the idle rate. Not "smaller", not "mostly" — a connection that is
    // not told about an entity is told nothing about it.
    CHECK(SameRate(revokedMoving, idle));

    // The positive control, without which the above would also pass on a server
    // that had simply stopped sending: the same edit to the entity that *is* in
    // the set costs real bytes.
    const SendCost keptStart = Measure(harness.server, harness.serverSide());
    for (std::uint32_t i = 0; i < kWindow; ++i)
    {
        harness.serverScene.GetMut<ECS::Transform>(kept)->position.x += 1.f;
        harness.Step();
    }
    const SendCost keptMoving = Since(keptStart, Measure(harness.server, harness.serverSide()));

    CHECK_FALSE(SameRate(keptMoving, idle));
    CHECK(keptMoving.bytes * idle.snapshots > idle.bytes * keptMoving.snapshots);
}

TEST_CASE("the body-state pass filters too, or zero bytes is a lie")
{
    // WriteBodyStates is an independent walk with its own acked-based gate, and
    // a revoked entity stays acked until its despawn round-trips, so that gate
    // says yes for the whole exit window and for as long as the despawn keeps
    // being resent. Walking the live set there rather than the filtered one
    // would ship a falling box the connection cannot see every snapshot.
    Net::NetTransport transport;
    ECS::Scene serverScene;
    ECS::Scene clientScene;
    Physics::PhysicsWorld serverPhysics;
    Physics::PhysicsWorld clientPhysics;

    const auto pair = transport.CreateLoopbackPair();
    ReplicationServer server(transport, serverScene, &serverPhysics);
    ReplicationClient client(transport, clientScene, pair.second, &clientPhysics);
    server.SetContentSetHash(0);
    client.SetContentSetHash(0);
    server.AddConnection(pair.first);

    auto *provider = new ScriptedProvider();
    server.SetRelevancyProvider(std::unique_ptr<RelevancyProvider>(provider));

    constexpr float kFixedStep         = 1.f / 60.f;
    std::uint64_t tick               = 0;
    bool dropClientMessages = false;
    const auto step               = [&](std::uint32_t times)
                                    {
                                        for (std::uint32_t i = 0; i < times; ++i)
                                        {
                                            std::vector<Net::NetEvent> events;
                                            transport.Poll(events);
                                            for (const Net::NetEvent &event : events)
                                            {
                                                if (event.type != Net::NetEvent::Type::Message)
                                                    continue;
                                                if (event.connection == pair.first)
                                                {
                                                    if (!dropClientMessages)
                                                        server.HandleMessage(pair.first, event.payload);
                                                }
                                                else if (event.connection == pair.second)
                                                {
                                                    client.HandleMessage(event.payload);
                                                }
                                            }
                                            serverPhysics.Update(kFixedStep);
                                            serverPhysics.CaptureState();
                                            clientPhysics.Update(kFixedStep);
                                            clientPhysics.CaptureState();
                                            client.EnforceSleep();
                                            server.Tick(tick++);
                                        }
                                    };

    // One box that will never stop falling — it is spawned high enough that it
    // is still in the air at the end of the test, so its body state is captured
    // on every single tick and there is always something to leak.
    const auto spawnBox = [&](glm::vec3 position)
                          {
                              const ECS::Entity entity = serverScene.Create();
                              ECS::Transform transform;
                              transform.position = position;
                              (void)serverScene.Add<ECS::Transform>(entity, transform);

                              Physics::RigidBodyDescriptor descriptor;
                              descriptor.shape       = Physics::ColliderShape::Box;
                              descriptor.halfExtents = glm::vec3{0.5f};
                              descriptor.isStatic    = false;
                              (void)serverScene.Add<Physics::RigidBodyDescriptor>(entity, descriptor);
                              (void)serverScene.Add<Replicated>(entity, Replicated{});
                              (void)serverPhysics.AddBodyFromDescriptor(serverScene, entity, transform, descriptor);
                              return entity;
                          };

    const ECS::Entity falling = spawnBox({0.f, 400.f, 0.f});
    step(4);
    const NetId fallingId = server.NetIdOf(falling);
    REQUIRE(fallingId != InvalidNetId);

    provider->Set({fallingId});
    step(20);
    REQUIRE(client.EntityOf(fallingId) != ECS::NullEntity);

    constexpr std::uint32_t kWindow = 20;

    // Both measurements are taken with the client's acknowledgements withheld,
    // because that *is* the exit window: the acked set is what the body-state
    // pass gates on, and an entity only leaves it when its despawn round-trips.
    // Measuring after the round trip completes would find the acked gate doing
    // the work and prove nothing about relevancy at all.
    dropClientMessages = true;

    // What a falling, visible body costs while nothing is being acked.
    const SendCost visibleStart = Measure(server, pair.first);
    step(kWindow);
    const SendCost visible = Since(visibleStart, Measure(server, pair.first));
    REQUIRE(visible.snapshots > 0);

    provider->Remove(fallingId);
    step(6); // two snapshots: the despawn goes out and the client destroys its mirror
    REQUIRE(client.EntityOf(fallingId) == ECS::NullEntity);

    // ...and what the same body costs once nobody is being told about it. It is
    // still falling, still being captured every tick, still in the physics
    // world, and still in the server's acked set — the only thing that changed
    // is who is listening.
    const SendCost hiddenStart = Measure(server, pair.first);
    step(kWindow);
    const SendCost hidden = Since(hiddenStart, Measure(server, pair.first));
    REQUIRE(hidden.snapshots > 0);

    CHECK(visible.bytes * hidden.snapshots > hidden.bytes * visible.snapshots);
    // The strong form: what remains is the snapshot header and the despawn
    // being resent. A single quantized body state is well over ten bytes, so a
    // per-snapshot average below that cannot contain one.
    CHECK(hidden.bytes < hidden.snapshots * 10u);
}

TEST_CASE("re-entering before the despawn acks sends full state, not a delta")
{
    // The corrupt-half-mirror case. The server's acked set still lists the
    // entity, so without the forget rule the next snapshot sends a *delta* —
    // and the client, which destroyed its mirror when the despawn landed,
    // rebuilds a fresh entity out of whichever components that delta carried.
    Harness harness;
    auto *provider = new ScriptedProvider();
    harness.server.SetRelevancyProvider(std::unique_ptr<RelevancyProvider>(provider));

    const ECS::Entity entity = SpawnReplicated(harness.serverScene, {5.f, 6.f, 7.f});
    harness.Step(2);
    const NetId netId = harness.server.NetIdOf(entity);

    provider->Set({netId});
    harness.Step(12);
    REQUIRE(harness.client.EntityOf(netId) != ECS::NullEntity);

    // The window: the client keeps receiving and acting on what arrives, but
    // its acknowledgements never reach the server. So the client destroys its
    // mirror on the despawn while the server goes on believing it holds one.
    harness.dropClientMessages = true;

    provider->Remove(netId);
    harness.Step(6);
    REQUIRE(harness.client.EntityOf(netId) == ECS::NullEntity); // the client really did let go

    provider->Add(netId);
    harness.Step(6);

    harness.dropClientMessages = false;
    harness.Step(12);

    // The mirror is whole, Transform included. Without the forget rule the
    // re-grant arrives as a delta that omits it, leaving the rebuilt entity with
    // no Transform until the next keyframe sweep.
    const ECS::Entity mirror = harness.client.EntityOf(netId);
    REQUIRE(mirror != ECS::NullEntity);
    const ECS::Transform *transform = harness.clientScene.Get<ECS::Transform>(mirror);
    REQUIRE(transform != nullptr);
    CHECK(transform->position.x == doctest::Approx(5.f));
    CHECK(transform->position.y == doctest::Approx(6.f));
    CHECK(transform->position.z == doctest::Approx(7.f));
}

TEST_CASE("an explicit grant outranks the provider")
{
    Harness harness;
    auto *provider = new ScriptedProvider();
    harness.server.SetRelevancyProvider(std::unique_ptr<RelevancyProvider>(provider));

    const ECS::Entity pinned = SpawnReplicated(harness.serverScene, {1.f, 0.f, 0.f});
    harness.Step(2);
    const NetId netId = harness.server.NetIdOf(pinned);

    // The provider says no; the grant says yes.
    provider->Set({});
    harness.server.GrantRelevance(harness.serverSide(), netId);
    harness.Step(12);

    CHECK(harness.server.IsRelevant(harness.serverSide(), netId));
    CHECK(harness.client.EntityOf(netId) != ECS::NullEntity);

    // ...and revoking the grant hands the decision back to the provider, which
    // still says no.
    harness.server.RevokeRelevance(harness.serverSide(), netId);
    harness.Step(10);
    CHECK_FALSE(harness.server.IsRelevant(harness.serverSide(), netId));
    CHECK(harness.client.EntityOf(netId) == ECS::NullEntity);
}

TEST_CASE("a connection always sees what it controls")
{
    // Without the implicit grant, a player's own pawn leaves its set whenever
    // the view anchor is elsewhere — fatal to prediction, which needs the
    // controller to always hold its subject.
    Harness harness;
    auto *provider = new ScriptedProvider();
    harness.server.SetRelevancyProvider(std::unique_ptr<RelevancyProvider>(provider));
    harness.Step(4);

    const ECS::Entity pawn = SpawnReplicated(harness.serverScene, {1.f, 0.f, 0.f});
    harness.Step(2);
    const NetId netId = harness.server.NetIdOf(pawn);
    const ClientId id    = harness.server.ClientIdOf(harness.serverSide());

    provider->Set({}); // the provider wants this connection to see nothing at all
    harness.server.SetControl(pawn, id);
    harness.Step(12);

    CHECK(harness.server.IsRelevant(harness.serverSide(), netId));
    CHECK(harness.client.EntityOf(netId) != ECS::NullEntity);

    // Losing control gives the provider back its say.
    harness.server.ClearControl(pawn);
    harness.Step(10);
    CHECK_FALSE(harness.server.IsRelevant(harness.serverSide(), netId));
}

TEST_CASE("priority does not climb for entities outside the set")
{
    // Filtering strictly precedes prioritization — the ordering every scaled
    // system converged on. An out-of-set entity accumulating priority would
    // eventually outrank everything the connection can actually see, and the
    // first snapshot after it re-entered would be nothing but backlog.
    ReplicationConfig config;
    config.maxSnapshotBytes = 240; // small enough that ordering is observable

    Harness harness(config);
    auto *provider = new ScriptedProvider();
    harness.server.SetRelevancyProvider(std::unique_ptr<RelevancyProvider>(provider));

    std::vector<ECS::Entity> entities;
    for (std::uint32_t i = 0; i < 8; ++i)
        entities.push_back(SpawnReplicated(harness.serverScene, {static_cast<float>(i), 0.f, 0.f}));
    harness.Step(2);

    const NetId first = harness.server.NetIdOf(entities.front());
    provider->Set({first});
    harness.Step(40);

    CHECK(harness.client.ReplicatedEntityCount() == 1);

    // Everything else enters at once. Accumulators are not observable from
    // here, so what is checked is the consequence: the world converges without
    // starving the entity that was visible the whole time.
    std::vector<NetId> all;
    for (const ECS::Entity entity : entities)
        all.push_back(harness.server.NetIdOf(entity));
    provider->Set(all);
    harness.Step(60);

    CHECK(harness.client.ReplicatedEntityCount() == entities.size());
    CHECK(harness.client.EntityOf(first) != ECS::NullEntity);
}

TEST_CASE("the keyframe sweep re-anchors a filtered connection to its own set")
{
    ReplicationConfig config;
    config.keyframeIntervalTicks = 12;

    Harness harness(config);
    auto *provider = new ScriptedProvider();
    harness.server.SetRelevancyProvider(std::unique_ptr<RelevancyProvider>(provider));

    const ECS::Entity seen = SpawnReplicated(harness.serverScene, {1.f, 0.f, 0.f});
    const ECS::Entity gone = SpawnReplicated(harness.serverScene, {2.f, 0.f, 0.f});
    harness.Step(2);

    provider->Set({harness.server.NetIdOf(seen)});
    harness.Step(60); // several sweeps

    // A sweep resends everything the connection is *entitled* to, which must
    // not quietly widen into everything that exists.
    CHECK(harness.client.ReplicatedEntityCount() == 1);
    CHECK(harness.client.EntityOf(harness.server.NetIdOf(gone)) == ECS::NullEntity);
    const ConnectionDiagnostics *diagnostics = harness.server.Diagnostics(harness.serverSide());
    REQUIRE(diagnostics != nullptr);
    CHECK(diagnostics->keyframeSweeps > 0);
}

TEST_CASE("a departing connection takes its provider state with it")
{
    Harness harness;
    auto *provider = new ScriptedProvider();
    harness.server.SetRelevancyProvider(std::unique_ptr<RelevancyProvider>(provider));
    harness.Step(4);

    const ClientId id = harness.server.ClientIdOf(harness.serverSide());
    REQUIRE(id.IsValid());

    harness.server.RemoveConnection(harness.serverSide());

    REQUIRE(provider->forgotten.size() == 1);
    CHECK(provider->forgotten.front() == id);
    CHECK_FALSE(harness.server.IsRelevant(harness.serverSide(), NetId{1}));
    CHECK(harness.server.RelevantSet(harness.serverSide()).empty());
}
