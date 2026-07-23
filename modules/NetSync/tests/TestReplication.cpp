/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestReplication.cpp
/// @brief End-to-end replication over a real transport: two scenes, a loopback
/// connection, and the question of whether the second one ends up looking like
/// the first.
///
/// These are integration tests on purpose. The interesting failures in a delta
/// protocol — a baseline that advances when it shouldn't, a spawn that is only
/// sent once and then lost, a despawn that never arrives — are all invisible to
/// a unit test of either half alone.
///
/// The convergence oracle is *within epsilon and by component*, never
/// byte-exact: the engine is deliberately non-deterministic (fast-math, FMA),
/// and a test that demanded bit equality would be testing the wrong thing.

#include <doctest/doctest.h>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Net/NetTransport.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/NetSync/Replication.hpp>
#include <Assisi/NetSync/TestNetComponents.hpp>

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using namespace Assisi;
using namespace Assisi::NetSync;

namespace
{

/// A server and a client wired to each other through GNS's in-process socket
/// pair — the same object the listen server will use, so this harness exercises
/// the shipping path rather than a test-only one.
struct Harness
{
    Net::NetTransport transport;
    ECS::Scene        serverScene;
    ECS::Scene        clientScene;

    /// Both ends of the in-process pair. Created in the member init list
    /// because ReplicationClient takes its connection handle at construction —
    /// which is the right shape for the real thing (a client without a
    /// connection has nothing to do) and only awkward here.
    std::pair<Net::ConnectionId, Net::ConnectionId> pair;

    ReplicationServer server;
    ReplicationClient client;

    std::uint64_t tick = 0;

    explicit Harness(ReplicationConfig config = {})
        : pair(transport.CreateLoopbackPair()), server(transport, serverScene, config),
          client(transport, clientScene, pair.second)
    {
        server.AddConnection(pair.first);
    }

    [[nodiscard]] Net::ConnectionId serverSide() const { return pair.first; }
    [[nodiscard]] Net::ConnectionId clientSide() const { return pair.second; }

    /// One full network step: deliver everything in flight, then advance the
    /// server a tick. Messages are routed by which end they arrived on.
    void Step()
    {
        std::vector<Net::NetEvent> events;
        transport.Poll(events);
        for (const Net::NetEvent &event : events)
        {
            if (event.type != Net::NetEvent::Type::Message)
                continue;
            if (event.connection == serverSide())
                server.HandleMessage(serverSide(), event.payload);
            else if (event.connection == clientSide())
                client.HandleMessage(event.payload);
        }

        server.Tick(tick++);
    }

    void Step(std::uint32_t times)
    {
        for (std::uint32_t i = 0; i < times; ++i)
            Step();
    }
};

ECS::Entity SpawnReplicated(ECS::Scene &scene, glm::vec3 position)
{
    const ECS::Entity entity = scene.Create();
    ECS::Transform    transform;
    transform.position = position;
    (void)scene.Add<ECS::Transform>(entity, transform);
    (void)scene.Add<Replicated>(entity, Replicated{});
    return entity;
}

/// Convergence oracle: same replicated entity count, and every mirrored
/// transform within epsilon of the authoritative one.
bool Converged(const Harness &harness, float epsilon = 1e-4f)
{
    std::size_t serverCount = 0;
    for (auto [entity, replicated] : const_cast<ECS::Scene &>(harness.serverScene).Query<Replicated>())
    {
        (void)replicated;
        (void)entity;
        ++serverCount;
    }
    if (serverCount != harness.client.ReplicatedEntityCount())
        return false;

    for (auto [entity, replicated] : const_cast<ECS::Scene &>(harness.serverScene).Query<Replicated>())
    {
        (void)replicated;
        const NetId netId = harness.server.NetIdOf(entity);
        if (netId == InvalidNetId)
            return false;

        const ECS::Entity mirror = harness.client.EntityOf(netId);
        if (mirror == ECS::NullEntity)
            return false;

        const ECS::Transform *authoritative = const_cast<ECS::Scene &>(harness.serverScene).Get<ECS::Transform>(entity);
        const ECS::Transform *replica       = const_cast<ECS::Scene &>(harness.clientScene).Get<ECS::Transform>(mirror);
        if (authoritative == nullptr || replica == nullptr)
            return false;

        const glm::vec3 delta = authoritative->position - replica->position;
        if (std::abs(delta.x) > epsilon || std::abs(delta.y) > epsilon || std::abs(delta.z) > epsilon)
            return false;
    }
    return true;
}

} // namespace

