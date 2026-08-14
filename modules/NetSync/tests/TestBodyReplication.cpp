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

#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
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
    Net::NetTransport transport;
    ECS::Scene serverScene;
    ECS::Scene clientScene;
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
        // Neither hello goes out until each side knows its content set; these
        // tests are about bodies, so both take the empty set's hash.
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

        serverPhysics.Update(kFixedStep);
        serverPhysics.CaptureState();
        if (runRenderWriteback)
            serverPhysics.InterpolateTransforms(serverScene, 1.f);

        clientPhysics.Update(kFixedStep);
        clientPhysics.CaptureState();
        // Immediately after the client's own step, before anything reads it.
        client.EnforceSleep();

        // The client's render path, in the order a windowed one runs it: the
        // physics writeback first, then the view-side smoothing *on top of* the
        // pose it just wrote. Reversed, the offset would simply be overwritten.
        clientPhysics.InterpolateTransforms(clientScene, 1.f);
        client.SmoothView(0.0, kFixedStep);

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
    SpawnBox(harness.serverScene, harness.serverPhysics, {0.f, -1.f, 0.f}, /*isStatic=*/ true, {20.f, 1.f, 20.f},
             /*replicated=*/ false);
    SpawnBox(harness.clientScene, harness.clientPhysics, {0.f, -1.f, 0.f}, /*isStatic=*/ true, {20.f, 1.f, 20.f},
             /*replicated=*/ false);
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
    for (int32_t i = 0; i < 5; ++i)
        pile.push_back(SpawnBox(harness.serverScene, harness.serverPhysics,
                                {static_cast<float>(i) * 0.05f, 1.5f + static_cast<float>(i) * 1.2f, 0.f},
                                /*isStatic=*/ false));

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

    for (int32_t i = 0; i < 5; ++i)
        SpawnBox(harness.serverScene, harness.serverPhysics,
                 {static_cast<float>(i) * 0.05f, 1.5f + static_cast<float>(i) * 1.2f, 0.f}, /*isStatic=*/ false);

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

TEST_CASE("a quantized body record is at least 2.5x smaller than the whole-value one")
{
    // The measurement R8 exists to make, against the encoding R5 actually
    // shipped: a varint id, the at-rest bit, and then every value as a raw
    // 32-bit float — three for the position, *four* for the quaternion, and six
    // more for the velocities when awake.
    const auto wholeValueBits = [](bool asleep, std::size_t netIdBits)
                                { return netIdBits + 1 + 32 * 3 + 32 * 4 + (asleep ? 0 : 32 * 6); };

    BodyState awake;
    awake.netId           = NetId{7}; // one varint byte, same on both sides of the ratio
    awake.position        = {12.5f, -3.25f, 100.125f};
    awake.rotation        = glm::normalize(glm::quat{0.3f, 0.5f, -0.2f, 0.8f});
    awake.linearVelocity  = {-4.5f, 12.25f, 0.f};
    awake.angularVelocity = {1.5f, -0.25f, 3.f};

    Core::BitWriter awakeWriter;
    WriteBodyState(awake, awakeWriter);
    const double awakeRatio =
        static_cast<double>(wholeValueBits(false, 8)) / static_cast<double>(awakeWriter.BitsWritten());
    CAPTURE(awakeWriter.BitsWritten());
    CHECK(awakeRatio >= 2.5);

    BodyState resting = awake;
    resting.asleep    = true;
    Core::BitWriter restingWriter;
    WriteBodyState(resting, restingWriter);
    const double restingRatio =
        static_cast<double>(wholeValueBits(true, 8)) / static_cast<double>(restingWriter.BitsWritten());
    CAPTURE(restingWriter.BitsWritten());
    CHECK(restingRatio >= 2.5);
}

