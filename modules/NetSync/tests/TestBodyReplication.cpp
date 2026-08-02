/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBodyReplication.cpp
/// @brief Local simulation with authoritative correction: two scenes, two Jolt
/// worlds, one loopback, and the question of whether the second world ends up
/// arranged like the first.
///
/// The model under test is the one in docs/replication-plan-v4.md — both sides
/// step the same physics at 60 Hz, and the wire's job is to periodically
/// re-anchor the client rather than stream it a movie. So the oracle is not "did
/// the bytes arrive" but "did the two piles settle the same way, and did they
/// both stop talking about it afterwards".
///
/// Determinism is not assumed anywhere here: fast-math is on, corrections are
/// applied on one side and not the other, and the two worlds add bodies on
/// different schedules. Convergence is within epsilon, and the epsilon is
/// generous on purpose.

#include <doctest/doctest.h>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Net/NetTransport.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/NetSync/Replication.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

using namespace Assisi;
using namespace Assisi::NetSync;

namespace
{

constexpr float kFixedStep = 1.f / 60.f;

/// Both halves of the protocol, each with its own scene *and its own physics
/// world* — which is the whole point. A test that shared one world would prove
/// nothing about a model whose entire premise is two simulations kept in
/// agreement by corrections.
struct PhysicsHarness
{
    Net::NetTransport     transport;
    ECS::Scene            serverScene;
    ECS::Scene            clientScene;
    Physics::PhysicsWorld serverPhysics;
    Physics::PhysicsWorld clientPhysics;

    std::pair<Net::ConnectionId, Net::ConnectionId> pair;

    ReplicationServer server;
    ReplicationClient client;

    std::uint64_t tick = 0;

    /// Set to emulate a *windowed* host: the render-side writeback stamps every
    /// dynamic body's Transform every frame, sleeping ones included. That used
    /// to mean a settled world never stopped costing bandwidth; body state is
    /// read from the physics world instead, so it must now be irrelevant.
    bool runRenderWriteback = false;

    explicit PhysicsHarness(ReplicationConfig config = {})
        : pair(transport.CreateLoopbackPair()), server(transport, serverScene, &serverPhysics, config),
          client(transport, clientScene, pair.second, &clientPhysics)
    {
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

        serverPhysics.Update(kFixedStep);
        serverPhysics.CaptureState();
        if (runRenderWriteback)
            serverPhysics.InterpolateTransforms(serverScene, 1.f);

        clientPhysics.Update(kFixedStep);
        clientPhysics.CaptureState();
        // Immediately after the client's own step, before anything reads it.
        client.EnforceSleep();

        server.Tick(tick++);
    }

    void Step(std::uint32_t times)
    {
        for (std::uint32_t i = 0; i < times; ++i)
            Step();
    }
};

/// A box, in a scene and in its world. @p replicated false is a level's own
/// static geometry — present on both machines because both loaded the file, and
/// never on the wire.
ECS::Entity SpawnBox(ECS::Scene &scene, Physics::PhysicsWorld &physics, glm::vec3 position, bool isStatic,
                     glm::vec3 halfExtents = {0.5f, 0.5f, 0.5f}, bool replicated = true)
{
    const ECS::Entity entity = scene.Create();

    ECS::Transform transform;
    transform.position = position;
    (void)scene.Add<ECS::Transform>(entity, transform);

    Physics::RigidBodyDescriptor descriptor;
    descriptor.shape       = Physics::ColliderShape::Box;
    descriptor.halfExtents = halfExtents;
    descriptor.isStatic    = isStatic;
    (void)scene.Add<Physics::RigidBodyDescriptor>(entity, descriptor);

    if (replicated)
        (void)scene.Add<Replicated>(entity, Replicated{});

    (void)physics.AddBodyFromDescriptor(scene, entity, transform, descriptor);
    return entity;
}

/// The floor, on both machines, exactly as a level file would put it there.
void SpawnSharedFloor(PhysicsHarness &harness)
{
    SpawnBox(harness.serverScene, harness.serverPhysics, {0.f, -1.f, 0.f}, /*isStatic=*/true, {20.f, 1.f, 20.f},
             /*replicated=*/false);
    SpawnBox(harness.clientScene, harness.clientPhysics, {0.f, -1.f, 0.f}, /*isStatic=*/true, {20.f, 1.f, 20.f},
             /*replicated=*/false);
}

/// The mirror of @p entity, or NullEntity.
ECS::Entity MirrorOf(PhysicsHarness &harness, ECS::Entity entity)
{
    return harness.client.EntityOf(harness.server.NetIdOf(entity));
}

/// Distance between a body's authoritative pose and its mirror's.
float PoseError(PhysicsHarness &harness, ECS::Entity entity)
{
    const ECS::Entity mirror = MirrorOf(harness, entity);
    if (mirror == ECS::NullEntity)
        return std::numeric_limits<float>::infinity();

    const Physics::RigidBody *authoritative = harness.serverScene.Get<Physics::RigidBody>(entity);
    const Physics::RigidBody *replica       = harness.clientScene.Get<Physics::RigidBody>(mirror);
    if (authoritative == nullptr || replica == nullptr)
        return std::numeric_limits<float>::infinity();

    const auto [truthPosition, truthRotation]   = harness.serverPhysics.GetBodyTransform(*authoritative);
    const auto [mirrorPosition, mirrorRotation] = harness.clientPhysics.GetBodyTransform(*replica);
    (void)truthRotation;
    (void)mirrorRotation;
    return glm::length(truthPosition - mirrorPosition);
}

} // namespace