TEST_CASE("a client handshakes and receives the initial world")
{
    Harness harness;
    harness.Step(4); // handshake

    CHECK(harness.client.IsSynchronized());
    CHECK(harness.server.IsReady(harness.serverSide()));
    CHECK(harness.client.RejectMessage().empty());
    CHECK(harness.client.ServerTickRateHz() == harness.server.Config().tickRateHz);

    SpawnReplicated(harness.serverScene, {1.f, 2.f, 3.f});
    SpawnReplicated(harness.serverScene, {-4.f, 0.f, 5.f});

    harness.Step(12);

    CHECK(harness.client.ReplicatedEntityCount() == 2);
    CHECK(Converged(harness));
    CHECK(harness.client.SnapshotsRejected() == 0);
}

TEST_CASE("only entities marked Replicated cross the wire")
{
    Harness harness;
    harness.Step(4);

    SpawnReplicated(harness.serverScene, {1.f, 0.f, 0.f});

    // Scenery: has a transform, but no marker. Both sides already have it from
    // the level file, and restating it every snapshot would be pure waste.
    const ECS::Entity scenery = harness.serverScene.Create();
    (void)harness.serverScene.Add<ECS::Transform>(scenery, ECS::Transform{});

    harness.Step(12);

    CHECK(harness.client.ReplicatedEntityCount() == 1);
}

TEST_CASE("a moved entity converges, and an unmoved one stops costing bandwidth")
{
    Harness harness;
    harness.Step(4);

    const ECS::Entity entity = SpawnReplicated(harness.serverScene, {0.f, 0.f, 0.f});
    harness.Step(12);
    REQUIRE(Converged(harness));

    // Move it authoritatively. GetMut is what stamps the change tick, which is
    // what the delta is computed from.
    for (int i = 1; i <= 5; ++i)
    {
        ECS::Transform *transform = harness.serverScene.GetMut<ECS::Transform>(entity);
        REQUIRE(transform != nullptr);
        transform->position.x = static_cast<float>(i);
        harness.Step(6);
    }
    CHECK(Converged(harness));

    // Now leave it alone. Snapshots keep flowing (headers and acks), but with
    // nothing changed since the acked baseline they should carry no component
    // data — the whole point of delta replication.
    const ConnectionDiagnostics *before = harness.server.Diagnostics(harness.serverSide());
    REQUIRE(before != nullptr);
    const std::uint64_t bytesBefore     = before->bytesSent;
    const std::uint64_t snapshotsBefore = before->snapshotsSent;

    harness.Step(30);

    const ConnectionDiagnostics *after = harness.server.Diagnostics(harness.serverSide());
    REQUIRE(after != nullptr);
    const std::uint64_t idleSnapshots = after->snapshotsSent - snapshotsBefore;
    REQUIRE(idleSnapshots > 0);

    // A few bytes of header and framing per snapshot is expected; a transform's
    // worth of payload is not.
    const std::uint64_t bytesPerIdleSnapshot = (after->bytesSent - bytesBefore) / idleSnapshots;
    CHECK(bytesPerIdleSnapshot < 24);
    CHECK(Converged(harness));
}

TEST_CASE("a destroyed entity is despawned on the client")
{
    Harness harness;
    harness.Step(4);

    const ECS::Entity keep = SpawnReplicated(harness.serverScene, {1.f, 0.f, 0.f});
    const ECS::Entity drop = SpawnReplicated(harness.serverScene, {2.f, 0.f, 0.f});
    (void)keep;
    harness.Step(12);
    REQUIRE(harness.client.ReplicatedEntityCount() == 2);

    const NetId droppedId = harness.server.NetIdOf(drop);
    harness.serverScene.Destroy(drop);
    harness.serverScene.FlushDestroyed();

    harness.Step(12);

    CHECK(harness.client.ReplicatedEntityCount() == 1);
    CHECK(harness.client.EntityOf(droppedId) == ECS::NullEntity);
    CHECK(Converged(harness));

    // The NetId is retired, not recycled: a new entity must not inherit the
    // identity of the one that just died.
    const ECS::Entity fresh = SpawnReplicated(harness.serverScene, {3.f, 0.f, 0.f});
    harness.Step(6);
    CHECK(harness.server.NetIdOf(fresh) != droppedId);
}

