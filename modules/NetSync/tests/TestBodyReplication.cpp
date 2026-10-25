/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBodyReplication.cpp
/// @brief Local simulation with authoritative correction: two scenes, two Jolt
/// worlds, one loopback, and the question of whether the second world ends up
/// arranged like the first.
///
/// The model under test is local simulation with authoritative correction — both
/// sides step the same physics at 60 Hz, and the wire's job is to periodically
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
#include <Assisi/ECS/RenderOffset.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Net/NetTransport.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>
#include <Assisi/NetSync/ReplicationConfig.hpp>
#include <Assisi/NetSync/ReplicationProviders.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>
#include <Assisi/NetSync/TestNetComponents.hpp>
#include <Assisi/NetSync/TestTransport.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
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
    std::unique_ptr<Net::NetTransport> transport = Assisi::NetSync::Test::MakeTransport();
    ECS::Scene serverScene;
    ECS::Scene clientScene;
    Physics::PhysicsWorld serverPhysics{serverScene, Assisi::Physics::NoCollisionAssets()};
    Physics::PhysicsWorld clientPhysics{clientScene, Assisi::Physics::NoCollisionAssets()};

    std::pair<Net::ConnectionId, Net::ConnectionId> pair;

    ReplicationServer server;
    ReplicationClient client;

    std::uint64_t tick = 0;

    explicit PhysicsHarness(ReplicationConfig config = {})
        : pair(transport->CreateLoopbackPair()), server(*transport, serverScene, &serverPhysics, config),
        client(*transport, clientScene, pair.second, &clientPhysics)
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
        transport->Poll(events);
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
        clientPhysics.Update(kFixedStep);
        // Immediately after the client's own step, before anything reads it.
        client.EnforceSleep();

        // The client's view-side smoothing, as a windowed host runs it each frame.
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

    Physics::Collider collider;
    collider.shape       = Physics::ColliderShape::Box;
    collider.halfExtents = halfExtents;
    (void)scene.Add<Physics::Collider>(entity, collider);
    if (!isStatic)
        (void)scene.Add<Physics::RigidBody>(entity, Physics::RigidBody{});

    if (replicated)
        (void)scene.Add<Replicated>(entity, Replicated{});

    // Built now rather than on the next step, so a test can ask about the body
    // straight away.
    physics.Reconcile();
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

    if (!harness.serverPhysics.HasBody(entity) || !harness.clientPhysics.HasBody(mirror))
        return std::numeric_limits<float>::infinity();

    return glm::length(harness.serverPhysics.GetBodyPose(entity).position -
                       harness.clientPhysics.GetBodyPose(mirror).position);
}

/// Where @p entity is drawn: its Transform, which is the simulation pose, plus
/// the view offset smoothing lays over it. Unparented, so the two simply add.
glm::vec3 RenderedPosition(ECS::Scene &scene, ECS::Entity entity)
{
    const glm::vec3 position = scene.Get<ECS::Transform>(entity)->position;
    const ECS::RenderOffset *offset = scene.Get<ECS::RenderOffset>(entity);
    return offset != nullptr ? position + offset->position : position;
}

} // namespace

TEST_CASE("a pile settles on the server, and the client's own bodies settle to the same arrangement")
{
    PhysicsHarness harness;
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

        // The client built a real dynamic body from the replicated Collider and RigidBody —
        // not a kinematic ghost, and not an interpolated Transform.
        REQUIRE(harness.clientPhysics.HasBody(mirror));

        CHECK(PoseError(harness, entity) < 0.05f);

        // ...and both sides agree it has stopped. The sleep bit is replicated
        // state, so this is the client obeying a verdict rather than reaching
        // the same conclusion independently.
        REQUIRE(harness.serverPhysics.HasBody(entity));
        CHECK_FALSE(harness.serverPhysics.IsBodyActive(entity));
        CHECK_FALSE(harness.clientPhysics.IsBodyActive(mirror));
    }

    CHECK(harness.client.SnapshotsRejected() == 0);
}