TEST_CASE("a pile settles on the server, and the client's own bodies settle to the same arrangement")
{
    PhysicsHarness harness;
    // A windowed host, so the render writeback stamps every Transform every
    // frame — the condition that used to make a settled world replicate forever.
    harness.runRenderWriteback = true;
    harness.Step(4);
    SpawnSharedFloor(harness);

    std::vector<ECS::Entity> pile;
    for (int i = 0; i < 5; ++i)
        pile.push_back(SpawnBox(harness.serverScene, harness.serverPhysics,
                                {static_cast<float>(i) * 0.05f, 1.5f + static_cast<float>(i) * 1.2f, 0.f},
                                /*isStatic=*/false));

    // Long enough for a five-box pile to fall, bounce, and be put to sleep.
    harness.Step(600);

    for (const ECS::Entity entity : pile)
    {
        CAPTURE(entity.index);
        const ECS::Entity mirror = MirrorOf(harness, entity);
        REQUIRE(mirror != ECS::NullEntity);

        // The client built a real dynamic body from the replicated descriptor —
        // not a kinematic ghost, and not an interpolated Transform.
        const Physics::RigidBody *replica = harness.clientScene.Get<Physics::RigidBody>(mirror);
        REQUIRE(replica != nullptr);

        CHECK(PoseError(harness, entity) < 0.05f);

        // ...and both sides agree it has stopped. The sleep bit is replicated
        // state, so this is the client obeying a verdict rather than reaching
        // the same conclusion independently.
        const Physics::RigidBody *authoritative = harness.serverScene.Get<Physics::RigidBody>(entity);
        REQUIRE(authoritative != nullptr);
        CHECK_FALSE(harness.serverPhysics.IsBodyActive(*authoritative));
        CHECK_FALSE(harness.clientPhysics.IsBodyActive(*replica));
    }

    CHECK(harness.client.SnapshotsRejected() == 0);
}

TEST_CASE("a settled world stops costing bandwidth, with physics running")
{
    // The sentence the design notes celebrated and §2 of the plan showed to be
    // false: idle snapshots carry headers only. It was false because replication
    // read the render-side Transform, which the writeback re-stamps every frame
    // for every dynamic body, sleeping ones included. Reading the physics world
    // instead is what makes it true — so the writeback runs here on purpose.
    PhysicsHarness harness;
    harness.runRenderWriteback = true;
    harness.Step(4);
    SpawnSharedFloor(harness);

    for (int i = 0; i < 5; ++i)
        SpawnBox(harness.serverScene, harness.serverPhysics,
                 {static_cast<float>(i) * 0.05f, 1.5f + static_cast<float>(i) * 1.2f, 0.f}, /*isStatic=*/false);

    harness.Step(600); // fall, settle, sleep

    const ConnectionDiagnostics *diagnostics = harness.server.Diagnostics(harness.serverSide());
    REQUIRE(diagnostics != nullptr);
    const std::uint64_t bytesBefore     = diagnostics->bytesSent;
    const std::uint64_t snapshotsBefore = diagnostics->snapshotsSent;

    harness.Step(120);

    const std::uint64_t idleSnapshots = diagnostics->snapshotsSent - snapshotsBefore;
    REQUIRE(idleSnapshots > 0);
    CHECK((diagnostics->bytesSent - bytesBefore) / idleSnapshots < 24);
}