TEST_CASE("dropping the Replicated marker despawns the entity without destroying it")
{
    Harness harness;
    harness.Step(4);

    const ECS::Entity entity = SpawnReplicated(harness.serverScene, {5.f, 0.f, 0.f});
    harness.Step(12);
    REQUIRE(harness.client.ReplicatedEntityCount() == 1);

    harness.serverScene.Remove<Replicated>(entity);
    harness.Step(12);

    CHECK(harness.client.ReplicatedEntityCount() == 0);
    // Still very much alive on the server — it just stopped being anyone
    // else's business.
    CHECK(harness.serverScene.IsAlive(entity));
}

TEST_CASE("a late-joining client converges on a world already in motion")
{
    Harness harness;
    harness.Step(4);

    for (int i = 0; i < 8; ++i)
        SpawnReplicated(harness.serverScene, {static_cast<float>(i), 0.f, 0.f});
    harness.Step(12);
    REQUIRE(Converged(harness));

    // A second client shows up long after the world was built. Its baseline is
    // the empty one, which is the same code path as any other delta — that
    // unification is the reason late join needs no special message.
    ECS::Scene       lateScene;
    const auto       latePair = harness.transport.CreateLoopbackPair();
    ReplicationClient lateClient(harness.transport, lateScene, latePair.second);
    harness.server.AddConnection(latePair.first);

    for (std::uint32_t i = 0; i < 20; ++i)
    {
        std::vector<Net::NetEvent> events;
        harness.transport.Poll(events);
        for (const Net::NetEvent &event : events)
        {
            if (event.type != Net::NetEvent::Type::Message)
                continue;
            if (event.connection == harness.serverSide() || event.connection == latePair.first)
                harness.server.HandleMessage(event.connection, event.payload);
            else if (event.connection == harness.clientSide())
                harness.client.HandleMessage(event.payload);
            else if (event.connection == latePair.second)
                lateClient.HandleMessage(event.payload);
        }
        harness.server.Tick(harness.tick++);
    }

    CHECK(lateClient.IsSynchronized());
    CHECK(lateClient.ReplicatedEntityCount() == 8);
    CHECK(lateClient.SnapshotsRejected() == 0);
    // The original client is unaffected by the newcomer.
    CHECK(Converged(harness));
}

TEST_CASE("a world too big for one packet still converges, over several snapshots")
{
    // Force the budget low enough that the initial world cannot fit in one
    // snapshot. An entity dropped for space must come back as a spawn next
    // time — never be counted as delivered.
    ReplicationConfig config;
    config.maxSnapshotBytes = 120;
    Harness harness(config);
    harness.Step(4);

    for (int i = 0; i < 40; ++i)
        SpawnReplicated(harness.serverScene, {static_cast<float>(i), 1.f, 2.f});

    harness.Step(120);

    CHECK(harness.client.ReplicatedEntityCount() == 40);
    CHECK(Converged(harness));
    CHECK(harness.client.SnapshotsRejected() == 0);
}

TEST_CASE("input flows the other way and is bounded on arrival")
{
    Harness harness;
    harness.Step(4);
    REQUIRE(harness.client.IsSynchronized());

    InputCommandBuffer buffer;
    InputCommand       command;
    command.tick  = harness.tick + 4;
    command.moveX = 5.f; // well past any legal stick deflection
    command.moveY = 5.f;
    buffer.Push(command);
    harness.client.SendInput(buffer);

    harness.Step(4);

    const ConnectionDiagnostics *diagnostics = harness.server.Diagnostics(harness.serverSide());
    REQUIRE(diagnostics != nullptr);
    CHECK(diagnostics->commandsClamped == 1);
    CHECK(diagnostics->inputPacketsDropped == 0);

    // Run the tick it targets and check the simulation gets a bounded command.
    while (harness.tick <= command.tick)
        harness.Step();
    const InputCommand *applied = harness.server.ConsumeInput(harness.serverSide(), command.tick);
    if (applied != nullptr)
    {
        const float magnitude = std::sqrt(applied->moveX * applied->moveX + applied->moveY * applied->moveY);
        CHECK(magnitude <= doctest::Approx(1.f));
    }
}

TEST_CASE("the snapshot rate is clamped to a divisor of the tick rate")
{
    ReplicationConfig config;
    config.tickRateHz = 60;
    config.snapshotHz = 25; // 60/25 is not an integer

    Harness harness(config);
    // 60/25 truncates to a divisor of 2, i.e. 30 Hz — every snapshot then lands
    // on an exact tick instead of the interval alternating between 2 and 3.
    CHECK(harness.server.Config().snapshotHz == 30);
    CHECK(harness.server.IsSnapshotTick(0));
    CHECK(harness.server.IsSnapshotTick(2));
    CHECK_FALSE(harness.server.IsSnapshotTick(3));
}