TEST_CASE("a settled world stops costing bandwidth, with physics running")
{
    // Idle snapshots carry headers only: a sleeping body is no longer written
    // back, so nothing about it changes.
    PhysicsHarness harness;
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
    // Measured against a whole-value encoding: a varint id, the at-rest bit, and
    // then every value as a raw 32-bit float — three for the position, *four*
    // for the quaternion, and six more for the velocities when awake.
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
    // component. (The quaternion is smallest-three in both, so this ratio is
    // *lower* than the record-level one by construction, not a contradiction
    // of it.)
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

TEST_CASE("a character's body record carries its stance and what it stands on, and a body's carries neither")
{
    BodyState crouched;
    crouched.netId = NetId{9};
    crouched.position = {1.f, 2.f, 3.f};
    crouched.character = true;
    crouched.stance = Physics::Stance::Crouching;
    crouched.ground = Physics::GroundState::OnSteepGround;

    Core::BitWriter writer;
    WriteBodyState(crouched, writer);
    BodyState decoded;
    Core::BitReader reader(writer.Data());
    REQUIRE(ReadBodyState(reader, decoded));
    CHECK(decoded.character);
    CHECK(decoded.stance == Physics::Stance::Crouching);
    CHECK(decoded.ground == Physics::GroundState::OnSteepGround);

    BodyState crate = crouched;
    crate.character = false;
    Core::BitWriter crateWriter;
    WriteBodyState(crate, crateWriter);
    BodyState crateDecoded;
    Core::BitReader crateReader(crateWriter.Data());
    REQUIRE(ReadBodyState(crateReader, crateDecoded));
    CHECK_FALSE(crateDecoded.character);
    CHECK(crateWriter.BitsWritten() < writer.BitsWritten());
}

TEST_CASE("a character on the host reaches the client's confirmed copy with its stance and ground")
{
    PhysicsHarness harness;
    harness.Step(4);
    SpawnSharedFloor(harness);
    const ECS::Entity character = harness.serverScene.Create();
    (void)harness.serverScene.Add<ECS::Transform>(character, ECS::Transform{});
    (void)harness.serverScene.Add<Physics::Character>(character, Physics::Character{});
    (void)harness.serverScene.Add<Replicated>(character, Replicated{});
    harness.server.SetControl(character, harness.server.ClientIdOf(harness.serverSide()));
    harness.Step(10);
    harness.serverScene.GetMut<Physics::CharacterIntent>(character)->stance = Physics::Stance::Crouching;
    harness.Step(30);

    const ReplicationClient::Confirmed *confirmed = harness.client.ConfirmedOf(harness.server.NetIdOf(character));
    REQUIRE(confirmed != nullptr);
    REQUIRE(confirmed->physicsTick != 0);
    CHECK(confirmed->body.character);
    CHECK(confirmed->body.stance == Physics::Stance::Crouching);
    CHECK(confirmed->body.ground == Physics::GroundState::OnGround);
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
    REQUIRE(harness.clientPhysics.HasBody(mirror));
    REQUIRE_FALSE(harness.clientPhysics.IsBodyActive(mirror));

    const glm::vec3 restPosition = harness.clientPhysics.GetBodyPose(mirror).position;

    // Shove it, locally, the way a spurious contact would.
    harness.clientScene.GetMut<Physics::BodyState>(mirror)->linearVelocity = {5.f, 5.f, 0.f};
    harness.clientPhysics.Reconcile();
    REQUIRE(harness.clientPhysics.IsBodyActive(mirror));

    harness.Step(1);

    CHECK_FALSE(harness.clientPhysics.IsBodyActive(mirror));
    const glm::vec3 afterPosition = harness.clientPhysics.GetBodyPose(mirror).position;
    CHECK(glm::length(afterPosition - restPosition) < 1e-3f);
}

TEST_CASE("a sleeping body dragged by its Transform reaches clients")
{
    // The editor's gizmo drag, exactly: the Transform is written each frame and
    // the physics world moves the body to it. The body was asleep when the drag
    // began, so a capture that only watched the active set from before the drag
    // would never notice it moved, and the client would keep rendering the
    // pre-drag pose forever.
    PhysicsHarness harness;
    harness.Step(4);
    SpawnSharedFloor(harness);

    const ECS::Entity entity = SpawnBox(harness.serverScene, harness.serverPhysics, {0.f, 1.f, 0.f},
                                        /*isStatic=*/ false);
    harness.Step(400);

    REQUIRE(harness.serverPhysics.HasBody(entity));
    REQUIRE_FALSE(harness.serverPhysics.IsBodyActive(entity)); // settled and asleep
    REQUIRE(PoseError(harness, entity) < 0.05f);

    // --- the drag ---------------------------------------------------------
    const glm::vec3 startPosition = harness.serverScene.Get<ECS::Transform>(entity)->position;
    for (int32_t step = 1; step <= 20; ++step)
    {
        harness.serverScene.GetMut<ECS::Transform>(entity)->position =
            startPosition + glm::vec3{0.1f * static_cast<float>(step), 0.f, 0.f};
        harness.Step(3);
    }

    // Mid-drag the client should already be following, not waiting for the
    // gesture to end: the author is looking at both windows.
    CHECK(PoseError(harness, entity) < 0.15f);

    harness.serverScene.GetMut<ECS::Transform>(entity)->position = startPosition + glm::vec3{2.f, 0.f, 0.f};
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

    REQUIRE(harness.serverPhysics.HasBody(wall));

    // Exactly what the gizmo does: write the Transform through GetMut, which is
    // what stamps the change tick everything downstream filters on — the physics
    // world, which moves the collider, among them.
    {
        ECS::Transform *transform = harness.serverScene.GetMut<ECS::Transform>(wall);
        REQUIRE(transform != nullptr);
        transform->position += glm::vec3{0.f, 0.f, 4.f};
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
    REQUIRE(harness.clientPhysics.HasBody(mirror));
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
    // an empty shell until the sweep re-anchors it — the client-write rule at
    // its most extreme, and why disabling the sweep carries a warning.
    const ECS::Entity restored = MirrorOf(harness, entity);
    REQUIRE(restored != ECS::NullEntity);
    CHECK(restored != mirror);
    CHECK(harness.client.MirrorsResurrected() == 1);
    CHECK(harness.client.SnapshotsRejected() == 0);

    harness.Step(90); // past a sweep

    const ECS::Entity anchored = MirrorOf(harness, entity);
    REQUIRE(anchored != ECS::NullEntity);
    CHECK(harness.clientPhysics.HasBody(anchored));
    CHECK(PoseError(harness, entity) < 0.5f);
}

TEST_CASE("an unmarked dynamic body is simulated locally and never corrected")
{
    // Cosmetic local physics: the level's own dynamics run on both machines and
    // are nobody's business but the machine they are on. They *will* settle
    // differently; what this pins is that they do not travel.
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
        harness.server.Tick(harness.tick++);
    }
    for (const ECS::Entity entity : pile)
        REQUIRE_FALSE(harness.serverPhysics.IsBodyActive(entity));

    harness.Step(60);

    for (const ECS::Entity entity : pile)
    {
        CAPTURE(entity.index);
        const ECS::Entity mirror = MirrorOf(harness, entity);
        REQUIRE(mirror != ECS::NullEntity);
        REQUIRE(harness.clientPhysics.HasBody(mirror));
        CHECK(PoseError(harness, entity) < 0.05f);
        CHECK_FALSE(harness.clientPhysics.IsBodyActive(mirror));
    }
}

TEST_CASE("bodies converge through 150 ms of latency and 5% packet loss")
{
    // The soak, with physics on both sides. Must use the *network* loopback: the
    // in-process socket pair shares buffers and bypasses the packet layer, so
    // simulated lag and loss would not apply and the test would quietly prove
    // nothing.
    std::unique_ptr<Net::NetTransport> transport = Assisi::NetSync::Test::MakeTransport();
    ECS::Scene serverScene;
    ECS::Scene clientScene;
    Physics::PhysicsWorld serverPhysics{serverScene, Assisi::Physics::NoCollisionAssets()};
    Physics::PhysicsWorld clientPhysics{clientScene, Assisi::Physics::NoCollisionAssets()};

    Net::SimulatedConditions conditions;
    conditions.sendLossPercent = 5.f;
    conditions.recvLossPercent = 5.f;
    conditions.sendLagMs       = 75;
    conditions.recvLagMs       = 75;
    REQUIRE(Net::NetTransport::SetSimulatedConditions(conditions));

    const auto pair = transport->CreateLoopbackPair(true);
    ReplicationServer server(*transport, serverScene, &serverPhysics, ReplicationConfig{});
    ReplicationClient client(*transport, clientScene, pair.second, &clientPhysics);
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
        transport->Poll(events);
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
        clientPhysics.Update(kFixedStep);
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

        REQUIRE(serverPhysics.HasBody(entity));
        REQUIRE(clientPhysics.HasBody(mirror));

        const glm::vec3 truth = serverPhysics.GetBodyPose(entity).position;
        const glm::vec3 shown = clientPhysics.GetBodyPose(mirror).position;
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
    REQUIRE(harness.clientPhysics.HasBody(mirror));
    REQUIRE_FALSE(harness.clientPhysics.IsBodyActive(mirror));

    const Physics::Pose rest = harness.clientPhysics.GetBodyPose(mirror);
    const glm::vec3 restPosition = rest.position;

    // Client-side damage: shove the mirror a metre off and leave it asleep, so
    // sleep enforcement has nothing to notice and the delta path has nothing to
    // say. Exactly what the editor's "corrupt selected mirror" poke does.
    const glm::vec3 displaced = restPosition + glm::vec3{0.f, 0.f, 1.f};
    harness.clientPhysics.ApplyCorrection(mirror, Physics::Pose{rest.rotation, displaced},
                                          Physics::BodyState{.asleep = true});
    harness.client.SmoothView(0.0, kFixedStep);
    REQUIRE(glm::length(harness.clientScene.Get<ECS::Transform>(mirror)->position - displaced) < 1e-3f);

    // Ask for a re-anchor rather than waiting out the sweep.
    const std::uint64_t correctionsBefore = harness.client.Corrections().applied;
    harness.client.RequestKeyframe();
    for (int32_t i = 0; i < 30 && harness.client.Corrections().applied == correctionsBefore; ++i)
        harness.Step();
    REQUIRE(harness.client.Corrections().applied > correctionsBefore);

    // The simulation is back where the server said, immediately...
    const glm::vec3 correctedPosition = harness.clientPhysics.GetBodyPose(mirror).position;
    CHECK(glm::length(correctedPosition - restPosition) < 1e-3f);

    // ...and the picture did not follow it there. What continuity means with a
    // bounded convergence window is not "the picture is frozen" but "the picture
    // moved one frame's share of the window", which for a 1 m error over 0.1 s
    // at 60 Hz is about 17 cm — versus the whole metre a correction without
    // smoothing would have jumped.
    const glm::vec3 renderedAtCorrection = RenderedPosition(harness.clientScene, mirror);
    const float movedInOneFrame      = glm::length(renderedAtCorrection - displaced);
    const float onFrameShare         = 1.f * (kFixedStep / Smoothing().positionCorrectionTime);
    CAPTURE(movedInOneFrame);
    CHECK(movedInOneFrame < onFrameShare * 1.5f);
    CHECK(movedInOneFrame < 0.35f); // nowhere near the metre it would have popped

    // Over the following frames it closes the gap rather than jumping it.
    harness.Step(120);
    CHECK(glm::length(RenderedPosition(harness.clientScene, mirror) - restPosition) < 0.02f);

    // And the divergence it found is the number the correction cadence has to be
    // justified by, so it is measured rather than assumed.
    CHECK(harness.client.Corrections().divergenceMax > 0.9f);
}

TEST_CASE("a gameplay rule only the server runs makes its mirror trail; replicating the rule fixes it")
{
    // Under local simulation the client builds a body and steps it, so a mirror
    // missing a gameplay component simply does not run that rule: an unbounced
    // cube falls, rests, and every correction hauls it back up.
    //
    // Corrections keep the *simulation* right, which is why this looks fine in
    // isolation. What they cannot fix is that the error arrives again every
    // interval, so the visual offset hiding it never decays to zero and the body
    // renders steadily behind its own authoritative position.
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
                         REQUIRE(harness.serverPhysics.HasBody(entity));
                         REQUIRE(harness.clientPhysics.HasBody(mirror));

                         Result result;
                         double lagSum     = 0.0;
                         int32_t lagSamples = 0;
                         for (int32_t step = 0; step < 240; ++step)
                         {
                             const auto bounce = [](ECS::Scene &scene, Physics::PhysicsWorld &world, ECS::Entity body)
                                                 {
                                                     if (world.GetBodyPose(body).position.y < 0.7f)
                                                         scene.GetMut<Physics::BodyState>(body)->linearVelocity =
                                                             glm::vec3{0.f, 7.f, 0.f};
                                                 };

                             bounce(harness.serverScene, harness.serverPhysics, entity);
                             if (clientRunsTheRule)
                                 bounce(harness.clientScene, harness.clientPhysics, mirror);

                             harness.Step();

                             // The rendered pose against the client's own simulated one: their
                             // difference *is* the visual offset.
                             const glm::vec3 simulated = harness.clientPhysics.GetBodyPose(mirror).position;
                             const glm::vec3 rendered = RenderedPosition(harness.clientScene, mirror);
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
    // separate again — the amplification that is why the design refuses to lean
    // on same-binary determinism.
    CHECK(bothSides.meanDivergence < serverOnly.meanDivergence * 0.8f);
    CHECK(bothSides.worstLag < serverOnly.worstLag * 0.8f);

    // And the part the smoothing owns rather than the replication: with the
    // offset converging over a fixed window (0.1 s by default) rather than at a
    // per-frame rate, the *typical* frame is close to the simulation and the
    // peaks are the corrections being smoothed, not a body parked behind.
    //
    // Stated against the window rather than as an absolute distance, because it
    // is the window that bounds it: a body moving at speed v is at most ~v·T
    // behind while a correction is being paid off. The assertion is deliberately
    // loose because the exact figure depends on how fast the body happens to be
    // moving.
    CHECK(bothSides.meanLag < bothSides.worstLag * 0.6f);
}

TEST_CASE("a gameplay component whose absence would desync a mirror is replicated")
{
    // The marking, pinned so it cannot quietly regress. A Collider decides how a
    // body bounces and slides: a client that does not have it runs a different
    // simulation, which is the case above.
    const Core::Reflect::ComponentMeta *meta = Core::Reflect::ComponentRegistry::Instance().ById(
        Core::Reflect::ComponentRegistry::Instance().IdOf(typeid(Physics::Collider)));
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
    REQUIRE(harness.clientPhysics.HasBody(mirror));
    const Physics::Pose rest = harness.clientPhysics.GetBodyPose(mirror);
    const glm::vec3 restPosition = rest.position;

    // Well past the 2.5 m bound.
    harness.clientPhysics.ApplyCorrection(mirror,
                                          Physics::Pose{rest.rotation, restPosition + glm::vec3{0.f, 0.f, 8.f}},
                                          Physics::BodyState{.asleep = true});

    const std::uint64_t correctionsBefore = harness.client.Corrections().applied;
    harness.client.RequestKeyframe();
    for (int32_t i = 0; i < 30 && harness.client.Corrections().applied == correctionsBefore; ++i)
        harness.Step();
    REQUIRE(harness.client.Corrections().applied > correctionsBefore);

    // No decay period: the rendered pose is already the corrected one.
    CHECK(glm::length(harness.clientScene.Get<ECS::Transform>(mirror)->position - restPosition) < 0.01f);
}

// ── Per-entity policy and the body channel ───────────────────────────────────
//
// Excluding Collider means "replicate this as a visual, don't
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

void ExcludeCollider(ECS::Scene &scene, ECS::Entity entity)
{
    Replicated *marker = scene.GetMut<Replicated>(entity);
    REQUIRE(marker != nullptr);
    marker->excluded.Set(OrdinalOfType(typeid(Physics::Collider)));
}

} // namespace

TEST_CASE("a collider-excluded entity becomes a visual-only mirror")
{
    PhysicsHarness harness;
    SpawnSharedFloor(harness);

    const ECS::Entity falling = SpawnBox(harness.serverScene, harness.serverPhysics, {0.f, 6.f, 0.f},
                                         /*isStatic=*/ false);
    ExcludeCollider(harness.serverScene, falling);

    harness.Step(40);

    const ECS::Entity mirror = MirrorOf(harness, falling);
    REQUIRE(mirror != ECS::NullEntity);

    // No collider arrived, so the client never built a body — the mirror is
    // rendered by interpolation, not corrected by the solver.
    CHECK(harness.clientScene.Get<Physics::Collider>(mirror) == nullptr);
    CHECK(!harness.clientPhysics.HasBody(mirror));

    // ...and its Transform is *not* suppressed, which is the half that would be
    // easy to get wrong: the body channel normally owns motion for a bodied
    // entity, so without the policy check the mirror would receive neither.
    const ECS::Transform *mirrored = harness.clientScene.Get<ECS::Transform>(mirror);
    REQUIRE(mirrored != nullptr);
    CHECK(mirrored->position.y < 5.f); // it visibly fell rather than sitting at its spawn pose
}

TEST_CASE("a collider-excluded entity costs no body-state bytes")
{
    PhysicsHarness harness;
    SpawnSharedFloor(harness);

    const ECS::Entity falling = SpawnBox(harness.serverScene, harness.serverPhysics, {0.f, 6.f, 0.f},
                                         /*isStatic=*/ false);
    ExcludeCollider(harness.serverScene, falling);

    harness.Step(60);
    // The correction channel is silent for it: nothing was ever applied, because
    // nothing was ever sent.
    CHECK(harness.client.Corrections().applied == 0);
}

TEST_CASE("a resting visual-only mirror stops costing bandwidth")
{
    // A visual-only mirror has no body channel and travels by Transform delta,
    // so it is the writeback that decides whether a resting one keeps costing
    // bandwidth: the writeback suppresses no-op writes, so a sleeping body stops
    // dirtying its Transform.
    PhysicsHarness harness;
    SpawnSharedFloor(harness);

    const ECS::Entity crate = SpawnBox(harness.serverScene, harness.serverPhysics, {0.f, 2.f, 0.f},
                                       /*isStatic=*/ false);
    ExcludeCollider(harness.serverScene, crate);

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

TEST_CASE("excluding a collider mid-session tears the mirror's body down")
{
    // The invisible-obstacle bug class: without the teardown the Jolt body
    // outlives the authority that justified it and keeps colliding.
    PhysicsHarness harness;
    SpawnSharedFloor(harness);

    const ECS::Entity crate = SpawnBox(harness.serverScene, harness.serverPhysics, {0.f, 3.f, 0.f},
                                       /*isStatic=*/ false);
    harness.Step(30);

    const ECS::Entity mirror = MirrorOf(harness, crate);
    REQUIRE(mirror != ECS::NullEntity);
    REQUIRE(harness.clientPhysics.HasBody(mirror));

    ExcludeCollider(harness.serverScene, crate);
    harness.Step(30);

    CHECK(harness.clientScene.Get<Physics::Collider>(mirror) == nullptr);
    // Both halves: the component is gone *and* the body behind it, or the world
    // keeps an obstacle nobody can see.
    CHECK(!harness.clientPhysics.HasBody(mirror));
    CHECK(harness.clientScene.Get<ECS::Transform>(mirror) != nullptr);
}

TEST_CASE("re-including a collider rebuilds the body at the authoritative pose")
{
    // Policy moved, the collider did not, so its change tick still predates
    // the baseline and only the force-send delivers it.
    ReplicationConfig config;
    config.keyframeIntervalTicks = 0; // no sweep to rescue it
    PhysicsHarness harness(config);
    SpawnSharedFloor(harness);

    const ECS::Entity crate = SpawnBox(harness.serverScene, harness.serverPhysics, {0.f, 3.f, 0.f},
                                       /*isStatic=*/ false);
    ExcludeCollider(harness.serverScene, crate);
    harness.Step(40);

    const ECS::Entity mirror = MirrorOf(harness, crate);
    REQUIRE(mirror != ECS::NullEntity);
    REQUIRE(!harness.clientPhysics.HasBody(mirror));

    Replicated *marker = harness.serverScene.GetMut<Replicated>(crate);
    REQUIRE(marker != nullptr);
    marker->excluded = Core::Reflect::ComponentMask{};

    harness.Step(40);
    REQUIRE(harness.clientScene.Get<Physics::Collider>(mirror) != nullptr);
    REQUIRE(harness.clientPhysics.HasBody(mirror));
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
    ExcludeCollider(harness.serverScene, crate);
    harness.Step(60); // several sweeps

    const std::uint64_t appliedBefore = harness.client.Corrections().applied;
    harness.Step(120);                // several more
    CHECK(harness.client.Corrections().applied == appliedBefore);
}

TEST_CASE("a bodied entity that withholds its Transform sends no body state either")
{
    // The mirror's Transform comes from the collider's requirement, at its
    // defaults rather than from the wire, so body states would correct a pose the
    // client was never told. The editor warns about this shape; the server simply
    // does not spend bandwidth on it.
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
    CHECK(harness.client.Corrections().applied == 0);
}

TEST_CASE("a mirror the client predicts keeps its own pose while the host's is sent")
{
    // A player's camera, with nothing held back: the host sends where it looks,
    // a round trip after the client looked there. Written or interpolated over
    // the client's own, the view is pulled back to where it was every snapshot.
    PhysicsHarness harness;
    harness.Step(4);

    const ECS::Entity eye = harness.serverScene.Create();
    (void)harness.serverScene.Add<ECS::Transform>(eye, ECS::Transform{});
    (void)harness.serverScene.Add<Replicated>(eye, Replicated{});
    harness.server.SetControl(eye, harness.server.ClientIdOf(harness.serverSide()));
    harness.Step(10);

    const ECS::Entity mirror = MirrorOf(harness, eye);
    REQUIRE(mirror != ECS::NullEntity);
    REQUIRE(harness.client.Predicts(mirror));

    const glm::quat lookedUp = glm::angleAxis(glm::radians(30.f), glm::vec3(1.f, 0.f, 0.f));
    const glm::quat hostsLate = glm::angleAxis(glm::radians(10.f), glm::vec3(1.f, 0.f, 0.f));
    harness.clientScene.GetMut<ECS::Transform>(mirror)->rotation = lookedUp;
    harness.serverScene.GetMut<ECS::Transform>(eye)->rotation = hostsLate;
    harness.Step(10);

    CHECK(std::abs(glm::dot(harness.clientScene.Get<ECS::Transform>(mirror)->rotation, lookedUp)) ==
          doctest::Approx(1.f));
    const ECS::Transform *confirmed =
        harness.client.ConfirmedComponent<ECS::Transform>(harness.server.NetIdOf(eye));
    REQUIRE(confirmed != nullptr);
    CHECK(std::abs(glm::dot(confirmed->rotation, hostsLate)) == doctest::Approx(1.f));
}

TEST_CASE("a mirror whose Transform is not sent keeps the one the client gives it")
{
    // A player's camera: sent so it exists on every machine, its Transform kept
    // back because where it looks is each machine's own. Interpolating it would
    // write an old copy of the client's own pose back over it every frame, and
    // its view could never move.
    PhysicsHarness harness;
    harness.Step(4);

    const ECS::Entity eye = harness.serverScene.Create();
    (void)harness.serverScene.Add<ECS::Transform>(eye, ECS::Transform{});
    Replicated marker;
    marker.excluded.Set(OrdinalOfType(typeid(ECS::Transform)));
    (void)harness.serverScene.Add<Replicated>(eye, marker);
    harness.Step(10);

    const ECS::Entity mirror = MirrorOf(harness, eye);
    REQUIRE(mirror != ECS::NullEntity);
    // The client's own, as a blueprint the client expands itself gives it one.
    ECS::Transform own;
    own.rotation = glm::angleAxis(glm::radians(10.f), glm::vec3(1.f, 0.f, 0.f));
    (void)harness.clientScene.Add<ECS::Transform>(mirror, own);
    harness.Step(10);

    const glm::quat lookedUp = glm::angleAxis(glm::radians(30.f), glm::vec3(1.f, 0.f, 0.f));
    harness.clientScene.GetMut<ECS::Transform>(mirror)->rotation = lookedUp;
    harness.Step(10);

    CHECK(std::abs(glm::dot(harness.clientScene.Get<ECS::Transform>(mirror)->rotation, lookedUp)) ==
          doctest::Approx(1.f));
}

TEST_CASE("a walking character arrives on the client simulated, carrying its velocity")
{
    PhysicsHarness harness;
    harness.Step(4);
    SpawnSharedFloor(harness);

    const ECS::Entity walker = harness.serverScene.Create();
    (void)harness.serverScene.Add<ECS::Transform>(walker, ECS::Transform{});
    (void)harness.serverScene.Add<Physics::Character>(walker, Physics::Character{});
    (void)harness.serverScene.Add<Replicated>(walker, Replicated{});
    harness.Step(30);

    harness.serverScene.GetMut<Physics::CharacterIntent>(walker)->move = {1.f, 0.f, 0.f};
    harness.Step(60);

    const ECS::Entity mirror = MirrorOf(harness, walker);
    REQUIRE(mirror != ECS::NullEntity);
    CHECK(harness.clientPhysics.HasBody(mirror));
    REQUIRE(harness.serverScene.Get<Physics::CharacterState>(walker)->velocity.x > 1.f);
    CHECK(harness.clientScene.Get<Physics::CharacterState>(mirror)->velocity.x > 1.f);
    CHECK(PoseError(harness, walker) < 0.1f);
}

namespace
{

/// A character standing on the shared floor, sent to the client.
ECS::Entity SpawnStanding(PhysicsHarness &harness, glm::vec3 feet = glm::vec3{0.f})
{
    SpawnSharedFloor(harness);
    const ECS::Entity character = harness.serverScene.Create();
    (void)harness.serverScene.Add<ECS::Transform>(character, ECS::Transform{.position = feet});
    (void)harness.serverScene.Add<Physics::Character>(character, Physics::Character{});
    (void)harness.serverScene.Add<Replicated>(character, Replicated{});
    harness.Step(30);
    return character;
}

/// How far apart two facings are: 1 the same, 0 a half turn.
float Agreement(const glm::quat &a, const glm::quat &b) { return std::abs(glm::dot(a, b)); }

/// A half turn about up.
const glm::quat kTurnedRound = glm::angleAxis(glm::radians(180.f), glm::vec3(0.f, 1.f, 0.f));

/// Closer than a quantized rotation can miss by.
constexpr float kSameFacing = 0.999f;

} // namespace

TEST_CASE("a character that turns on the server turns on the client")
{
    // A character's capsule never turns; its facing is its Transform. A correction
    // that only places the body leaves every other player facing where it spawned.
    PhysicsHarness harness;
    harness.Step(4);
    const ECS::Entity character = SpawnStanding(harness);

    harness.serverScene.GetMut<ECS::Transform>(character)->rotation = kTurnedRound;
    harness.Step(30);

    const ECS::Entity mirror = MirrorOf(harness, character);
    REQUIRE(mirror != ECS::NullEntity);
    CHECK(Agreement(harness.clientScene.Get<ECS::Transform>(mirror)->rotation, kTurnedRound) > kSameFacing);
}

TEST_CASE("a character the client controls keeps the facing it gave itself")
{
    // The host's facing for it is the client's own turn arriving a round trip
    // late; taking it would pull the view back with every correction.
    PhysicsHarness harness;
    harness.Step(4);
    const ECS::Entity character = SpawnStanding(harness);
    harness.server.SetControl(character, harness.server.ClientIdOf(harness.serverSide()));
    harness.Step(10);

    const ECS::Entity mirror = MirrorOf(harness, character);
    REQUIRE(mirror != ECS::NullEntity);
    REQUIRE(harness.client.ControlsEntity(mirror));
    // A character never sleeps, so corrections keep arriving the whole time.
    harness.clientScene.GetMut<ECS::Transform>(mirror)->rotation = kTurnedRound;
    harness.Step(30);
    REQUIRE(harness.client.Corrections().applied > 0);

    CHECK(Agreement(harness.clientScene.Get<ECS::Transform>(mirror)->rotation, kTurnedRound) > kSameFacing);
}

namespace
{

/// How many ticks the client's own character runs ahead of the host's copy.
constexpr std::uint32_t kLeadTicks = 3;

/// Ticks the two copies walk in step before the case looks at them.
constexpr std::uint32_t kAgreeingTicks = 10;

/// How far the host moves its copy sideways behind the client's back (m).
constexpr float kHostShove = 0.5f;

/// How near a replayed prediction lands to where it should (m): the host's
/// velocity arrives rounded to a few centimetres a second, and the replay
/// carries that over its few steps.
constexpr float kReplayedNear = 0.01f;

/// Where another player stands in the client's character's way.
constexpr glm::vec3 kOtherPlayerAhead{1.5f, 0.f, 0.f};

/// Ticks of walking into the other player: long enough to reach it, push it
/// along and be stopped.
constexpr std::uint32_t kPushTicks = 90;

/// Further than the other player is pushed when it is pushed at all (m).
constexpr float kPushedFar = 0.5f;

/// How far another player may move and still count as not pushed: settling, not
/// a shove (m).
constexpr float kNotPushed = 0.05f;

/// How far short of touching another character a walker may stop: the padding
/// characters keep between them, and rounding (m).
constexpr float kTouchSlack = 0.1f;

/// How far ahead of the host's a client's mirror of another player may be: a
/// quantized position, not a shove (m).
constexpr float kMirrorNearHost = 0.01f;

/// The most a client's own character may move back in a tick (m): rounding,
/// not a pull.
constexpr float kMostBackPerTick = 0.001f;

/// Steps a starved controller holds its last move for, as a host holds a
/// player's controls through a short gap.
constexpr std::uint32_t kRigHoldTicks = 6;

/// The move a step runs on: @p command's when one came, otherwise @p held
/// until the gap passes kRigHoldTicks. The host and the client's replay both
/// use it, as App::RelayHeldCommand is used by both.
glm::vec3 MoveFor(const InputCommand *command, glm::vec3 held, std::uint32_t starvedTicks)
{
    if (command != nullptr)
    {
        return {command->moveX, 0.f, command->moveY};
    }
    return starvedTicks > kRigHoldTicks ? glm::vec3{0.f} : held;
}

/// What a PredictionRig stands in, besides the floor and the client's
/// character at the origin.
struct RigLevel
{
    /// Light crates on the floor, the host's and sent to the client, each
    /// resting with its centre at this x and z.
    std::vector<glm::vec3> crates = {};
    /// Another player's character: the host's to move, and on the client a
    /// mirror like any other.
    std::optional<glm::vec3> other = std::nullopt;
    /// A second character the client controls.
    std::optional<glm::vec3> secondAt = std::nullopt;
    /// A moving deck the client's characters stand on, carried by it: a level's
    /// own kinematic platform, on both machines and never sent, moved by each
    /// from the tick alone as Oscillate moves one.
    bool deck = false;
    /// The crates are kept for the host to simulate.
    bool hostSimulated = false;
};

/// A light crate's half size (m), and its mass (kg): one a character shoves
/// along at a walk.
constexpr float kCrateHalf = 0.4f;
constexpr float kCrateMass = 10.f;

/// The deck's size (m), and where its top is.
constexpr glm::vec3 kDeckHalfExtents{6.f, 0.25f, 6.f};
constexpr float kDeckTop = 0.5f;

/// How fast the deck travels (m/s) and turns (rad/s). Turning, so a rider
/// stepped again against a deck standing still walks a different line.
constexpr glm::vec3 kDeckVelocity{0.5f, 0.f, 0.25f};
constexpr float kDeckYawRate = 0.5f;

/// A moving deck in @p scene, as RigLevel::deck describes.
ECS::Entity AddDeck(ECS::Scene &scene, Physics::PhysicsWorld &physics)
{
    const ECS::Entity deck = SpawnBox(scene, physics, {0.f, kDeckTop - kDeckHalfExtents.y, 0.f}, /*isStatic=*/ false,
                                      kDeckHalfExtents, /*replicated=*/ false);
    scene.GetMut<Physics::RigidBody>(deck)->motion = Physics::MotionType::Kinematic;
    (void)scene.Add<Physics::Carrier>(deck, Physics::Carrier{});
    physics.Reconcile();
    return deck;
}

/// Puts @p deck where it is at @p tick, having set off at @p from.
void MoveDeck(ECS::Scene &scene, ECS::Entity deck, std::uint64_t tick, std::uint64_t from)
{
    if (deck == ECS::NullEntity || tick < from)
    {
        return;
    }
    const float seconds = static_cast<float>(tick - from) * kFixedStep;
    ECS::Transform &transform = *scene.GetMut<ECS::Transform>(deck);
    transform.position = glm::vec3{0.f, kDeckTop - kDeckHalfExtents.y, 0.f} + kDeckVelocity * seconds;
    transform.rotation = glm::angleAxis(kDeckYawRate * seconds, glm::vec3{0.f, 1.f, 0.f});
}

/// Both machines walking the client's characters the same way, the client
/// kLeadTicks ahead and sending its commands as a player's machine does, with
/// each machine's step and the wire between them run by hand so a case
/// decides when a snapshot lands. The client steps its physics alone, and
/// replays through PhysicsWorld::StepCharacters, as App's StepWorld does.
struct PredictionRig
{
    PhysicsHarness harness;
    ECS::Entity character{ECS::NullEntity};
    ECS::Entity mirror{ECS::NullEntity};
    ECS::Entity otherPlayer{ECS::NullEntity};
    /// A second character the client controls, when the case asks for one.
    ECS::Entity second{ECS::NullEntity};
    ECS::Entity secondMirror{ECS::NullEntity};
    /// The level's crates on the host, and their mirrors, in the same order.
    std::vector<ECS::Entity> crates;
    std::vector<ECS::Entity> crateMirrors;
    glm::vec3 move{1.f, 0.f, 0.f};
    InputCommandBuffer sent;
    /// What the host stepped its characters on last, and the steps in a row
    /// it had no command.
    glm::vec3 hostMove{0.f};
    std::uint32_t hostStarved = 0;
    /// Called with each machine's scene, its copy of the client's character and
    /// the tick after every live step it takes: the rig's FixedUpdate system,
    /// a game's, which a replay does not run.
    std::function<void(ECS::Scene &, ECS::Entity, std::uint64_t)> afterStep;
    /// The newest tick the client stepped.
    std::uint64_t clientTick = 0;
    /// Replays run, and steps they took.
    std::uint32_t replays = 0;
    std::uint32_t replayedSteps = 0;

    /// The deck on each machine, when the level has one, and the tick it sets
    /// off at on both.
    ECS::Entity hostDeck{ECS::NullEntity};
    ECS::Entity clientDeck{ECS::NullEntity};
    std::uint64_t deckFrom = 0;

    explicit PredictionRig(const RigLevel &level = {}, const ReplicationConfig &config = {}) : harness(config)
    {
        harness.Step(4);
        if (level.deck)
        {
            hostDeck = AddDeck(harness.serverScene, harness.serverPhysics);
            clientDeck = AddDeck(harness.clientScene, harness.clientPhysics);
        }
        character = SpawnStanding(harness, glm::vec3{0.f, level.deck ? kDeckTop : 0.f, 0.f});
        (void)harness.serverScene.Add<Test::Charge>(character, Test::Charge{});
        if (level.other.has_value())
        {
            otherPlayer = harness.serverScene.Create();
            (void)harness.serverScene.Add<ECS::Transform>(otherPlayer, ECS::Transform{.position = *level.other});
            (void)harness.serverScene.Add<Physics::Character>(otherPlayer, Physics::Character{});
            (void)harness.serverScene.Add<Replicated>(otherPlayer, Replicated{});
        }
        for (const glm::vec3 at : level.crates)
        {
            const ECS::Entity crate = SpawnBox(harness.serverScene, harness.serverPhysics,
                                               glm::vec3{at.x, kCrateHalf, at.z}, /*isStatic=*/ false,
                                               glm::vec3{kCrateHalf});
            harness.serverScene.GetMut<Physics::RigidBody>(crate)->mass = kCrateMass;
            if (level.hostSimulated)
            {
                (void)harness.serverScene.Add<HostSimulated>(crate, HostSimulated{});
            }
            crates.push_back(crate);
        }
        const ClientId client = harness.server.ClientIdOf(harness.serverSide());
        harness.server.SetControl(character, client);
        if (level.secondAt.has_value())
        {
            second = harness.serverScene.Create();
            (void)harness.serverScene.Add<ECS::Transform>(second, ECS::Transform{.position = *level.secondAt});
            (void)harness.serverScene.Add<Physics::Character>(second, Physics::Character{});
            (void)harness.serverScene.Add<Replicated>(second, Replicated{});
            harness.server.SetControl(second, client);
        }
        harness.Step(30);
        mirror = MirrorOf(harness, character);
        REQUIRE(mirror != ECS::NullEntity);
        REQUIRE(harness.client.ControlsEntity(mirror));
        if (second != ECS::NullEntity)
        {
            secondMirror = MirrorOf(harness, second);
            REQUIRE(secondMirror != ECS::NullEntity);
        }
        for (const ECS::Entity crate : crates)
        {
            crateMirrors.push_back(MirrorOf(harness, crate));
            REQUIRE(crateMirrors.back() != ECS::NullEntity);
        }
        Deliver();
        deckFrom = harness.tick;

        // The client's first commands, sent before the host reaches them and
        // there when it does.
        for (std::uint32_t i = 0; i < kLeadTicks; ++i)
        {
            StepClient(harness.tick + i);
        }
        Deliver();
    }

    /// The client's characters, by what each machine calls them.
    [[nodiscard]] std::vector<ECS::Entity> HostCharacters() const
    {
        return second == ECS::NullEntity ? std::vector<ECS::Entity>{character}
                                         : std::vector<ECS::Entity>{character, second};
    }
    [[nodiscard]] std::vector<ECS::Entity> ClientCharacters() const
    {
        return second == ECS::NullEntity ? std::vector<ECS::Entity>{mirror}
                                         : std::vector<ECS::Entity>{mirror, secondMirror};
    }

    /// What a client's StepWorld does before a live step: steps again every
    /// tick since the host's word it got wrong, on what the host applied.
    void ReplayPending()
    {
        std::optional<ReplicationClient::Replay> replay = harness.client.BeginRollback();
        const bool syncTest = !replay.has_value();
        if (syncTest)
        {
            replay = harness.client.BeginSyncTest();
        }
        if (!replay.has_value())
        {
            return;
        }
        replays += syncTest ? 0u : 1u;
        glm::vec3 held = move;
        for (const std::uint64_t tick : replay->ticks)
        {
            const ReplicationClient::AppliedCommand applied = harness.client.CommandAppliedAt(tick);
            held = MoveFor(applied.command, held, applied.starvedTicks);
            for (const ECS::Entity entity : ClientCharacters())
            {
                harness.clientScene.GetMut<Physics::CharacterIntent>(entity)->move = held;
            }
            harness.clientPhysics.StepCharacters(replay->characters, harness.client.FollowersAt(tick), kFixedStep,
                                                 replay->bodies);
            harness.client.RecordHistory(tick);
            ++replayedSteps;
        }
        harness.client.EndRollback();
        if (syncTest)
        {
            harness.client.EndSyncTest();
        }
    }

    /// Hands every message waiting on the wire to its side.
    void Deliver() { DeliverToClient(std::numeric_limits<std::size_t>::max()); }

    /// Hands the host everything waiting for it, and the client the oldest
    /// @p most messages waiting for it; the rest wait for a later call.
    void DeliverToClient(std::size_t most)
    {
        std::vector<Net::NetEvent> events;
        harness.transport->Poll(events);
        for (Net::NetEvent &event : events)
        {
            if (event.type != Net::NetEvent::Type::Message)
                continue;
            if (event.connection == harness.pair.first)
                harness.server.HandleMessage(harness.pair.first, event.payload);
            else if (event.connection == harness.pair.second)
                forClient.push_back(std::move(event));
        }
        for (std::size_t handed = 0; handed < most && !forClient.empty(); ++handed)
        {
            harness.client.HandleMessage(forClient.front().payload);
            forClient.pop_front();
        }
        // A despawned mirror goes when the frame ends, as an application ends it.
        harness.clientScene.FlushDestroyed();
    }

    /// Messages to the client not handed to it yet, oldest first.
    std::deque<Net::NetEvent> forClient;

    /// The client's step for command @p tick, as a client takes one: any replay
    /// first, then the step and the host's sleep verdicts held to, its record,
    /// and its command sent.
    void StepClient(std::uint64_t tick)
    {
        clientTick = tick;
        ReplayPending();
        InputCommand command;
        command.tick = tick;
        command.moveX = move.x;
        command.moveY = move.z;
        for (const ECS::Entity entity : ClientCharacters())
        {
            harness.clientScene.GetMut<Physics::CharacterIntent>(entity)->move = move;
        }
        MoveDeck(harness.clientScene, clientDeck, tick, deckFrom);
        harness.clientPhysics.Update(kFixedStep);
        harness.client.EnforceSleep();
        if (afterStep)
        {
            afterStep(harness.clientScene, mirror, tick);
        }
        harness.client.RecordStep(command);
        if (!loseCommands)
        {
            sent.Push(command);
            harness.client.SendInput(sent);
        }
    }

    /// While set, the client's commands are lost on the way, the copies of
    /// them later packets repeat included.
    bool loseCommands = false;

    /// The host's step of its tick, on the command the client sent for it.
    void StepHost()
    {
        const InputCommand *command = harness.server.ConsumeInput(harness.serverSide(), harness.tick);
        hostStarved = command != nullptr ? 0u : hostStarved + 1u;
        hostMove = MoveFor(command, hostMove, hostStarved);
        for (const ECS::Entity entity : HostCharacters())
        {
            harness.serverScene.GetMut<Physics::CharacterIntent>(entity)->move = hostMove;
        }
        MoveDeck(harness.serverScene, hostDeck, harness.tick, deckFrom);
        harness.serverPhysics.Update(kFixedStep);
        if (afterStep)
        {
            afterStep(harness.serverScene, character, harness.tick);
        }
        harness.server.Tick(harness.tick++);
    }

    /// One tick on both machines: the host's step and snapshot, and the
    /// client's step kLeadTicks further on. Nothing is delivered.
    void StepBoth()
    {
        StepHost();
        StepClient(harness.tick - 1u + kLeadTicks);
    }

    [[nodiscard]] glm::vec3 ClientPosition() const
    {
        return harness.clientScene.Get<ECS::Transform>(mirror)->position;
    }
};

} // namespace

TEST_CASE("a correction that agrees with the client's prediction leaves its character alone")
{
    // The host's answer for a tick arrives a round trip after the client stepped
    // it. Snapping to it puts the character back where it was that long ago.
    PredictionRig rig;
    const std::uint64_t correctionsBefore = rig.harness.client.Corrections().applied;
    for (std::uint32_t i = 0; i < kAgreeingTicks; ++i)
    {
        rig.StepBoth();
        const glm::vec3 before = rig.ClientPosition();
        rig.Deliver();
        rig.ReplayPending();
        CHECK(rig.ClientPosition() == before);
    }

    // It walked the whole time, so the corrections were about a moving character.
    CHECK(rig.harness.client.Corrections().applied > correctionsBefore);
    CHECK(rig.harness.client.Corrections().predictionsKept > 0);
    CHECK(rig.harness.client.Corrections().predictionsReplayed == 0);
    CHECK(rig.replays == 0);
    CHECK(rig.ClientPosition().x > 0.1f);
}

TEST_CASE("a correction the client got wrong is replayed from the host's state, not snapped to the past")
{
    PredictionRig rig;
    for (std::uint32_t i = 0; i < kAgreeingTicks; ++i)
    {
        rig.StepBoth();
        rig.Deliver();
    }

    // Something only the host knows of moves its copy sideways.
    const glm::vec3 shove{0.f, 0.f, kHostShove};
    const Physics::Pose hostPose = rig.harness.serverPhysics.GetBodyPose(rig.character);
    rig.harness.serverPhysics.ApplyCorrection(
        rig.character, Physics::Pose{hostPose.rotation, hostPose.position + shove},
        Physics::BodyState{.linearVelocity = rig.harness.serverPhysics.GetBodyState(rig.character).linearVelocity});

    // Corrected to where the host has it now, plus the steps the client has
    // taken since: still ahead, and over by the shove.
    const std::uint64_t correctionsBefore = rig.harness.client.Corrections().applied;
    glm::vec3 before{0.f};
    for (std::uint32_t i = 0; i < kAgreeingTicks && rig.harness.client.Corrections().applied == correctionsBefore; ++i)
    {
        rig.StepBoth();
        before = rig.ClientPosition();
        rig.Deliver();
        rig.ReplayPending();
    }
    REQUIRE(rig.harness.client.Corrections().applied > correctionsBefore);
    CHECK(glm::length(rig.ClientPosition() - (before + shove)) < kReplayedNear);
    CHECK(rig.harness.client.Corrections().predictionsReplayed == 1);
    CHECK(rig.replays == 1);
    // From the host's tick to the newest the client stepped.
    CHECK(rig.replayedSteps == kLeadTicks);

    // Eased across rather than jumped: one frame later it is drawn nearer
    // where it was than where it went.
    rig.harness.client.SmoothView(0.0, kFixedStep);
    CHECK(glm::length(RenderedPosition(rig.harness.clientScene, rig.mirror) - before) < kHostShove / 2.f);

    SUBCASE("and the replay is what the next agreeing correction is compared with")
    {
        for (std::uint32_t i = 0; i < kAgreeingTicks; ++i)
        {
            rig.StepBoth();
            const glm::vec3 settled = rig.ClientPosition();
            rig.Deliver();
            rig.ReplayPending();
            CHECK(rig.ClientPosition() == settled);
        }
        CHECK(rig.replays == 1);
    }
}

namespace
{

/// Where the client's second character stands: clear of the first.
constexpr glm::vec3 kSecondCharacterAt{0.f, 0.f, 3.f};

/// How far a replay may move a character the host agreed with (m): far under
/// the two millimetres the host's position is rounded to, which is what it
/// would move by if it were put where the host had it.
constexpr float kMatchedUnmoved = 1e-4f;

/// Ticks a client runs ahead with no word from the host: past every tick it
/// keeps.
constexpr std::uint32_t kPastTheRing = static_cast<std::uint32_t>(kPredictionHistoryTicks) + 6u;

/// Ticks between two snapshots at the default rates.
constexpr std::uint32_t kSnapshotIntervalTicks = 3;

/// Ticks of three snapshot intervals.
constexpr std::uint32_t kBurstTicks = 3 * kSnapshotIntervalTicks;

/// Shoves @p rig's host copy of @p entity sideways by @p shove, as something
/// only the host knows of would.
void ShoveOnHost(PredictionRig &rig, ECS::Entity entity, glm::vec3 shove)
{
    const Physics::Pose pose = rig.harness.serverPhysics.GetBodyPose(entity);
    rig.harness.serverPhysics.ApplyCorrection(
        entity, Physics::Pose{pose.rotation, pose.position + shove},
        Physics::BodyState{.linearVelocity = rig.harness.serverPhysics.GetBodyState(entity).linearVelocity});
}

/// A FixedUpdate system draining the second character's charge by one a step
/// from tick `from` on, on whichever machine steps it.
struct Drain
{
    std::uint64_t from = 0;
    std::uint64_t newestOnClient = 0; ///< The newest tick the client drained at.

    void Step(PredictionRig &rig, ECS::Scene &scene, std::uint64_t tick)
    {
        const bool onClient = &scene == &rig.harness.clientScene;
        if (tick >= from)
        {
            --scene.GetMut<Test::Charge>(onClient ? rig.secondMirror : rig.second)->level;
        }
        if (onClient)
        {
            newestOnClient = std::max(newestOnClient, tick);
        }
    }
};

/// Runs @p rig until the client has replayed once, or kAgreeingTicks pass.
void RunUntilReplayed(PredictionRig &rig)
{
    const std::uint32_t before = rig.replays;
    for (std::uint32_t i = 0; i < kAgreeingTicks && rig.replays == before; ++i)
    {
        rig.StepBoth();
        rig.Deliver();
        rig.ReplayPending();
    }
}

} // namespace

TEST_CASE("a rollback for one predicted character leaves the other where its own steps put it")
{
    // The host agreed with the second, so it is put back to the client's own
    // record and stepped again; put back where the host had it instead, it
    // would move by the host's rounding on every rollback of its neighbour.
    PredictionRig rig(RigLevel{.other = std::nullopt, .secondAt = kSecondCharacterAt, .deck = false});
    for (std::uint32_t i = 0; i < kAgreeingTicks; ++i)
    {
        rig.StepBoth();
        rig.Deliver();
        rig.ReplayPending();
    }
    REQUIRE(rig.replays == 0);

    ShoveOnHost(rig, rig.character, glm::vec3{0.f, 0.f, -kHostShove});
    glm::vec3 secondBefore{0.f};
    for (std::uint32_t i = 0; i < kAgreeingTicks && rig.replays == 0; ++i)
    {
        rig.StepBoth();
        secondBefore = rig.harness.clientScene.Get<ECS::Transform>(rig.secondMirror)->position;
        rig.Deliver();
        rig.ReplayPending();
    }
    REQUIRE(rig.replays == 1);
    CHECK(glm::length(rig.harness.clientScene.Get<ECS::Transform>(rig.secondMirror)->position - secondBefore) <
          kMatchedUnmoved);
}

TEST_CASE("a character riding a moving, turning deck steps again with it, to where its own steps put it")
{
    // A rollback for the first character puts the second, which the host
    // agreed with, back to its own record; stepped again against the deck
    // where it is now rather than where it was, it would walk a different line.
    const glm::vec3 secondOnDeck{0.f, kDeckTop, 3.f};
    PredictionRig rig(RigLevel{.other = std::nullopt, .secondAt = secondOnDeck, .deck = true});
    for (std::uint32_t i = 0; i < kAgreeingTicks; ++i)
    {
        rig.StepBoth();
        rig.Deliver();
        rig.ReplayPending();
    }
    REQUIRE(rig.replays == 0);
    REQUIRE(rig.harness.clientScene.Get<Physics::CharacterState>(rig.secondMirror)->baseEntity == rig.clientDeck);

    ShoveOnHost(rig, rig.character, glm::vec3{0.f, 0.f, kHostShove});
    ECS::Transform secondBefore;
    for (std::uint32_t i = 0; i < kAgreeingTicks && rig.replays == 0; ++i)
    {
        rig.StepBoth();
        secondBefore = *rig.harness.clientScene.Get<ECS::Transform>(rig.secondMirror);
        rig.Deliver();
        rig.ReplayPending();
    }
    REQUIRE(rig.replays == 1);
    const ECS::Transform &secondAfter = *rig.harness.clientScene.Get<ECS::Transform>(rig.secondMirror);
    CHECK(glm::length(secondAfter.position - secondBefore.position) < kMatchedUnmoved);
    CHECK(std::abs(glm::dot(secondAfter.rotation, secondBefore.rotation)) > 1.f - kMatchedUnmoved);
    CHECK(rig.harness.clientScene.Get<Physics::CharacterState>(rig.secondMirror)->baseEntity == rig.clientDeck);
}

TEST_CASE("a predicted character the host despawns is neither put back nor remembered")
{
    PredictionRig rig(RigLevel{.other = std::nullopt, .secondAt = kSecondCharacterAt, .deck = false});
    for (std::uint32_t i = 0; i < kAgreeingTicks; ++i)
    {
        rig.StepBoth();
        rig.Deliver();
        rig.ReplayPending();
    }
    const NetId secondId = rig.harness.server.NetIdOf(rig.second);
    REQUIRE(rig.harness.client.PredictionCountsOf(secondId) != nullptr);

    // The host takes the second away and shoves the first in the same tick: the
    // rollback that follows covers ticks the second was alive for.
    rig.harness.serverScene.Destroy(rig.second);
    rig.harness.serverScene.FlushDestroyed();
    rig.second = ECS::NullEntity;
    ShoveOnHost(rig, rig.character, glm::vec3{0.f, 0.f, kHostShove});
    const ECS::Entity secondMirror = rig.secondMirror;
    rig.secondMirror = ECS::NullEntity;
    RunUntilReplayed(rig);

    REQUIRE(rig.replays == 1);
    CHECK_FALSE(rig.harness.clientScene.IsAlive(secondMirror));
    CHECK(rig.harness.client.PredictionCountsOf(secondId) == nullptr);
}

TEST_CASE("an answer from before every tick the client kept snaps the character to the host's")
{
    PredictionRig rig;
    rig.StepBoth();
    rig.Deliver();

    // The client runs on alone, far past what it keeps.
    for (std::uint32_t i = 0; i < kPastTheRing; ++i)
    {
        rig.StepClient(rig.harness.tick + kLeadTicks + i);
    }
    ShoveOnHost(rig, rig.character, glm::vec3{0.f, 0.f, kHostShove});
    // Enough host steps for one to send a snapshot.
    for (std::uint32_t i = 0; i < kSnapshotIntervalTicks; ++i)
    {
        rig.StepHost();
    }
    rig.Deliver();
    rig.ReplayPending();

    CHECK(rig.replays == 0);
    CHECK(rig.harness.client.Corrections().hardSnaps == 1);
    const ReplicationClient::Confirmed *host = rig.harness.client.ConfirmedOf(rig.harness.server.NetIdOf(rig.character));
    REQUIRE(host != nullptr);
    CHECK(glm::length(rig.ClientPosition() - host->body.position) < kMatchedUnmoved);
}

TEST_CASE("a predicted component is not stepped again, and keeps what the live steps made it")
{
    // A FixedUpdate system drains the second character's charge every live
    // step from one tick on, on both machines. A rollback for the first
    // character steps characters again, not the game's systems: the second's
    // charge keeps what the live steps left, and its history what they
    // recorded, so the host's later answers agree with it. The host answers
    // every tick, so each tick the rollback stepped again is compared.
    constexpr std::int32_t kFullCharge = 1000;
    ReplicationConfig everyTick;
    everyTick.snapshotHz = everyTick.tickRateHz;
    PredictionRig rig(RigLevel{.other = std::nullopt, .secondAt = kSecondCharacterAt, .deck = false}, everyTick);
    (void)rig.harness.serverScene.Add<Test::Charge>(rig.second, Test::Charge{.level = kFullCharge});
    for (std::uint32_t i = 0; i < kAgreeingTicks; ++i)
    {
        rig.StepBoth();
        rig.Deliver();
        rig.ReplayPending();
    }
    const std::uint32_t replaysBeforeDrain = rig.replays;
    REQUIRE(rig.harness.clientScene.Get<Test::Charge>(rig.secondMirror) != nullptr);
    REQUIRE(rig.harness.clientScene.Get<Test::Charge>(rig.secondMirror)->level == kFullCharge);

    // From a tick neither machine has stepped yet.
    Drain drain{.from = rig.harness.tick + kLeadTicks};
    rig.afterStep = [&rig, &drain](ECS::Scene &scene, ECS::Entity entity, std::uint64_t tick)
                    {
                        (void)entity;
                        drain.Step(rig, scene, tick);
                    };
    for (std::uint32_t i = 0; i < kAgreeingTicks; ++i)
    {
        rig.StepBoth();
        rig.Deliver();
        rig.ReplayPending();
    }
    REQUIRE(rig.replays == replaysBeforeDrain);

    ShoveOnHost(rig, rig.character, glm::vec3{0.f, 0.f, kHostShove});
    RunUntilReplayed(rig);
    REQUIRE(rig.replays == replaysBeforeDrain + 1u);
    const std::int32_t drained = static_cast<std::int32_t>(drain.newestOnClient - drain.from + 1u);
    CHECK(rig.harness.clientScene.Get<Test::Charge>(rig.secondMirror)->level == kFullCharge - drained);

    // Answers about the ticks the replay stepped again.
    for (std::uint32_t i = 0; i < kAgreeingTicks; ++i)
    {
        rig.StepBoth();
        rig.Deliver();
        rig.ReplayPending();
    }
    const ReplicationClient::EntityPredictionCounts *counts =
        rig.harness.client.PredictionCountsOf(rig.harness.server.NetIdOf(rig.second));
    REQUIRE(counts != nullptr);
    const std::unordered_map<Core::Reflect::ComponentId, ReplicationClient::PredictionCounts>::const_iterator charge =
        counts->components.find(Core::Reflect::ComponentIdOf<Test::Charge>());
    REQUIRE(charge != counts->components.end());
    CHECK(charge->second.compares > 0);
    CHECK(charge->second.corrections == 0);
}

TEST_CASE("a tick the host stepped without the client's command is stepped again on what the host held")
{
    // Past the host's hold, so it holds some of the gap and lets go of the rest.
    constexpr std::uint32_t kLostTicks = kRigHoldTicks + 2u;
    PredictionRig rig;
    for (std::uint32_t i = 0; i < kAgreeingTicks; ++i)
    {
        rig.StepBoth();
        rig.Deliver();
        rig.ReplayPending();
    }

    const std::uint64_t lastSent = rig.clientTick;
    rig.loseCommands = true;
    for (std::uint32_t i = 0; i < kLostTicks; ++i)
    {
        rig.StepBoth();
        rig.Deliver();
        rig.ReplayPending();
    }
    rig.loseCommands = false;
    // Until the host has stepped and spoken of every lost tick.
    for (std::uint32_t i = 0; i < kAgreeingTicks; ++i)
    {
        rig.StepBoth();
        rig.Deliver();
        rig.ReplayPending();
    }

    const ReplicationClient::AppliedCommand before = rig.harness.client.CommandAppliedAt(lastSent);
    CHECK(before.command != nullptr);
    for (std::uint32_t i = 0; i < kLostTicks; ++i)
    {
        CAPTURE(i);
        const ReplicationClient::AppliedCommand lost = rig.harness.client.CommandAppliedAt(lastSent + 1u + i);
        CHECK(lost.command == nullptr);
        CHECK(lost.starvedTicks == i + 1u);
    }
    const ReplicationClient::AppliedCommand after = rig.harness.client.CommandAppliedAt(lastSent + 1u + kLostTicks);
    CHECK(after.command != nullptr);
}

TEST_CASE("a predicted component the host disagrees with takes the host's value without a replay, and is named")
{
    // The client's charge never changes and the host's changes every tick: a
    // disagreement on every snapshot. Each is a correction to the host's
    // value, and none steps anything again.
    PredictionRig rig;
    for (std::uint32_t i = 0; i < kAgreeingTicks; ++i)
    {
        rig.StepBoth();
        rig.Deliver();
        rig.ReplayPending();
    }
    rig.afterStep = [&rig](ECS::Scene &scene, ECS::Entity entity, std::uint64_t tick)
                    {
                        (void)tick;
                        if (&scene == &rig.harness.serverScene)
                        {
                            ++scene.GetMut<Test::Charge>(entity)->level;
                        }
                    };
    const std::uint32_t replaysBefore = rig.replays;
    for (std::uint32_t i = 0; i < kBurstTicks; ++i)
    {
        rig.StepBoth();
        rig.Deliver();
        rig.ReplayPending();
    }

    CHECK(rig.replays == replaysBefore);
    const NetId netId = rig.harness.server.NetIdOf(rig.character);
    const ReplicationClient::EntityPredictionCounts *counts = rig.harness.client.PredictionCountsOf(netId);
    REQUIRE(counts != nullptr);
    const std::unordered_map<Core::Reflect::ComponentId, ReplicationClient::PredictionCounts>::const_iterator charge =
        counts->components.find(Core::Reflect::ComponentIdOf<Test::Charge>());
    REQUIRE(charge != counts->components.end());
    CHECK(charge->second.compares >= 2);
    CHECK(charge->second.corrections >= 2);
    CHECK(charge->second.firstDifference.find("level") != std::string::npos);
    CHECK(counts->physics.corrections == 0);

    // The host's charge, which only the host raises, as of its newest answer.
    const ReplicationClient::Confirmed *host = rig.harness.client.ConfirmedOf(netId);
    REQUIRE(host != nullptr);
    const ReplicationClient::Confirmed::Components::const_iterator sent =
        host->components.find(Core::Reflect::ComponentIdOf<Test::Charge>());
    REQUIRE(sent != host->components.end());
    REQUIRE(sent->second.size() == sizeof(Test::Charge));
    Test::Charge hostCharge;
    std::memcpy(&hostCharge, sent->second.data(), sizeof(Test::Charge));
    CHECK(hostCharge.level > 0);
    CHECK(rig.harness.clientScene.Get<Test::Charge>(rig.mirror)->level == hostCharge.level);
}

namespace
{

/// The client's counts for the rig's character as they stand.
ReplicationClient::EntityPredictionCounts CharacterCounts(const PredictionRig &rig)
{
    const ReplicationClient::EntityPredictionCounts *counts =
        rig.harness.client.PredictionCountsOf(rig.harness.server.NetIdOf(rig.character));
    REQUIRE(counts != nullptr);
    return *counts;
}

/// The Charge section of @p counts.
ReplicationClient::PredictionCounts ChargeOf(const ReplicationClient::EntityPredictionCounts &counts)
{
    const std::unordered_map<Core::Reflect::ComponentId, ReplicationClient::PredictionCounts>::const_iterator charge =
        counts.components.find(Core::Reflect::ComponentIdOf<Test::Charge>());
    REQUIRE(charge != counts.components.end());
    return charge->second;
}

/// Steps both machines @p ticks times, delivering and replaying after each.
void RunFed(PredictionRig &rig, std::uint32_t ticks)
{
    for (std::uint32_t i = 0; i < ticks; ++i)
    {
        rig.StepBoth();
        rig.Deliver();
        rig.ReplayPending();
    }
}

} // namespace

TEST_CASE("a correction at a tick the host stepped without the client's command is counted as starved")
{
    // The host raises the charge every tick and the client never does, so every
    // snapshot corrects it. While the client's commands arrive each correction
    // is a divergence; while they are lost on the wire the host steps on what it
    // held, and each is counted apart, as the connection's doing.
    PredictionRig rig;
    RunFed(rig, kAgreeingTicks);
    rig.afterStep = [&rig](ECS::Scene &scene, ECS::Entity entity, std::uint64_t tick)
                    {
                        (void)tick;
                        if (&scene == &rig.harness.serverScene)
                        {
                            ++scene.GetMut<Test::Charge>(entity)->level;
                        }
                    };
    RunFed(rig, kBurstTicks);
    const ReplicationClient::EntityPredictionCounts fed = CharacterCounts(rig);
    CHECK(ChargeOf(fed).corrections > 0);
    CHECK(ChargeOf(fed).starvedCorrections == 0);
    CHECK(fed.physics.starvedCorrections == 0);

    // The commands already on their way still arrive: the host steps on them
    // until the client's lead and a snapshot have passed. Then it holds the
    // last controls a few steps and lets go, so the character it stops is
    // rolled back to on the client, which kept walking.
    rig.loseCommands = true;
    RunFed(rig, kLeadTicks + kSnapshotIntervalTicks);
    const ReplicationClient::EntityPredictionCounts lost = CharacterCounts(rig);
    RunFed(rig, kRigHoldTicks + kBurstTicks);
    const ReplicationClient::EntityPredictionCounts starved = CharacterCounts(rig);
    CHECK(ChargeOf(starved).corrections == ChargeOf(lost).corrections);
    CHECK(ChargeOf(starved).starvedCorrections > ChargeOf(lost).starvedCorrections);
    CHECK(starved.physics.corrections == lost.physics.corrections);
    CHECK(starved.physics.starvedCorrections > lost.physics.starvedCorrections);

    const std::vector<NetId> predicted = rig.harness.client.PredictedEntities();
    CHECK(std::ranges::find(predicted, rig.harness.server.NetIdOf(rig.character)) != predicted.end());
}

TEST_CASE("a client walking its character into another player is stopped by it, on both machines, without jitter")
{
    // Characters block each other, so neither machine moves the other player.
    // Were the mirror shoved on the client alone, the host's next word would put
    // it back into the way of the client's character, which would be pushed
    // back in turn: a jitter every snapshot for as long as the two touch.
    PredictionRig rig(RigLevel{.other = kOtherPlayerAhead, .secondAt = std::nullopt, .deck = false});
    const ECS::Entity otherMirror = MirrorOf(rig.harness, rig.otherPlayer);
    REQUIRE(otherMirror != ECS::NullEntity);

    float mostBack = 0.f;
    float mostAheadOfHost = 0.f;
    glm::vec3 last = rig.ClientPosition();
    for (std::uint32_t i = 0; i < kPushTicks; ++i)
    {
        rig.StepBoth();
        rig.Deliver();
        const glm::vec3 now = rig.ClientPosition();
        mostBack = std::max(mostBack, last.x - now.x);
        const float onClient = rig.harness.clientScene.Get<ECS::Transform>(otherMirror)->position.x;
        const float onHost = rig.harness.serverScene.Get<ECS::Transform>(rig.otherPlayer)->position.x;
        mostAheadOfHost = std::max(mostAheadOfHost, onClient - onHost);
        last = now;
    }

    // It walked up to the other player and was stopped there, on the host too.
    const float otherOnHost = rig.harness.serverScene.Get<ECS::Transform>(rig.otherPlayer)->position.x;
    CHECK(std::abs(otherOnHost - kOtherPlayerAhead.x) < kNotPushed);
    const float touching = kOtherPlayerAhead.x - 2.f * Physics::Character{}.radius;
    REQUIRE(rig.ClientPosition().x > touching - kTouchSlack);
    CHECK(mostAheadOfHost < kMirrorNearHost);
    CHECK(mostBack < kMostBackPerTick);
}

TEST_CASE("a Collider with no RigidBody replicates as a Transform, not as a body")
{
    PhysicsHarness harness;
    harness.Step(4);
    const ECS::Entity wall = SpawnBox(harness.serverScene, harness.serverPhysics, {0.f, 2.f, 0.f}, /*isStatic=*/ true);
    harness.Step(30);

    const ECS::Entity mirror = MirrorOf(harness, wall);
    REQUIRE(mirror != ECS::NullEntity);
    CHECK(harness.clientScene.Has<Physics::Collider>(mirror));
    CHECK(harness.client.Corrections().applied == 0);
}

TEST_CASE("A client's world leaves breaking joints to the server")
{
    PhysicsHarness harness;

    CHECK(harness.serverPhysics.BreaksJoints());
    CHECK_FALSE(harness.clientPhysics.BreaksJoints());
}

namespace
{

/// Where a crate rests in front of the client's character, which walks along
/// +x from the origin: far enough that the crate is given before it is reached.
constexpr glm::vec3 kCrateAhead{2.5f, 0.f, 0.f};

/// Ticks a walk up to kCrateAhead takes, with room.
constexpr std::uint32_t kReachTicks = 60;

/// Ticks of a long push: several metres at a walk.
constexpr std::uint32_t kLongPushTicks = 200;

/// Further than a long push moves a crate when it is pushed at all (m).
constexpr float kPushedCrateFar = 3.f;

/// Ticks of walking away from a crate, past the leave distance.
constexpr std::uint32_t kWalkAwayTicks = 120;

/// How far apart two crates pushed one into the other may be drawn (m):
/// the solver's contact slop, not a gap anyone sees.
constexpr float kDrawnInContact = 0.02f;

/// A speed far past what a report may claim (m/s), and a jump far past what
/// one tick allows (m).
constexpr float kCheatSpeed = 100.f;
constexpr float kCheatJump = 10.f;

/// Ticks for a refused report to reach the host and the host's word to come
/// back: a few snapshots.
constexpr std::uint32_t kRoundTripTicks = 12;

/// How near the host's crate the client's is drawn once the host has it back
/// and it has settled (m).
constexpr float kBackWithHost = 0.05f;

/// An upward kick on a light crate (N s): over 5 m/s.
constexpr glm::vec3 kKickUp{0.f, 60.f, 0.f};

/// Steps the rig a tick on both machines and hands everything over.
void Tick(PredictionRig &rig)
{
    rig.StepBoth();
    rig.Deliver();
}

/// Walks the rig's character until the host has given crate @p index to the
/// client and the client knows; false if that never happened.
bool WalkUntilGiven(PredictionRig &rig, std::size_t index = 0)
{
    const ClientId client = rig.harness.server.ClientIdOf(rig.harness.serverSide());
    for (std::uint32_t i = 0; i < kReachTicks; ++i)
    {
        Tick(rig);
        if (rig.harness.server.SimulatorOf(rig.crates[index]) == client &&
            rig.harness.client.Simulates(rig.crateMirrors[index]))
        {
            return true;
        }
    }
    return false;
}

/// Ticks the rig until the host simulates @p crate itself; false if it never
/// does within a round trip.
bool TickUntilTakenBack(PredictionRig &rig, ECS::Entity crate)
{
    for (std::uint32_t i = 0; i < kRoundTripTicks; ++i)
    {
        Tick(rig);
        if (!rig.harness.server.SimulatorOf(crate).IsValid())
        {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("a crate is given to the client whose character comes near it, and taken back once it walks away")
{
    PredictionRig rig(RigLevel{.crates = {kCrateAhead}});
    const ClientId client = rig.harness.server.ClientIdOf(rig.harness.serverSide());
    REQUIRE(rig.harness.server.SimulatorOf(rig.crates[0]) == InvalidClientId);

    REQUIRE(WalkUntilGiven(rig));
    CHECK(rig.harness.server.SimulatorOf(rig.crates[0]) == client);
    // Given before it is reached, so the first push is already the client's.
    CHECK_FALSE(rig.harness.serverPhysics.IsTouching(rig.character, rig.crates[0]));

    rig.move = glm::vec3{-1.f, 0.f, 0.f};
    for (std::uint32_t i = 0; i < kWalkAwayTicks; ++i)
    {
        Tick(rig);
    }
    CHECK(rig.harness.server.SimulatorOf(rig.crates[0]) == InvalidClientId);
    CHECK_FALSE(rig.harness.client.Simulates(rig.crateMirrors[0]));
}

TEST_CASE("a crate its client pushes is never put back by the host on that client")
{
    PredictionRig rig(RigLevel{.crates = {kCrateAhead}});
    REQUIRE(WalkUntilGiven(rig));
    const ECS::Entity crate = rig.crateMirrors[0];
    const float start = rig.harness.clientPhysics.GetBodyPose(crate).position.x;

    std::uint32_t moved = 0;
    float largestOffset = 0.f;
    for (std::uint32_t i = 0; i < kLongPushTicks; ++i)
    {
        rig.StepBoth();
        const Physics::Pose before = rig.harness.clientPhysics.GetBodyPose(crate);
        rig.Deliver();
        const Physics::Pose after = rig.harness.clientPhysics.GetBodyPose(crate);
        moved += before.position == after.position && before.rotation == after.rotation ? 0u : 1u;
        if (const ECS::RenderOffset *offset = rig.harness.clientScene.Get<ECS::RenderOffset>(crate))
        {
            largestOffset = std::max(largestOffset, glm::length(offset->position));
        }
    }
    REQUIRE(rig.harness.clientPhysics.GetBodyPose(crate).position.x > start + kPushedCrateFar);
    CHECK(rig.harness.client.Simulates(crate));
    CHECK(moved == 0u);
    CHECK(largestOffset == 0.f);
}

TEST_CASE("a client's character pushing the crate it simulates is not corrected by the host")
{
    // The host writes each report over its crate at the report's tick, so its
    // copy of the character meets the crate where the client's met it.
    PredictionRig rig(RigLevel{.crates = {kCrateAhead}});
    REQUIRE(WalkUntilGiven(rig));
    const ECS::Entity crate = rig.crates[0];
    const float start = rig.harness.serverPhysics.GetBodyPose(crate).position.x;
    const NetId own = rig.harness.client.NetIdOf(rig.mirror);
    const std::uint64_t correctionsBefore = rig.harness.client.PredictionCountsOf(own)->physics.corrections;

    for (std::uint32_t i = 0; i < kLongPushTicks; ++i)
    {
        Tick(rig);
    }
    const ReplicationClient::EntityPredictionCounts *counts = rig.harness.client.PredictionCountsOf(own);
    REQUIRE(counts != nullptr);
    REQUIRE(rig.harness.serverPhysics.GetBodyPose(crate).position.x > start + kPushedCrateFar);
    REQUIRE(counts->physics.compares > 0u);
    CHECK(counts->physics.corrections == correctionsBefore);
    CHECK(rig.harness.server.Diagnostics(rig.harness.serverSide())->reportsRefused == 0u);
    CHECK(rig.harness.server.Diagnostics(rig.harness.serverSide())->reportsAccepted > 0u);
}

TEST_CASE("a crate pushed into a second crate is drawn against it on the client the whole push")
{
    // The second crate is the client's too once the two touch, so both run on
    // the client's own timeline; a mirror the host corrects a round trip late
    // was drawn centimetres ahead of the crate pushing it.
    // Given so near that the character never comes near enough the second
    // crate for it: that one is the client's only by touching the first.
    ReplicationConfig nearOnly;
    nearOnly.ownership.ownershipDistance = 1.f;
    nearOnly.ownership.leaveDistance = 2.f;
    const glm::vec3 behind = kCrateAhead + glm::vec3{2.f * kCrateHalf + 0.05f, 0.f, 0.f};
    PredictionRig rig(RigLevel{.crates = {kCrateAhead, behind}}, nearOnly);
    REQUIRE(WalkUntilGiven(rig, 0));
    REQUIRE(WalkUntilGiven(rig, 1));
    const ECS::Entity pushed = rig.crateMirrors[0];
    const ECS::Entity ahead = rig.crateMirrors[1];
    const float start = RenderedPosition(rig.harness.clientScene, ahead).x;

    // From the tick the two first meet: the solver counts them touching a few
    // centimetres before they do.
    float widest = 0.f;
    std::uint32_t together = 0;
    for (std::uint32_t i = 0; i < kLongPushTicks; ++i)
    {
        Tick(rig);
        const float gap = RenderedPosition(rig.harness.clientScene, ahead).x -
                          RenderedPosition(rig.harness.clientScene, pushed).x - 2.f * kCrateHalf;
        if (together == 0 && gap >= kDrawnInContact)
        {
            continue;
        }
        ++together;
        widest = std::max(widest, gap);
    }
    REQUIRE(RenderedPosition(rig.harness.clientScene, ahead).x > start + kPushedCrateFar);
    REQUIRE(together > kLongPushTicks / 2);
    CHECK(rig.harness.client.Simulates(pushed));
    CHECK(rig.harness.client.Simulates(ahead));
    CHECK(widest < kDrawnInContact);
}

TEST_CASE("with the sync test on, a client pushing a crate it simulates steps each tick again as it did live")
{
    PredictionRig rig(RigLevel{.crates = {kCrateAhead}});
    rig.harness.client.SetSyncTest(true);
    REQUIRE(WalkUntilGiven(rig));
    for (std::uint32_t i = 0; i < kLongPushTicks; ++i)
    {
        Tick(rig);
    }
    REQUIRE(rig.harness.serverPhysics.IsTouching(rig.character, rig.crates[0]));
    CHECK(rig.harness.client.SyncTestResults().replays > 0u);
    CHECK(rig.harness.client.SyncTestResults().differences == 0u);
}

TEST_CASE("a report the host does not believe is refused, and the client is shown the host's crate")
{
    SUBCASE("too fast")
    {
        PredictionRig rig(RigLevel{.crates = {kCrateAhead}});
        REQUIRE(WalkUntilGiven(rig));
        const ECS::Entity crate = rig.crateMirrors[0];
        rig.move = glm::vec3{0.f};
        rig.harness.clientPhysics.ApplyCorrection(crate, rig.harness.clientPhysics.GetBodyPose(crate),
                                                  Physics::BodyState{.linearVelocity = {0.f, 0.f, kCheatSpeed}});
        CHECK(TickUntilTakenBack(rig, rig.crates[0]));
        CHECK(rig.harness.server.Diagnostics(rig.harness.serverSide())->reportsRefused > 0u);
        for (std::uint32_t i = 0; i < kRoundTripTicks; ++i)
        {
            Tick(rig);
        }
        CHECK_FALSE(rig.harness.client.Simulates(crate));
        CHECK(PoseError(rig.harness, rig.crates[0]) < kBackWithHost);
    }
    SUBCASE("out of reach")
    {
        PredictionRig rig(RigLevel{.crates = {kCrateAhead}});
        REQUIRE(WalkUntilGiven(rig));
        const ECS::Entity crate = rig.crateMirrors[0];
        rig.move = glm::vec3{0.f};
        Physics::Pose far = rig.harness.clientPhysics.GetBodyPose(crate);
        far.position.z += kCheatJump;
        rig.harness.clientPhysics.ApplyCorrection(crate, far, Physics::BodyState{});
        CHECK(TickUntilTakenBack(rig, rig.crates[0]));
        CHECK(rig.harness.server.Diagnostics(rig.harness.serverSide())->reportsRefused > 0u);
        for (std::uint32_t i = 0; i < kRoundTripTicks; ++i)
        {
            Tick(rig);
        }
        CHECK_FALSE(rig.harness.client.Simulates(crate));
        CHECK(PoseError(rig.harness, rig.crates[0]) < kBackWithHost);
    }
}

TEST_CASE("a crate whose client stops reporting is taken back after the timeout")
{
    PredictionRig rig(RigLevel{.crates = {kCrateAhead}});
    REQUIRE(WalkUntilGiven(rig));
    rig.loseCommands = true;
    const std::uint32_t timeout = ReplicationConfig{}.ownership.reportTimeoutTicks;
    for (std::uint32_t i = 0; i + kRoundTripTicks < timeout; ++i)
    {
        Tick(rig);
    }
    REQUIRE(rig.harness.server.SimulatorOf(rig.crates[0]).IsValid());
    for (std::uint32_t i = 0; i < 2u * kRoundTripTicks; ++i)
    {
        Tick(rig);
    }
    CHECK(rig.harness.server.SimulatorOf(rig.crates[0]) == InvalidClientId);
}

TEST_CASE("a kick the host's own game gives a crate a client simulates takes it back, and lands")
{
    PredictionRig rig(RigLevel{.crates = {kCrateAhead}});
    REQUIRE(WalkUntilGiven(rig));
    rig.move = glm::vec3{0.f};
    const ECS::Entity crate = rig.crates[0];
    rig.harness.serverPhysics.AddImpulse(crate, kKickUp);
    rig.StepBoth();
    CHECK(rig.harness.server.SimulatorOf(crate) == InvalidClientId);
    CHECK(rig.harness.serverPhysics.GetBodyState(crate).linearVelocity.y > 1.f);

    float highest = 0.f;
    for (std::uint32_t i = 0; i < kRoundTripTicks * 2u; ++i)
    {
        Tick(rig);
        highest = std::max(highest, rig.harness.clientPhysics.GetBodyPose(rig.crateMirrors[0]).position.y);
    }
    CHECK(highest > kCrateHalf + 0.5f);
}

TEST_CASE("a crate kept for the host is never given to a client")
{
    PredictionRig rig(RigLevel{.crates = {kCrateAhead}, .hostSimulated = true});
    std::uint32_t given = 0;
    for (std::uint32_t i = 0; i < kLongPushTicks; ++i)
    {
        Tick(rig);
        given += rig.harness.server.SimulatorOf(rig.crates[0]).IsValid() ? 1u : 0u;
    }
    REQUIRE(rig.harness.serverPhysics.GetBodyPose(rig.crates[0]).position.x > kCrateAhead.x + kPushedCrateFar);
    CHECK(given == 0u);
    CHECK_FALSE(rig.harness.client.Simulates(rig.crateMirrors[0]));
}

TEST_CASE("a crate asleep on the host before the client reaches it moves the tick the client pushes it")
{
    // The host's word that it is asleep came while it was the host's; held to
    // it, the client would put the crate back to sleep after every step.
    constexpr std::uint32_t kSettleTicks = 180;
    PredictionRig rig(RigLevel{.crates = {kCrateAhead}});
    rig.move = glm::vec3{0.f};
    for (std::uint32_t i = 0; i < kSettleTicks; ++i)
    {
        Tick(rig);
    }
    REQUIRE_FALSE(rig.harness.serverPhysics.IsBodyActive(rig.crates[0]));
    REQUIRE_FALSE(rig.harness.clientPhysics.IsBodyActive(rig.crateMirrors[0]));

    rig.move = glm::vec3{1.f, 0.f, 0.f};
    REQUIRE(WalkUntilGiven(rig));
    const float start = rig.harness.clientPhysics.GetBodyPose(rig.crateMirrors[0]).position.x;
    for (std::uint32_t i = 0; i < kLongPushTicks; ++i)
    {
        Tick(rig);
    }
    CHECK(rig.harness.clientPhysics.GetBodyPose(rig.crateMirrors[0]).position.x > start + kPushedCrateFar);
}

namespace
{

/// A player with no connection, whose character only the host steps, as the
/// host's game moves it.
constexpr ClientId kStranger{99};

/// A crate ahead and to one side of the rig's character, and where a second
/// player stands beside it: both meet the crate's near face as they walk on.
constexpr glm::vec3 kSharedCrate{2.5f, 0.f, 0.35f};
constexpr glm::vec3 kBesideCharacter{0.f, 0.f, 0.7f};

/// A crate clear of the character, and one high above the floor and still
/// falling while a case runs.
constexpr glm::vec3 kCrateAside{0.f, 0.f, -3.f};
constexpr float kCrateFallingHeight = 12.f;

/// Ticks two players pushing together keep at it.
constexpr std::uint32_t kSharedPushTicks = 90;

/// The most a crate two players push, or the client's character pushing it,
/// may be drawn to move in a frame, in steps of a walk: 1.2 measured.
constexpr float kMostStepOverWalk = 1.5f;

/// The most times the host may correct a crate the client predicts while the
/// client pushes it alone the whole way: the client's starts from the host's
/// state as the wire rounds it, and over a long push the two drift a couple
/// of centimetres sideways.
constexpr std::uint64_t kMostPushCorrections = 3;

/// A second player's character standing at @p at, controlled by kStranger.
ECS::Entity AddStranger(PredictionRig &rig, glm::vec3 at)
{
    ECS::Scene &scene = rig.harness.serverScene;
    const ECS::Entity stranger = scene.Create();
    (void)scene.Add<ECS::Transform>(stranger, ECS::Transform{.position = at});
    (void)scene.Add<Physics::Character>(stranger, Physics::Character{});
    (void)scene.Add<Replicated>(stranger, Replicated{});
    rig.harness.server.SetControl(stranger, kStranger);
    rig.harness.serverPhysics.Reconcile();
    return stranger;
}

/// Whether the host runs the rig's crate itself while both the rig's
/// character and @p stranger touch it.
bool Contested(PredictionRig &rig, ECS::Entity stranger)
{
    const ECS::Entity crate = rig.crates[0];
    return rig.harness.serverPhysics.IsTouching(rig.character, crate) &&
           rig.harness.serverPhysics.IsTouching(stranger, crate) && !rig.harness.server.SimulatorOf(crate).IsValid();
}

/// Walks the rig's character and @p stranger on together until the host runs
/// the crate they both push and the client predicts it; false if that never
/// happened.
bool PushTogetherUntilContested(PredictionRig &rig, ECS::Entity stranger)
{
    for (std::uint32_t i = 0; i < kReachTicks; ++i)
    {
        rig.harness.serverScene.GetMut<Physics::CharacterIntent>(stranger)->move = rig.move;
        Tick(rig);
        if (Contested(rig, stranger) && rig.harness.client.Predicts(rig.crateMirrors[0]))
        {
            return true;
        }
    }
    return false;
}

/// Gives crate @p index to this rig's client to predict, as a game's own rule
/// would, and runs until the client has it.
void GiveCrate(PredictionRig &rig, std::size_t index = 0)
{
    (void)rig.harness.serverScene.Add<Predicted>(
        rig.crates[index], Predicted{rig.harness.server.ClientIdOf(rig.harness.serverSide()).value});
    for (std::uint32_t i = 0; i < kAgreeingTicks && !rig.harness.client.Predicts(rig.crateMirrors[index]); ++i)
    {
        Tick(rig);
    }
    REQUIRE(rig.harness.client.Predicts(rig.crateMirrors[index]));
}

/// How far an angular velocity component is rounded to on the wire (rad/s).
float AngularVelocityStep()
{
    const BodyQuantization &quantization = Quantization();
    return 2.f * quantization.angularVelocityMax / static_cast<float>(1u << quantization.angularVelocityBits);
}

} // namespace

TEST_CASE("a crate two players push at once is the host's to simulate, and the client predicts it")
{
    PredictionRig rig(RigLevel{.crates = {kSharedCrate}});
    const ECS::Entity stranger = AddStranger(rig, kBesideCharacter);
    REQUIRE(PushTogetherUntilContested(rig, stranger));
    CHECK(rig.harness.serverPhysics.IsTouching(stranger, rig.crates[0]));
    CHECK_FALSE(rig.harness.server.SimulatorOf(rig.crates[0]).IsValid());
    CHECK_FALSE(rig.harness.client.Simulates(rig.crateMirrors[0]));
}

TEST_CASE("a crate two players push together moves with the client's push, eased to the host's, never jumped")
{
    PredictionRig rig(RigLevel{.crates = {kSharedCrate}});
    const ECS::Entity stranger = AddStranger(rig, kBesideCharacter);
    REQUIRE(PushTogetherUntilContested(rig, stranger));
    const ECS::Entity crate = rig.crateMirrors[0];
    const glm::vec3 start = RenderedPosition(rig.harness.clientScene, crate);
    glm::vec3 last = start;
    glm::vec3 lastOwn = RenderedPosition(rig.harness.clientScene, rig.mirror);
    float mostBack = 0.f;
    float largestStep = 0.f;
    float largestOwnStep = 0.f;
    for (std::uint32_t i = 0; i < kSharedPushTicks; ++i)
    {
        rig.harness.serverScene.GetMut<Physics::CharacterIntent>(stranger)->move = rig.move;
        Tick(rig);
        rig.harness.client.SmoothView(0.0, kFixedStep);
        const glm::vec3 now = RenderedPosition(rig.harness.clientScene, crate);
        const glm::vec3 nowOwn = RenderedPosition(rig.harness.clientScene, rig.mirror);
        mostBack = std::max(mostBack, last.x - now.x);
        largestStep = std::max(largestStep, glm::length(now - last));
        largestOwnStep = std::max(largestOwnStep, glm::length(nowOwn - lastOwn));
        last = now;
        lastOwn = nowOwn;
    }
    const ReplicationClient::EntityPredictionCounts *counts =
        rig.harness.client.PredictionCountsOf(rig.harness.client.NetIdOf(crate));
    REQUIRE(counts != nullptr);
    REQUIRE(Contested(rig, stranger));
    REQUIRE(last.x > start.x + kPushedFar);
    REQUIRE(counts->physics.compares > 0u);
    // The other player's push reaches this client a round trip late, so it is
    // corrected as they push; each correction is eased, never drawn as a jump.
    const float walkStep = Physics::Character{}.walkSpeed * kFixedStep;
    CHECK(mostBack < kMostBackPerTick);
    CHECK(largestStep < walkStep * kMostStepOverWalk);
    CHECK(largestOwnStep < walkStep * kMostStepOverWalk);
}

TEST_CASE("a crate the host runs stays predicted a while after the client's character lets go, then is a mirror")
{
    // So a player pushing in bursts does not have it change hands with each.
    constexpr std::uint32_t kJustAfter = 10;
    constexpr std::uint32_t kLongAfter = 60;
    PredictionRig rig(RigLevel{.crates = {kSharedCrate}});
    const ECS::Entity stranger = AddStranger(rig, kBesideCharacter);
    REQUIRE(PushTogetherUntilContested(rig, stranger));

    rig.move = glm::vec3{-1.f, 0.f, 0.f};
    for (std::uint32_t i = 0; i < kJustAfter; ++i)
    {
        Tick(rig);
    }
    REQUIRE_FALSE(rig.harness.clientPhysics.IsTouching(rig.mirror, rig.crateMirrors[0]));
    CHECK(rig.harness.client.Predicts(rig.crateMirrors[0]));
    for (std::uint32_t i = 0; i < kLongAfter; ++i)
    {
        Tick(rig);
    }
    CHECK_FALSE(rig.harness.client.Predicts(rig.crateMirrors[0]));
}

TEST_CASE("a crate the other player lets go of is the client's to simulate after the dwell, handed over once")
{
    PredictionRig rig(RigLevel{.crates = {kSharedCrate}});
    const ECS::Entity stranger = AddStranger(rig, kBesideCharacter);
    REQUIRE(PushTogetherUntilContested(rig, stranger));
    rig.harness.serverPhysics.Teleport(stranger, Physics::Pose{glm::quat(1.f, 0.f, 0.f, 0.f), {-20.f, 0.f, 0.f}});

    const ClientId client = rig.harness.server.ClientIdOf(rig.harness.serverSide());
    const std::uint32_t dwell = ReplicationConfig{}.ownership.touchDwellTicks;
    std::uint32_t hostRunFor = 0;
    std::uint32_t changes = 0;
    ClientId simulator = InvalidClientId;
    for (std::uint32_t i = 0; i < dwell + kRoundTripTicks; ++i)
    {
        Tick(rig);
        const ClientId now = rig.harness.server.SimulatorOf(rig.crates[0]);
        hostRunFor += now.IsValid() ? 0u : 1u;
        changes += now == simulator ? 0u : 1u;
        simulator = now;
    }
    CHECK(hostRunFor + 1u >= dwell);
    CHECK(simulator == client);
    CHECK(changes == 1u);
    CHECK(rig.harness.client.Simulates(rig.crateMirrors[0]));
    CHECK_FALSE(rig.harness.client.Predicts(rig.crateMirrors[0]));
}

TEST_CASE("a crate given to the client to predict moves ahead of its character on the client, and the host agrees")
{
    PredictionRig rig(RigLevel{.crates = {kCrateAhead}});
    rig.move = glm::vec3{0.f};
    GiveCrate(rig);
    for (std::uint32_t i = 0; i < kAgreeingTicks; ++i)
    {
        Tick(rig);
    }

    rig.move = glm::vec3{1.f, 0.f, 0.f};
    const ECS::Entity crate = rig.crateMirrors[0];
    const glm::vec3 start = rig.harness.clientScene.Get<ECS::Transform>(crate)->position;
    glm::vec3 last = start;
    float mostBack = 0.f;
    for (std::uint32_t i = 0; i < kPushTicks; ++i)
    {
        Tick(rig);
        const glm::vec3 now = rig.harness.clientScene.Get<ECS::Transform>(crate)->position;
        mostBack = std::max(mostBack, last.x - now.x);
        last = now;
    }

    const ReplicationClient::EntityPredictionCounts *counts =
        rig.harness.client.PredictionCountsOf(rig.harness.client.NetIdOf(crate));
    REQUIRE(counts != nullptr);
    REQUIRE(last.x > start.x + kPushedFar);
    REQUIRE(counts->physics.compares > 0u);
    const ReplicationClient::EntityPredictionCounts *own =
        rig.harness.client.PredictionCountsOf(rig.harness.client.NetIdOf(rig.mirror));
    REQUIRE(own != nullptr);
    CHECK(own->physics.corrections == 0u);
    CHECK(counts->physics.corrections <= kMostPushCorrections);
    CHECK(mostBack < kMostBackPerTick);
    CHECK_FALSE(rig.harness.server.SimulatorOf(rig.crates[0]).IsValid());
}

TEST_CASE("a predicted crate the host moves where the client did not is put where the host had it, in one rollback, "
          "eased")
{
    PredictionRig rig(RigLevel{.crates = {kCrateAside}});
    rig.move = glm::vec3{0.f};
    GiveCrate(rig);
    for (std::uint32_t i = 0; i < kAgreeingTicks; ++i)
    {
        Tick(rig);
    }

    const glm::vec3 shove{0.f, 0.f, kHostShove};
    const Physics::Pose hostPose = rig.harness.serverPhysics.GetBodyPose(rig.crates[0]);
    rig.harness.serverPhysics.ApplyCorrection(rig.crates[0],
                                              Physics::Pose{hostPose.rotation, hostPose.position + shove},
                                              Physics::BodyState{});
    const std::uint32_t replaysBefore = rig.replays;
    glm::vec3 before{0.f};
    for (std::uint32_t i = 0; i < kAgreeingTicks && rig.replays == replaysBefore; ++i)
    {
        rig.StepBoth();
        before = rig.harness.clientScene.Get<ECS::Transform>(rig.crateMirrors[0])->position;
        rig.Deliver();
        rig.ReplayPending();
    }
    REQUIRE(rig.replays == replaysBefore + 1u);

    const ReplicationClient::Confirmed *host =
        rig.harness.client.ConfirmedOf(rig.harness.server.NetIdOf(rig.crates[0]));
    REQUIRE(host != nullptr);
    CHECK(glm::length(rig.harness.clientPhysics.GetBodyPose(rig.crateMirrors[0]).position - host->body.position) <
          kReplayedNear);

    // Eased across rather than jumped: one frame later it is drawn nearer
    // where it was than where it went.
    rig.harness.client.SmoothView(0.0, kFixedStep);
    CHECK(glm::length(RenderedPosition(rig.harness.clientScene, rig.crateMirrors[0]) - before) < kHostShove / 2.f);
}

TEST_CASE("a rollback of the client's character leaves a predicted crate the host agreed with where it was")
{
    // The crate is falling, so where it was when the rollback goes back to is
    // far from where it is: put back there, and never solved again, it would
    // hang a round trip behind for good.
    PredictionRig rig(RigLevel{.crates = {kCrateAside}});
    rig.move = glm::vec3{0.f};
    Physics::Pose high = rig.harness.serverPhysics.GetBodyPose(rig.crates[0]);
    high.position.y = kCrateFallingHeight;
    rig.harness.serverPhysics.ApplyCorrection(rig.crates[0], high, Physics::BodyState{});
    for (std::uint32_t i = 0; i < kRoundTripTicks; ++i)
    {
        Tick(rig);
    }
    GiveCrate(rig);
    for (std::uint32_t i = 0; i < kAgreeingTicks; ++i)
    {
        Tick(rig);
    }
    REQUIRE(rig.harness.serverPhysics.GetBodyState(rig.crates[0]).linearVelocity.y < 0.f);
    // Its first record on this client's timeline is corrected once, as the
    // host's word for that tick arrives; the rollback below must add none.
    const NetId crateId = rig.harness.server.NetIdOf(rig.crates[0]);
    REQUIRE(rig.harness.client.PredictionCountsOf(crateId) != nullptr);
    const std::uint64_t correctionsBefore = rig.harness.client.PredictionCountsOf(crateId)->physics.corrections;

    const glm::vec3 shove{0.f, 0.f, kHostShove};
    const Physics::Pose hostPose = rig.harness.serverPhysics.GetBodyPose(rig.character);
    rig.harness.serverPhysics.ApplyCorrection(rig.character,
                                              Physics::Pose{hostPose.rotation, hostPose.position + shove},
                                              Physics::BodyState{});
    const std::uint32_t replaysBefore = rig.replays;
    glm::vec3 before{0.f};
    glm::vec3 after{0.f};
    for (std::uint32_t i = 0; i < kAgreeingTicks && rig.replays == replaysBefore; ++i)
    {
        rig.StepBoth();
        rig.Deliver();
        before = rig.harness.clientPhysics.GetBodyPose(rig.crateMirrors[0]).position;
        rig.ReplayPending();
        after = rig.harness.clientPhysics.GetBodyPose(rig.crateMirrors[0]).position;
    }
    REQUIRE(rig.replays == replaysBefore + 1u);
    REQUIRE(rig.harness.client.PredictionCountsOf(crateId)->physics.corrections == correctionsBefore);
    CHECK(glm::length(after - before) < kReplayedNear);
}

TEST_CASE("a predicted body's angular velocity differs past its tolerance, and agrees within it")
{
    const ViewSmoothing smoothing;
    const BodyState mine;
    BodyState host = mine;
    host.angularVelocity = glm::vec3{0.f, smoothing.predictionAngularVelocityTolerance + AngularVelocityStep(), 0.f};
    CHECK_FALSE(PredictedBodyDifference(mine, host, smoothing).empty());
    host.angularVelocity = glm::vec3{0.f, smoothing.predictionAngularVelocityTolerance - AngularVelocityStep(), 0.f};
    CHECK(PredictedBodyDifference(mine, host, smoothing).empty());
}

TEST_CASE("a body the host's game gives a client to predict is never given to one to simulate")
{
    PredictionRig rig(RigLevel{.crates = {kCrateAhead}});
    rig.move = glm::vec3{0.f};
    GiveCrate(rig);
    rig.move = glm::vec3{1.f, 0.f, 0.f};
    std::uint32_t given = 0;
    for (std::uint32_t i = 0; i < kLongPushTicks; ++i)
    {
        Tick(rig);
        given += rig.harness.server.SimulatorOf(rig.crates[0]).IsValid() ? 1u : 0u;
    }
    CHECK(given == 0u);
}