TEST_CASE("the correction stream shrinks with the encoding")
{
    // The record-level ratio above is exact; this is the one that matters in
    // practice, where the stream also carries framing and ids. Two runs of the
    // same falling pile, one at the shipping resolution and one at 32 bits a
    // component. (The quaternion is smallest-three in both — there is no way to
    // ask for the old four-float form any more — so this ratio is *lower* than
    // the record-level one by construction, not a contradiction of it.)
    const BodyQuantization defaults = Quantization();

    BodyQuantization wide    = defaults;
    wide.positionBits        = 32;
    wide.linearVelocityBits  = 32;
    wide.angularVelocityBits = 32;

    const auto run = [](const BodyQuantization &quantization)
                     {
                         SetQuantization(quantization);

                         PhysicsHarness harness;
                         harness.Step(4);
                         SpawnSharedFloor(harness);
                         for (int32_t i = 0; i < 6; ++i)
                             SpawnBox(harness.serverScene, harness.serverPhysics,
                                      {static_cast<float>(i) * 0.05f, 1.5f + static_cast<float>(i) * 1.15f, 0.f},
                                      /*isStatic=*/ false);

                         // While it is falling: the correction stream is the whole cost, and a
                         // settled world would measure nothing.
                         harness.Step(30);
                         const std::uint64_t before = harness.client.Corrections().bytesApplied;
                         harness.Step(150);
                         return harness.client.Corrections().bytesApplied - before;
                     };

    const std::uint64_t wideBytes      = run(wide);
    const std::uint64_t quantizedBytes = run(defaults);
    SetQuantization(defaults);

    REQUIRE(wideBytes > 0);
    REQUIRE(quantizedBytes > 0);
    CAPTURE(wideBytes);
    CAPTURE(quantizedBytes);
    CHECK(static_cast<double>(wideBytes) / static_cast<double>(quantizedBytes) >= 1.9);
}

TEST_CASE("quantized body state round-trips within a quantum")
{
    // Round-to-nearest, so error does not accumulate across corrections: each
    // re-anchor lands within half a quantum of the truth, independently of the
    // last. That is what makes precision a display-quality knob rather than a
    // correctness one.
    const BodyQuantization &q = Quantization();

    BodyState source;
    source.netId           = NetId{42};
    source.position        = {12.5f, -3.25f, 100.125f};
    source.rotation        = glm::normalize(glm::quat{0.3f, 0.5f, -0.2f, 0.8f});
    source.linearVelocity  = {-4.5f, 12.25f, 0.f};
    source.angularVelocity = {1.5f, -0.25f, 3.f};
    source.asleep          = false;

    Core::BitWriter writer;
    WriteBodyState(source, writer);

    BodyState decoded;
    Core::BitReader reader(writer.Data());
    REQUIRE(ReadBodyState(reader, decoded));

    CHECK(decoded.netId == source.netId);
    CHECK_FALSE(decoded.asleep);

    const float positionQuantum = (2.f * q.positionExtent) / static_cast<float>(1u << q.positionBits);
    CHECK(glm::length(decoded.position - source.position) < positionQuantum * 2.f);

    // Smallest-three drops the largest component and reconstructs it from the
    // unit-length constraint, so the comparison is on the rotation, not the
    // four numbers (q and -q are the same rotation).
    CHECK(std::abs(glm::dot(decoded.rotation, source.rotation)) > 0.999f);

    const float linearQuantum = (2.f * q.linearVelocityMax) / static_cast<float>(1u << q.linearVelocityBits);
    CHECK(glm::length(decoded.linearVelocity - source.linearVelocity) < linearQuantum * 2.f);

    // An asleep record carries no velocities at all, and is much smaller for it.
    BodyState resting = source;
    resting.asleep    = true;
    Core::BitWriter restingWriter;
    WriteBodyState(resting, restingWriter);
    CHECK(restingWriter.BitsWritten() < writer.BitsWritten());
}

TEST_CASE("two builds that quantize differently refuse to pair, and the summary says which field")
{
    // The failure a handshake exists to prevent: identical component tables,
    // identical framing, and every position silently decoded into the wrong
    // number because one side thought the world was twice as wide.
    const BodyQuantization defaults = Quantization();
    const std::uint64_t baseline = NetProtocolHash();

    BodyQuantization wider  = defaults;
    wider.positionExtent    = defaults.positionExtent * 2.f;
    SetQuantization(wider);
    const std::uint64_t widerHash    = NetProtocolHash();
    const std::string widerSummary = NetProtocolSummary();

    BodyQuantization coarser    = defaults;
    coarser.positionBits        = defaults.positionBits - 1;
    SetQuantization(coarser);
    const std::uint64_t coarserHash = NetProtocolHash();

    SetQuantization(defaults);

    CHECK(widerHash != baseline);
    CHECK(coarserHash != baseline);
    CHECK(widerHash != coarserHash);
    CHECK(NetProtocolHash() == baseline);

    // Diffing two summaries has to name the offending field; a 64-bit mismatch
    // never could.
    CHECK(widerSummary.find("positionExtent=512") != std::string::npos);
    CHECK(widerSummary.find("positionBits=") != std::string::npos);
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
                                        /*isStatic=*/ false);
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