TEST_CASE("a component removed on the server is removed on the client")
{
    Harness harness;
    harness.Step(4);

    const ECS::Entity entity = SpawnReplicated(harness.serverScene, {1.f, 0.f, 0.f});
    (void)harness.serverScene.Add<Test::Health>(entity, Test::Health{42});
    harness.Step(12);

    const NetId       netId  = harness.server.NetIdOf(entity);
    const ECS::Entity mirror = harness.client.EntityOf(netId);
    REQUIRE(mirror != ECS::NullEntity);
    REQUIRE(harness.clientScene.Get<Test::Health>(mirror) != nullptr);
    CHECK(harness.clientScene.Get<Test::Health>(mirror)->value == 42);

    // Change detection has nothing to say about this: removal stamps no tick,
    // so the server finds it by diffing the acked component set — the same way
    // it finds despawns, one level down.
    harness.serverScene.Remove<Test::Health>(entity);
    harness.Step(12);

    CHECK(harness.clientScene.Get<Test::Health>(mirror) == nullptr);
    // The entity itself is untouched; only the component went away.
    CHECK(harness.clientScene.IsAlive(mirror));
    CHECK(harness.clientScene.Get<ECS::Transform>(mirror) != nullptr);
    CHECK(harness.client.SnapshotsRejected() == 0);
}

TEST_CASE("a component removed and re-added ends up present")
{
    Harness harness;
    harness.Step(4);

    const ECS::Entity entity = SpawnReplicated(harness.serverScene, {0.f, 0.f, 0.f});
    (void)harness.serverScene.Add<Test::Health>(entity, Test::Health{7});
    harness.Step(12);

    const ECS::Entity mirror = harness.client.EntityOf(harness.server.NetIdOf(entity));
    REQUIRE(mirror != ECS::NullEntity);

    // Both happen between two snapshots, so one snapshot carries the removal
    // *and* the re-add. Removals are applied first for exactly this reason.
    harness.serverScene.Remove<Test::Health>(entity);
    (void)harness.serverScene.Add<Test::Health>(entity, Test::Health{99});
    harness.Step(12);

    REQUIRE(harness.clientScene.Get<Test::Health>(mirror) != nullptr);
    CHECK(harness.clientScene.Get<Test::Health>(mirror)->value == 99);
}

TEST_CASE("removals are not re-sent once acknowledged")
{
    Harness harness;
    harness.Step(4);

    const ECS::Entity entity = SpawnReplicated(harness.serverScene, {0.f, 0.f, 0.f});
    (void)harness.serverScene.Add<Test::Health>(entity, Test::Health{1});
    harness.Step(12);
    harness.serverScene.Remove<Test::Health>(entity);
    harness.Step(12);

    // Once the client has acked a snapshot without the component, the component
    // is no longer in its baseline, so there is nothing left to diff against —
    // an idle snapshot must go back to costing almost nothing.
    const ConnectionDiagnostics *before = harness.server.Diagnostics(harness.serverSide());
    REQUIRE(before != nullptr);
    const std::uint64_t bytesBefore     = before->bytesSent;
    const std::uint64_t snapshotsBefore = before->snapshotsSent;

    harness.Step(30);

    const ConnectionDiagnostics *after = harness.server.Diagnostics(harness.serverSide());
    REQUIRE(after != nullptr);
    const std::uint64_t idleSnapshots = after->snapshotsSent - snapshotsBefore;
    REQUIRE(idleSnapshots > 0);
    CHECK((after->bytesSent - bytesBefore) / idleSnapshots < 24);
}

TEST_CASE("the client is told when its initial world is complete")
{
    // A budget small enough that the world takes several snapshots to arrive.
    ReplicationConfig config;
    config.maxSnapshotBytes = 120;
    Harness harness(config);
    harness.Step(4);

    for (int i = 0; i < 40; ++i)
        SpawnReplicated(harness.serverScene, {static_cast<float>(i), 0.f, 0.f});

    // Mid-download: synchronized, but the world is demonstrably not all here.
    harness.Step(6);
    CHECK(harness.client.IsSynchronized());
    CHECK(harness.client.IsJoining());
    CHECK_FALSE(harness.client.IsWorldComplete());
    CHECK(harness.client.ReplicatedEntityCount() < 40);

    harness.Step(150);

    CHECK(harness.client.IsWorldComplete());
    CHECK_FALSE(harness.client.IsJoining());
    CHECK(harness.client.ReplicatedEntityCount() == 40);
}