TEST_CASE("a client body woken by something the server never saw is put back to sleep")
{
    // The local wake-cascade. Client poses differ from the server's by whatever
    // the last correction has not yet removed, so a settling pile can produce
    // contacts the server never had — and Jolt wakes by island, so one spurious
    // local contact wakes a mirror the server will never speak of again.
    PhysicsHarness harness;
    harness.Step(4);
    SpawnSharedFloor(harness);

    const ECS::Entity entity = SpawnBox(harness.serverScene, harness.serverPhysics, {0.f, 1.f, 0.f},
                                        /*isStatic=*/false);
    harness.Step(400);

    const ECS::Entity mirror = MirrorOf(harness, entity);
    REQUIRE(mirror != ECS::NullEntity);
    const Physics::RigidBody *replica = harness.clientScene.Get<Physics::RigidBody>(mirror);
    REQUIRE(replica != nullptr);
    REQUIRE_FALSE(harness.clientPhysics.IsBodyActive(*replica));

    const auto [restPosition, restRotation] = harness.clientPhysics.GetBodyTransform(*replica);
    (void)restRotation;

    // Shove it, locally, the way a spurious contact would.
    harness.clientPhysics.SetBodyLinearVelocity(*replica, {5.f, 5.f, 0.f});
    REQUIRE(harness.clientPhysics.IsBodyActive(*replica));

    harness.Step(1);

    CHECK_FALSE(harness.clientPhysics.IsBodyActive(*replica));
    const auto [afterPosition, afterRotation] = harness.clientPhysics.GetBodyTransform(*replica);
    (void)afterRotation;
    CHECK(glm::length(afterPosition - restPosition) < 1e-3f);
}

TEST_CASE("a mirror destroyed by client-side gameplay comes back, and says so")
{
    ReplicationConfig config;
    config.keyframeIntervalTicks = 60; // short, so the second half of this is quick
    PhysicsHarness harness(config);
    harness.Step(4);
    SpawnSharedFloor(harness);

    const ECS::Entity entity = SpawnBox(harness.serverScene, harness.serverPhysics, {0.f, 3.f, 0.f},
                                        /*isStatic=*/false);
    harness.Step(30);

    const ECS::Entity mirror = MirrorOf(harness, entity);
    REQUIRE(mirror != ECS::NullEntity);
    REQUIRE(harness.clientScene.Get<Physics::RigidBody>(mirror) != nullptr);
    REQUIRE(harness.client.MirrorsResurrected() == 0);

    // Gameplay runs over the play world, mirrors included — a kill-Z volume or a
    // timed despawner would do exactly this. The apply path has to survive it
    // rather than dereference a handle whose slot may since have been reused.
    harness.clientScene.Destroy(mirror);
    harness.clientScene.FlushDestroyed();

    harness.Step(12);

    // The entity is back at the next update, counted rather than silent. Its
    // *state* is not: the server has no idea anything happened, so its delta
    // still says "nothing changed since you acked" and the resurrected mirror is
    // an empty shell until the sweep re-anchors it. That is the client-write
    // rule of §3.5 at its most extreme, and it is why the sweep's off position
    // carries a warning.
    const ECS::Entity restored = MirrorOf(harness, entity);
    REQUIRE(restored != ECS::NullEntity);
    CHECK(restored != mirror);
    CHECK(harness.client.MirrorsResurrected() == 1);
    CHECK(harness.client.SnapshotsRejected() == 0);

    harness.Step(90); // past a sweep

    const ECS::Entity anchored = MirrorOf(harness, entity);
    REQUIRE(anchored != ECS::NullEntity);
    CHECK(harness.clientScene.Get<Physics::RigidBody>(anchored) != nullptr);
    CHECK(PoseError(harness, entity) < 0.5f);
}

TEST_CASE("an unmarked dynamic body is simulated locally and never corrected")
{
    // Cosmetic local physics: the level's own dynamics run on both machines and
    // are nobody's business but the machine they are on. The warning R7 shows
    // exists because they *will* settle differently; what this pins is that they
    // do not travel.
    PhysicsHarness harness;
    harness.Step(4);
    SpawnSharedFloor(harness);

    SpawnBox(harness.serverScene, harness.serverPhysics, {3.f, 2.f, 0.f}, /*isStatic=*/false, {0.5f, 0.5f, 0.5f},
             /*replicated=*/false);
    harness.Step(200);

    CHECK(harness.client.ReplicatedEntityCount() == 0);
}