TEST_CASE("a body moved while it is not simulating still reaches clients")
{
    // The editor's gizmo drag, exactly: hold the body Static for the gesture (so
    // the solver does not fight a teleport into whatever it overlaps), move it
    // each frame, then restore its authored motion type. Nothing in that
    // sequence ever makes the body *active* — so a capture that only watches the
    // active set never notices it moved, and the client keeps rendering the
    // pre-drag pose forever.
    //
    // The same hole swallows an inspector Transform edit, and any gameplay that
    // teleports a sleeping body.
    PhysicsHarness harness;
    harness.Step(4);
    SpawnSharedFloor(harness);

    const ECS::Entity entity = SpawnBox(harness.serverScene, harness.serverPhysics, {0.f, 1.f, 0.f},
                                        /*isStatic=*/ false);
    harness.Step(400);

    const Physics::RigidBody *authoritative = harness.serverScene.Get<Physics::RigidBody>(entity);
    REQUIRE(authoritative != nullptr);
    REQUIRE_FALSE(harness.serverPhysics.IsBodyActive(*authoritative)); // settled and asleep
    REQUIRE(PoseError(harness, entity) < 0.05f);

    // --- the drag ---------------------------------------------------------
    harness.serverPhysics.SetBodyMotionType(*authoritative, Physics::BodyMotion::Static);

    const auto [startPosition, startRotation] = harness.serverPhysics.GetBodyTransform(*authoritative);
    for (int32_t step = 1; step <= 20; ++step)
    {
        harness.serverPhysics.SetBodyTransform(*authoritative,
                                               startPosition + glm::vec3{0.1f * static_cast<float>(step), 0.f, 0.f},
                                               startRotation);
        harness.Step(3);
    }

    // Mid-drag the client should already be following, not waiting for the
    // gesture to end: the author is looking at both windows.
    CHECK(PoseError(harness, entity) < 0.15f);

    harness.serverPhysics.SetBodyMotionType(*authoritative, Physics::BodyMotion::Dynamic);
    harness.serverPhysics.SetBodyTransform(*authoritative, startPosition + glm::vec3{2.f, 0.f, 0.f}, startRotation);
    harness.Step(400); // let it settle again wherever it was dropped

    CHECK(PoseError(harness, entity) < 0.05f);
}

TEST_CASE("a static replicated body moved by an author reaches clients")
{
    // The harder half of the same bug: a static body is *never* active, so
    // "record it when it stops being active" fires exactly once, at its load
    // pose, and never again. Moving a replicated wall would be invisible to
    // every client for the rest of the session — and the keyframe sweep would
    // not save it either, because the sweep resends the recorded state and the
    // recorded state is the stale one.
    PhysicsHarness harness;
    harness.Step(4);
    SpawnSharedFloor(harness);

    const ECS::Entity wall = SpawnBox(harness.serverScene, harness.serverPhysics, {0.f, 0.5f, -2.f},
                                      /*isStatic=*/ true, {2.f, 0.5f, 0.25f});
    harness.Step(60);
    REQUIRE(MirrorOf(harness, wall) != ECS::NullEntity);
    REQUIRE(PoseError(harness, wall) < 0.01f);

    const Physics::RigidBody *body = harness.serverScene.Get<Physics::RigidBody>(wall);
    REQUIRE(body != nullptr);

    // Exactly what the gizmo does, in the order it does it: write the Transform
    // through GetMut — which is what stamps the change tick everything downstream
    // filters on — and then bring the body along. An author who moved only the
    // Jolt body would be writing through a path that stamps nothing, which is the
    // same engine-wide rule that governs every other consumer of change ticks.
    {
        ECS::Transform *transform = harness.serverScene.GetMut<ECS::Transform>(wall);
        REQUIRE(transform != nullptr);
        transform->position += glm::vec3{0.f, 0.f, 4.f};
        harness.serverPhysics.SetBodyTransform(*body, transform->position, transform->rotation);
    }

    harness.Step(60);

    // The visual: a static body's pose is authored data, so it travels as an
    // ordinary Transform delta rather than as body state.
    const ECS::Entity mirror = MirrorOf(harness, wall);
    REQUIRE(mirror != ECS::NullEntity);
    CHECK(glm::distance(harness.clientScene.Get<ECS::Transform>(mirror)->position,
                        harness.serverScene.Get<ECS::Transform>(wall)->position) < 0.01f);

    // ...and the collider went with it. This is the half that is invisible until
    // something falls through the wall it can see.
    CHECK(PoseError(harness, wall) < 0.01f);
}