TEST_CASE("the world converges through 150 ms of latency and 5% packet loss")
{
    // The soak case. This one must use the *network* loopback: the default
    // in-process socket pair shares buffers and bypasses the packet layer
    // entirely, so simulated lag and loss would not apply to it and the test
    // would quietly prove nothing.
    // The transport comes first: these are global GNS config values, and there
    // is nothing to configure until the library has been initialized by at
    // least one live NetTransport.
    Net::NetTransport transport;
    ECS::Scene        serverScene;
    ECS::Scene        clientScene;

    Net::SimulatedConditions conditions;
    conditions.sendLossPercent = 5.f;
    conditions.recvLossPercent = 5.f;
    conditions.sendLagMs       = 75;
    conditions.recvLagMs       = 75;
    REQUIRE(Net::NetTransport::SetSimulatedConditions(conditions));

    const auto        pair = transport.CreateLoopbackPair(true);
    ReplicationServer server(transport, serverScene, ReplicationConfig{});
    ReplicationClient client(transport, clientScene, pair.second);
    server.AddConnection(pair.first);

    std::vector<ECS::Entity> entities;
    for (int i = 0; i < 16; ++i)
        entities.push_back(SpawnReplicated(serverScene, {static_cast<float>(i), 0.f, 0.f}));

    // Real time has to pass for GNS's induced latency to elapse, so this loop
    // sleeps rather than spinning. ~4 s of wall clock at 60 Hz.
    std::uint64_t tick = 0;
    for (int step = 0; step < 240; ++step)
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

        // Keep the world moving: a static world would converge trivially, and
        // it is the *delta* path under loss that this is here to exercise.
        for (std::size_t i = 0; i < entities.size(); ++i)
        {
            ECS::Transform *transform = serverScene.GetMut<ECS::Transform>(entities[i]);
            transform->position.y     = static_cast<float>(step) * 0.01f + static_cast<float>(i);
        }

        server.Tick(tick++);
        std::this_thread::sleep_for(std::chrono::milliseconds{16});
    }

    // Let the last snapshots drain without further mutation, so the comparison
    // is against a settled world rather than one still in flight.
    for (int step = 0; step < 60; ++step)
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
        server.Tick(tick++);
        std::this_thread::sleep_for(std::chrono::milliseconds{16});
    }

    // These are process-global in GNS; clear them before asserting so a failure
    // here does not also break every test that runs afterwards.
    Net::NetTransport::SetSimulatedConditions(Net::SimulatedConditions{});

    CHECK(client.IsSynchronized());
    CHECK(client.ReplicatedEntityCount() == entities.size());
    CHECK(client.SnapshotsRejected() == 0);

    // Within epsilon, never byte-exact — the engine is deliberately
    // non-deterministic, so equality is the wrong question to ask.
    for (const ECS::Entity entity : entities)
    {
        const NetId netId = server.NetIdOf(entity);
        REQUIRE(netId != InvalidNetId);
        const ECS::Entity mirror = client.EntityOf(netId);
        REQUIRE(mirror != ECS::NullEntity);

        const ECS::Transform *authoritative = serverScene.Get<ECS::Transform>(entity);
        const ECS::Transform *replica       = clientScene.Get<ECS::Transform>(mirror);
        REQUIRE(authoritative != nullptr);
        REQUIRE(replica != nullptr);
        CHECK(replica->position.x == doctest::Approx(authoritative->position.x).epsilon(1e-4));
        CHECK(replica->position.y == doctest::Approx(authoritative->position.y).epsilon(1e-4));
    }
}

TEST_CASE("Reset drops the mirrored world, which is how v1 reconnects")
{
    Harness harness;
    harness.Step(4);

    SpawnReplicated(harness.serverScene, {1.f, 1.f, 1.f});
    SpawnReplicated(harness.serverScene, {2.f, 2.f, 2.f});
    harness.Step(12);
    REQUIRE(harness.client.ReplicatedEntityCount() == 2);

    harness.client.Reset();
    harness.clientScene.FlushDestroyed();

    CHECK(harness.client.ReplicatedEntityCount() == 0);
    CHECK_FALSE(harness.client.IsSynchronized());
    CHECK(harness.client.LastAppliedTick() == 0);
}