TEST_CASE("a client joining a world that settled before it connected gets the rest poses")
{
    // The dirty-init rule. A body that fell asleep before anyone was watching
    // never produces a sleep *transition* for the client to receive, so without
    // an initial capture the mirror would be built at the level file's pose and
    // left to re-settle — which under non-determinism means a different pile.
    PhysicsHarness harness;
    SpawnSharedFloor(harness);

    std::vector<ECS::Entity> pile;
    for (int i = 0; i < 4; ++i)
        pile.push_back(SpawnBox(harness.serverScene, harness.serverPhysics,
                                {static_cast<float>(i) * 0.05f, 1.5f + static_cast<float>(i) * 1.2f, 0.f},
                                /*isStatic=*/false));

    // Let it settle with nobody connected: the server ticks, the client is not
    // yet ready, so no snapshot describes any of this.
    for (int i = 0; i < 500; ++i)
    {
        harness.serverPhysics.Update(kFixedStep);
        harness.serverPhysics.CaptureState();
        harness.server.Tick(harness.tick++);
    }
    for (const ECS::Entity entity : pile)
        REQUIRE_FALSE(harness.serverPhysics.IsBodyActive(*harness.serverScene.Get<Physics::RigidBody>(entity)));

    harness.Step(60);

    for (const ECS::Entity entity : pile)
    {
        CAPTURE(entity.index);
        const ECS::Entity mirror = MirrorOf(harness, entity);
        REQUIRE(mirror != ECS::NullEntity);
        const Physics::RigidBody *replica = harness.clientScene.Get<Physics::RigidBody>(mirror);
        REQUIRE(replica != nullptr);
        CHECK(PoseError(harness, entity) < 0.05f);
        CHECK_FALSE(harness.clientPhysics.IsBodyActive(*replica));
    }
}

TEST_CASE("bodies converge through 150 ms of latency and 5% packet loss")
{
    // The soak, with physics on both sides. Must use the *network* loopback: the
    // in-process socket pair shares buffers and bypasses the packet layer, so
    // simulated lag and loss would not apply and the test would quietly prove
    // nothing.
    Net::NetTransport     transport;
    ECS::Scene            serverScene;
    ECS::Scene            clientScene;
    Physics::PhysicsWorld serverPhysics;
    Physics::PhysicsWorld clientPhysics;

    Net::SimulatedConditions conditions;
    conditions.sendLossPercent = 5.f;
    conditions.recvLossPercent = 5.f;
    conditions.sendLagMs       = 75;
    conditions.recvLagMs       = 75;
    REQUIRE(Net::NetTransport::SetSimulatedConditions(conditions));

    const auto        pair = transport.CreateLoopbackPair(true);
    ReplicationServer server(transport, serverScene, &serverPhysics, ReplicationConfig{});
    ReplicationClient client(transport, clientScene, pair.second, &clientPhysics);
    server.AddConnection(pair.first);

    // Floor on both, as a level would put it.
    SpawnBox(serverScene, serverPhysics, {0.f, -1.f, 0.f}, true, {20.f, 1.f, 20.f}, false);
    SpawnBox(clientScene, clientPhysics, {0.f, -1.f, 0.f}, true, {20.f, 1.f, 20.f}, false);

    std::vector<ECS::Entity> pile;
    for (int i = 0; i < 6; ++i)
        pile.push_back(SpawnBox(serverScene, serverPhysics,
                                {static_cast<float>(i) * 0.05f, 1.5f + static_cast<float>(i) * 1.1f, 0.f}, false));

    // Real time has to pass for GNS's induced latency to elapse, so this sleeps
    // rather than spins.
    std::uint64_t tick = 0;
    for (int step = 0; step < 260; ++step)
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

        serverPhysics.Update(kFixedStep);
        serverPhysics.CaptureState();
        clientPhysics.Update(kFixedStep);
        clientPhysics.CaptureState();
        client.EnforceSleep();

        server.Tick(tick++);
        std::this_thread::sleep_for(std::chrono::milliseconds{16});
    }

    // Process-global in GNS; clear before asserting so a failure here does not
    // also break every test that runs afterwards.
    Net::NetTransport::SetSimulatedConditions(Net::SimulatedConditions{});

    CHECK(client.IsSynchronized());
    CHECK(client.ReplicatedEntityCount() == pile.size());
    CHECK(client.SnapshotsRejected() == 0);

    for (const ECS::Entity entity : pile)
    {
        const NetId netId = server.NetIdOf(entity);
        REQUIRE(netId != InvalidNetId);
        const ECS::Entity mirror = client.EntityOf(netId);
        REQUIRE(mirror != ECS::NullEntity);

        const Physics::RigidBody *authoritative = serverScene.Get<Physics::RigidBody>(entity);
        const Physics::RigidBody *replica       = clientScene.Get<Physics::RigidBody>(mirror);
        REQUIRE(authoritative != nullptr);
        REQUIRE(replica != nullptr);

        const auto [truth, truthRotation]   = serverPhysics.GetBodyTransform(*authoritative);
        const auto [shown, shownRotation]   = clientPhysics.GetBodyTransform(*replica);
        (void)truthRotation;
        (void)shownRotation;
        CHECK(glm::length(truth - shown) < 0.2f);
    }
}