TEST_CASE("a mirror destroyed by client-side gameplay comes back, and says so")
{
    ReplicationConfig config;
    config.keyframeIntervalTicks = 60; // short, so the second half of this is quick
    PhysicsHarness harness(config);
    harness.Step(4);
    SpawnSharedFloor(harness);

    const ECS::Entity entity = SpawnBox(harness.serverScene, harness.serverPhysics, {0.f, 3.f, 0.f},
                                        /*isStatic=*/ false);
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

    SpawnBox(harness.serverScene, harness.serverPhysics, {3.f, 2.f, 0.f}, /*isStatic=*/ false, {0.5f, 0.5f, 0.5f},
             /*replicated=*/ false);
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
    for (int32_t i = 0; i < 4; ++i)
        pile.push_back(SpawnBox(harness.serverScene, harness.serverPhysics,
                                {static_cast<float>(i) * 0.05f, 1.5f + static_cast<float>(i) * 1.2f, 0.f},
                                /*isStatic=*/ false));

    // Let it settle with nobody connected: the server ticks, the client is not
    // yet ready, so no snapshot describes any of this.
    for (int32_t i = 0; i < 500; ++i)
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
    Net::NetTransport transport;
    ECS::Scene serverScene;
    ECS::Scene clientScene;
    Physics::PhysicsWorld serverPhysics;
    Physics::PhysicsWorld clientPhysics;

    Net::SimulatedConditions conditions;
    conditions.sendLossPercent = 5.f;
    conditions.recvLossPercent = 5.f;
    conditions.sendLagMs       = 75;
    conditions.recvLagMs       = 75;
    REQUIRE(Net::NetTransport::SetSimulatedConditions(conditions));

    const auto pair = transport.CreateLoopbackPair(true);
    ReplicationServer server(transport, serverScene, &serverPhysics, ReplicationConfig{});
    ReplicationClient client(transport, clientScene, pair.second, &clientPhysics);
    server.SetContentSetHash(0);
    client.SetContentSetHash(0);
    server.AddConnection(pair.first);

    // Floor on both, as a level would put it.
    SpawnBox(serverScene, serverPhysics, {0.f, -1.f, 0.f}, true, {20.f, 1.f, 20.f}, false);
    SpawnBox(clientScene, clientPhysics, {0.f, -1.f, 0.f}, true, {20.f, 1.f, 20.f}, false);

    std::vector<ECS::Entity> pile;
    for (int32_t i = 0; i < 6; ++i)
        pile.push_back(SpawnBox(serverScene, serverPhysics,
                                {static_cast<float>(i) * 0.05f, 1.5f + static_cast<float>(i) * 1.1f, 0.f}, false));

    // Real time has to pass for GNS's induced latency to elapse, so this sleeps
    // rather than spins.
    std::uint64_t tick = 0;
    for (int32_t step = 0; step < 260; ++step)
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

TEST_CASE("a correction moves the simulation at once and the picture gradually")
{
    // The two jobs the design refuses to conflate. The *simulation* is snapped
    // hard, because extrapolation has to proceed from a valid physics state; the
    // *view* absorbs the whole jump into an offset and decays it, because a body
    // teleporting on screen reads as a bug. Both, at the same instant, from one
    // correction.
    PhysicsHarness harness;
    harness.Step(4);
    SpawnSharedFloor(harness);

    const ECS::Entity entity = SpawnBox(harness.serverScene, harness.serverPhysics, {0.f, 1.f, 0.f},
                                        /*isStatic=*/ false);
    harness.Step(400);

    const ECS::Entity mirror = MirrorOf(harness, entity);
    REQUIRE(mirror != ECS::NullEntity);
    const Physics::RigidBody *replica = harness.clientScene.Get<Physics::RigidBody>(mirror);
    REQUIRE(replica != nullptr);
    REQUIRE_FALSE(harness.clientPhysics.IsBodyActive(*replica));

    const auto [restPosition, restRotation] = harness.clientPhysics.GetBodyTransform(*replica);

    // Client-side damage: shove the mirror a metre off and leave it asleep, so
    // sleep enforcement has nothing to notice and the delta path has nothing to
    // say. Exactly what the editor's "corrupt selected mirror" poke does.
    const glm::vec3 displaced = restPosition + glm::vec3{0.f, 0.f, 1.f};
    harness.clientPhysics.ApplyBodyState(*replica, displaced, restRotation, glm::vec3{0.f}, glm::vec3{0.f},
                                         /*activate=*/ false);
    harness.clientPhysics.InterpolateTransforms(harness.clientScene, 1.f);
    harness.client.SmoothView(0.0, kFixedStep);
    REQUIRE(glm::length(harness.clientScene.Get<ECS::Transform>(mirror)->position - displaced) < 1e-3f);

    // Ask for a re-anchor rather than waiting out the sweep.
    const std::uint64_t correctionsBefore = harness.client.Corrections().applied;
    harness.client.RequestKeyframe();
    for (int32_t i = 0; i < 30 && harness.client.Corrections().applied == correctionsBefore; ++i)
        harness.Step();
    REQUIRE(harness.client.Corrections().applied > correctionsBefore);

    // The simulation is back where the server said, immediately...
    const auto [correctedPosition, correctedRotation] = harness.clientPhysics.GetBodyTransform(*replica);
    (void)correctedRotation;
    CHECK(glm::length(correctedPosition - restPosition) < 1e-3f);

    // ...and the picture did not follow it there. What continuity means with a
    // bounded convergence window is not "the picture is frozen" but "the picture
    // moved one frame's share of the window", which for a 1 m error over 0.1 s
    // at 60 Hz is about 17 cm — versus the whole metre a correction without
    // smoothing would have jumped.
    const glm::vec3 renderedAtCorrection = harness.clientScene.Get<ECS::Transform>(mirror)->position;
    const float movedInOneFrame      = glm::length(renderedAtCorrection - displaced);
    const float onFrameShare         = 1.f * (kFixedStep / Smoothing().positionCorrectionTime);
    CAPTURE(movedInOneFrame);
    CHECK(movedInOneFrame < onFrameShare * 1.5f);
    CHECK(movedInOneFrame < 0.35f); // nowhere near the metre it would have popped

    // Over the following frames it closes the gap rather than jumping it.
    harness.Step(120);
    CHECK(glm::length(harness.clientScene.Get<ECS::Transform>(mirror)->position - restPosition) < 0.02f);

    // And the divergence it found is the number the correction cadence has to be
    // justified by, so it is measured rather than assumed.
    CHECK(harness.client.Corrections().divergenceMax > 0.9f);
}

TEST_CASE("a gameplay rule only the server runs makes its mirror trail; replicating the rule fixes it")
{
    // `Test.alvl`'s bouncing cube, and the bug it exposed. `Bounce` was left
    // unreplicated on the reasoning that "a client-side bounce is a local guess
    // at what the server's bounce also did" — true, but only if the client *has*
    // one. Under local simulation the client builds a body and steps it, and a
    // mirror missing the component simply does not bounce: it falls, rests, and
    // every correction hauls it back up.
    //
    // The corrections keep the *simulation* right, which is why it looked fine
    // in isolation. What they cannot fix is that the error arrives again every
    // interval, so the visual offset hiding it never decays to zero — the body
    // renders steadily behind its own authoritative position. That is the
    // "simulation is perfect, just severely behind" this reproduces.
    //
    // Modelled by applying the rule on one side or both. An impulse is essential:
    // a *constant* push proves nothing, because the client extrapolates a
    // constant velocity correctly and never diverges.
    struct Result
    {
        float meanDivergence = 0.f;
        float worstLag       = 0.f;
        /// The number that actually characterises the complaint. A *peak* offset
        /// right after a correction is honest — it is the size of the jump being
        /// hidden — but a high *mean* is the body sitting behind its own
        /// simulation frame after frame, which is what someone watching two
        /// windows sees and calls lag.
        float meanLag = 0.f;
    };

    const auto run = [](bool clientRunsTheRule)
                     {
                         PhysicsHarness harness;
                         harness.Step(4);
                         SpawnSharedFloor(harness);

                         const ECS::Entity entity = SpawnBox(harness.serverScene, harness.serverPhysics, {0.f, 1.f, 0.f},
                                                             /*isStatic=*/ false);
                         harness.Step(60);

                         const ECS::Entity mirror        = MirrorOf(harness, entity);
                         const Physics::RigidBody *authoritative = harness.serverScene.Get<Physics::RigidBody>(entity);
                         const Physics::RigidBody *replica       = harness.clientScene.Get<Physics::RigidBody>(mirror);
                         REQUIRE(authoritative != nullptr);
                         REQUIRE(replica != nullptr);

                         Result result;
                         double lagSum     = 0.0;
                         int32_t lagSamples = 0;
                         for (int32_t step = 0; step < 240; ++step)
                         {
                             const auto bounce = [](Physics::PhysicsWorld &world, const Physics::RigidBody &body)
                                                 {
                                                     const auto [pose, rotation] = world.GetBodyTransform(body);
                                                     (void)rotation;
                                                     if (pose.y < 0.7f)
                                                         world.SetBodyLinearVelocity(body, {0.f, 7.f, 0.f});
                                                 };

                             bounce(harness.serverPhysics, *authoritative);
                             if (clientRunsTheRule)
                                 bounce(harness.clientPhysics, *replica);

                             harness.Step();

                             // The rendered pose against the client's own simulated one: their
                             // difference *is* the visual offset, since the writeback wrote the
                             // physics pose and the smoothing then added the offset on top of it.
                             const auto [simulated, simulatedRotation] = harness.clientPhysics.GetBodyTransform(*replica);
                             (void)simulatedRotation;
                             const glm::vec3 rendered = harness.clientScene.Get<ECS::Transform>(mirror)->position;
                             const float lag      = glm::length(rendered - simulated);
                             result.worstLag          = std::max(result.worstLag, lag);
                             lagSum += static_cast<double>(lag);
                             ++lagSamples;
                         }

                         result.meanLag        = static_cast<float>(lagSum / static_cast<double>(lagSamples));
                         result.meanDivergence = harness.client.Corrections().divergenceMean();
                         return result;
                     };

    const Result serverOnly = run(/*clientRunsTheRule=*/ false);
    const Result bothSides  = run(/*clientRunsTheRule=*/ true);

    CAPTURE(serverOnly.meanDivergence);
    CAPTURE(serverOnly.worstLag);
    CAPTURE(serverOnly.meanLag);
    CAPTURE(bothSides.meanDivergence);
    CAPTURE(bothSides.worstLag);
    CAPTURE(bothSides.meanLag);

    // Running the rule on both sides is what replicating the component buys, and
    // it is a clear improvement on both halves — but deliberately *not* asserted
    // as a collapse, because it is not one. A threshold (or contact) rule fires
    // at a slightly different instant on each side, since the client's body sits
    // wherever the last correction left it, and from there the trajectories
    // separate again. This is exactly the amplification §3.1 warns about, and it
    // is why the design refuses to lean on same-binary determinism.
    CHECK(bothSides.meanDivergence < serverOnly.meanDivergence * 0.8f);
    CHECK(bothSides.worstLag < serverOnly.worstLag * 0.8f);

    // And the part the smoothing owns rather than the replication: with the
    // offset converging over a fixed window (0.1 s by default) rather than at a
    // per-frame rate, the *typical* frame is close to the simulation and the
    // peaks are the corrections being smoothed, not a body parked behind.
    //
    // Stated against the window rather than as an absolute distance, because it
    // is the window that bounds it: a body moving at speed v is at most ~v·T
    // behind while a correction is being paid off. Under the pre-fix per-frame
    // decay this was ~7x the per-correction error and stayed there indefinitely;
    // the assertion is deliberately loose because the exact figure depends on
    // how fast the body happens to be moving.
    CHECK(bothSides.meanLag < bothSides.worstLag * 0.6f);
}

TEST_CASE("a gameplay component whose absence would desync a mirror is replicated")
{
    // The marking, pinned so it cannot quietly regress. Bounce rewrites a body's
    // velocity on contact: a client that does not have it runs a different
    // simulation, which is the case above.
    const Core::Reflect::ComponentMeta *meta = Core::Reflect::ComponentRegistry::Instance().ById(
        Core::Reflect::ComponentRegistry::Instance().IdOf(typeid(Physics::Bounce)));
    REQUIRE(meta != nullptr);
    CHECK(meta->replicable);
}

TEST_CASE("a correction past the snap bound is admitted rather than smoothed")
{
    // Smoothing a teleport reads worse than admitting it: a body sliding half a
    // room to catch up looks like a bug, where a teleport looks like a teleport.
    PhysicsHarness harness;
    harness.Step(4);
    SpawnSharedFloor(harness);

    const ECS::Entity entity = SpawnBox(harness.serverScene, harness.serverPhysics, {0.f, 1.f, 0.f},
                                        /*isStatic=*/ false);
    harness.Step(400);

    const ECS::Entity mirror  = MirrorOf(harness, entity);
    const Physics::RigidBody *replica = harness.clientScene.Get<Physics::RigidBody>(mirror);
    REQUIRE(replica != nullptr);
    const auto [restPosition, restRotation] = harness.clientPhysics.GetBodyTransform(*replica);

    // Well past the 2.5 m bound.
    harness.clientPhysics.ApplyBodyState(*replica, restPosition + glm::vec3{0.f, 0.f, 8.f}, restRotation,
                                         glm::vec3{0.f}, glm::vec3{0.f}, /*activate=*/ false);

    const std::uint64_t correctionsBefore = harness.client.Corrections().applied;
    harness.client.RequestKeyframe();
    for (int32_t i = 0; i < 30 && harness.client.Corrections().applied == correctionsBefore; ++i)
        harness.Step();
    REQUIRE(harness.client.Corrections().applied > correctionsBefore);

    // No decay period: the rendered pose is already the corrected one.
    CHECK(glm::length(harness.clientScene.Get<ECS::Transform>(mirror)->position - restPosition) < 0.01f);
}

// ── Per-entity policy and the body channel (P2b) ─────────────────────────────
//
// Excluding RigidBodyDescriptor means "replicate this as a visual, don't
// simulate it on clients": no body state, no client-side Jolt body, and the
// Transform travels normally so the mirror interpolates. It is a useful thing to
// be able to author — debris the server simulates but clients only watch — and
// it needs the whole body pipeline to agree, or the mirror ends up with its
// Transform suppressed *and* no corrections, frozen at its load pose.

namespace
{

std::size_t OrdinalOfType(const std::type_info &type)
{
    const Core::Reflect::ComponentRegistry &registry = Core::Reflect::ComponentRegistry::Instance();
    return registry.ReplicableOrdinalOf(registry.IdOf(std::type_index(type)));
}

void ExcludeDescriptor(ECS::Scene &scene, ECS::Entity entity)
{
    Replicated *marker = scene.GetMut<Replicated>(entity);
    REQUIRE(marker != nullptr);
    marker->excluded.Set(OrdinalOfType(typeid(Physics::RigidBodyDescriptor)));
}

} // namespace

TEST_CASE("a descriptor-excluded entity becomes a visual-only mirror")
{
    PhysicsHarness harness;
    harness.runRenderWriteback = true; // a windowed host, so the Transform tracks the sim
    SpawnSharedFloor(harness);

    const ECS::Entity falling = SpawnBox(harness.serverScene, harness.serverPhysics, {0.f, 6.f, 0.f},
                                         /*isStatic=*/ false);
    ExcludeDescriptor(harness.serverScene, falling);

    harness.Step(40);

    const ECS::Entity mirror = MirrorOf(harness, falling);
    REQUIRE(mirror != ECS::NullEntity);

    // No descriptor arrived, so the client never built a body — the mirror is
    // rendered by interpolation, not corrected by the solver.
    CHECK(harness.clientScene.Get<Physics::RigidBodyDescriptor>(mirror) == nullptr);
    CHECK(harness.clientScene.Get<Physics::RigidBody>(mirror) == nullptr);

    // ...and its Transform is *not* suppressed, which is the half that would be
    // easy to get wrong: the body channel normally owns motion for a bodied
    // entity, so without the policy check the mirror would receive neither.
    const ECS::Transform *mirrored = harness.clientScene.Get<ECS::Transform>(mirror);
    REQUIRE(mirrored != nullptr);
    CHECK(mirrored->position.y < 5.f); // it visibly fell rather than sitting at its spawn pose
}

TEST_CASE("a descriptor-excluded entity costs no body-state bytes")
{
    PhysicsHarness harness;
    SpawnSharedFloor(harness);

    const ECS::Entity falling = SpawnBox(harness.serverScene, harness.serverPhysics, {0.f, 6.f, 0.f},
                                         /*isStatic=*/ false);
    ExcludeDescriptor(harness.serverScene, falling);

    harness.Step(60);
    // The correction channel is silent for it: nothing was ever applied, because
    // nothing was ever sent.
    CHECK(harness.client.Corrections().applied == 0);
}

TEST_CASE("a resting visual-only mirror stops costing bandwidth")
{
    // The cost model D6 has to honour. On a windowed host the writeback used to
    // stamp every dynamic body's Transform every frame, sleeping ones included —
    // so a visual-only mirror, which has no body channel and travels by Transform
    // delta, would have resent its pose forever. Suppressing the no-op write at
    // the source fixes it for this case and removes a false dirty for every
    // resting body engine-wide.
    PhysicsHarness harness;
    harness.runRenderWriteback = true;
    SpawnSharedFloor(harness);

    const ECS::Entity crate = SpawnBox(harness.serverScene, harness.serverPhysics, {0.f, 2.f, 0.f},
                                       /*isStatic=*/ false);
    ExcludeDescriptor(harness.serverScene, crate);

    harness.Step(240); // fall, land, settle, sleep

    const ConnectionDiagnostics *diagnostics = harness.server.Diagnostics(harness.serverSide());
    REQUIRE(diagnostics != nullptr);
    const std::uint64_t settled = diagnostics->bytesSent;

    harness.Step(120);
    const std::uint64_t idle = diagnostics->bytesSent - settled;

    // Snapshot headers still go out every tick; what must not is a Transform
    // block per snapshot for a body that has not moved in two seconds.
    //
    // The threshold sits between two measured values rather than being guessed:
    // 440 bytes with the writeback's no-op suppression (empty-snapshot framing,
    // ~11 bytes per snapshot) against 2080 without it (the same framing plus a
    // Transform block every time). Anything under a kilobyte here means the
    // resting body is contributing nothing.
    CAPTURE(idle);
    CHECK(idle < 1000);
}

TEST_CASE("excluding a descriptor mid-session tears the mirror's body down")
{
    // The invisible-obstacle bug class: without the teardown the Jolt body
    // outlives the authority that justified it and keeps colliding, and the
    // stale transient RigidBody blocks any future rebuild.
    PhysicsHarness harness;
    SpawnSharedFloor(harness);

    const ECS::Entity crate = SpawnBox(harness.serverScene, harness.serverPhysics, {0.f, 3.f, 0.f},
                                       /*isStatic=*/ false);
    harness.Step(30);

    const ECS::Entity mirror = MirrorOf(harness, crate);
    REQUIRE(mirror != ECS::NullEntity);
    REQUIRE(harness.clientScene.Get<Physics::RigidBody>(mirror) != nullptr);

    ExcludeDescriptor(harness.serverScene, crate);
    harness.Step(30);

    CHECK(harness.clientScene.Get<Physics::RigidBodyDescriptor>(mirror) == nullptr);
    // Both halves: the component is gone *and* the body behind it, or the world
    // keeps an obstacle nobody can see.
    CHECK(harness.clientScene.Get<Physics::RigidBody>(mirror) == nullptr);
    CHECK(harness.clientScene.Get<ECS::Transform>(mirror) != nullptr);
}

TEST_CASE("re-including a descriptor rebuilds the body at the authoritative pose")
{
    // Rides D11: policy moved, the descriptor did not, so its change tick still
    // predates the baseline and only the force-send delivers it.
    ReplicationConfig config;
    config.keyframeIntervalTicks = 0; // no sweep to rescue it
    PhysicsHarness harness(config);
    SpawnSharedFloor(harness);

    const ECS::Entity crate = SpawnBox(harness.serverScene, harness.serverPhysics, {0.f, 3.f, 0.f},
                                       /*isStatic=*/ false);
    ExcludeDescriptor(harness.serverScene, crate);
    harness.Step(40);

    const ECS::Entity mirror = MirrorOf(harness, crate);
    REQUIRE(mirror != ECS::NullEntity);
    REQUIRE(harness.clientScene.Get<Physics::RigidBody>(mirror) == nullptr);

    Replicated *marker = harness.serverScene.GetMut<Replicated>(crate);
    REQUIRE(marker != nullptr);
    marker->excluded = Core::Reflect::ComponentMask{};

    harness.Step(40);
    REQUIRE(harness.clientScene.Get<Physics::RigidBodyDescriptor>(mirror) != nullptr);
    REQUIRE(harness.clientScene.Get<Physics::RigidBody>(mirror) != nullptr);
    // Built from the correction stream, so it starts where the server says rather
    // than re-settling from the level pose.
    CHECK(PoseError(harness, crate) < 0.2f);
}

TEST_CASE("a stale body record cannot outlive the exclusion that ended it")
{
    // CaptureBodyStates erases on exclusion. Without that the record keeps its
    // nonzero tick, which beats every post-sweep empty baseline — so the stale
    // state would be resent to every client after every keyframe sweep for the
    // rest of the session.
    ReplicationConfig config;
    config.keyframeIntervalTicks = 20; // sweep often, so a leak shows quickly
    PhysicsHarness harness(config);
    SpawnSharedFloor(harness);

    const ECS::Entity crate = SpawnBox(harness.serverScene, harness.serverPhysics, {0.f, 3.f, 0.f},
                                       /*isStatic=*/ false);
    harness.Step(40);
    ExcludeDescriptor(harness.serverScene, crate);
    harness.Step(60); // several sweeps

    const std::uint64_t appliedBefore = harness.client.Corrections().applied;
    harness.Step(120);                // several more
    CHECK(harness.client.Corrections().applied == appliedBefore);
}

TEST_CASE("a bodied entity that withholds its Transform sends no body state either")
{
    // D9. Both client-side body builders require a Transform, so one can never be
    // built — and body states it must drop on arrival are pure waste. The editor
    // warns about this shape; the server simply does not spend bandwidth on it.
    PhysicsHarness harness;
    SpawnSharedFloor(harness);

    const ECS::Entity crate = SpawnBox(harness.serverScene, harness.serverPhysics, {0.f, 4.f, 0.f},
                                       /*isStatic=*/ false);
    {
        Replicated *marker = harness.serverScene.GetMut<Replicated>(crate);
        REQUIRE(marker != nullptr);
        marker->excluded.Set(OrdinalOfType(typeid(ECS::Transform)));
    }

    harness.Step(60);

    const ECS::Entity mirror = MirrorOf(harness, crate);
    REQUIRE(mirror != ECS::NullEntity);
    CHECK(harness.clientScene.Get<ECS::Transform>(mirror) == nullptr);
    CHECK(harness.client.Corrections().applied == 0);
}
