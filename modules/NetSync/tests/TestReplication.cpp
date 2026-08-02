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

#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Net/NetTransport.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/NetSync/Replication.hpp>
#include <Assisi/NetSync/TestNetComponents.hpp>

#include <chrono>
#include <cmath>
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

    /// @p deferHandshake models an application that has a world to build before
    /// it may answer — the editor's join. Off by default, which is every other
    /// case here.
    explicit Harness(ReplicationConfig config = {}, bool deferHandshake = false, LevelIdentity level = {})
        : pair(transport.CreateLoopbackPair()), server(transport, serverScene, /*physics=*/nullptr, config),
          client(transport, clientScene, pair.second)
    {
        client.SetDeferHandshake(deferHandshake);
        server.SetLevelIdentity(std::move(level));
        server.AddConnection(pair.first);
    }

    [[nodiscard]] Net::ConnectionId serverSide() const { return pair.first; }
    [[nodiscard]] Net::ConnectionId clientSide() const { return pair.second; }

    /// While set, client→server messages (acks, input) are buffered instead of
    /// delivered — the only way to put an ack *in flight across* a server-side
    /// event, which is what the keyframe sweep's ring-clear has to survive.
    bool holdClientMessages = false;

    /// While set, server→client messages are dropped on the floor. Together with
    /// the above this makes the two directions independently stoppable, which is
    /// what it takes to test a rule about ordering rather than about delivery.
    bool dropServerMessages = false;

    std::vector<std::vector<std::byte>> heldClientMessages;

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
            {
                if (holdClientMessages)
                    heldClientMessages.emplace_back(event.payload.begin(), event.payload.end());
                else
                    server.HandleMessage(serverSide(), event.payload);
            }
            else if (event.connection == clientSide() && !dropServerMessages)
            {
                client.HandleMessage(event.payload);
            }
        }

        server.Tick(tick++);
    }

    /// Deliver everything Step() held, oldest first.
    void ReleaseHeldMessages()
    {
        holdClientMessages = false;
        for (const std::vector<std::byte> &payload : heldClientMessages)
            server.HandleMessage(serverSide(), payload);
        heldClientMessages.clear();
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

TEST_CASE("the handshake carries which level the host is running, and its content hash")
{
    LevelIdentity level;
    level.addressing  = LevelAddressing::Virtual;
    level.path        = "levels/Materials.alvl";
    level.contentHash = 0xFEEDFACECAFEBEEDull;

    Harness harness(ReplicationConfig{}, /*deferHandshake=*/true, level);
    harness.Step(4);

    // Deferred: connected, told which level, and deliberately not synchronized.
    // A client that answered here would start receiving snapshots against a
    // world it has not built, mapping the host's NetIds onto whatever local
    // entities happen to occupy those slots.
    REQUIRE(harness.client.IsAwaitingLevel());
    CHECK_FALSE(harness.client.IsSynchronized());

    const ServerHello &hello = harness.client.Handshake();
    CHECK(hello.level.addressing == LevelAddressing::Virtual);
    CHECK(hello.level.path == "levels/Materials.alvl");
    CHECK(hello.level.contentHash == 0xFEEDFACECAFEBEEDull);
    CHECK(hello.tickRateHz == harness.server.Config().tickRateHz);

    // Nothing arrives while it holds off, however long the world runs.
    SpawnReplicated(harness.serverScene, {1.f, 2.f, 3.f});
    harness.Step(20);
    CHECK(harness.client.ReplicatedEntityCount() == 0);
    CHECK(harness.client.SnapshotsApplied() == 0);

    harness.client.ConfirmLevelReady();
    harness.Step(12);

    CHECK(harness.client.IsSynchronized());
    CHECK_FALSE(harness.client.IsAwaitingLevel());
    CHECK(harness.client.ReplicatedEntityCount() == 1);
    CHECK(Converged(harness));
}

TEST_CASE("a host with no level advertises none, and an aborted join says why")
{
    Harness harness(ReplicationConfig{}, /*deferHandshake=*/true);
    harness.Step(4);

    REQUIRE(harness.client.IsAwaitingLevel());
    // The default. An editor client treats it as a clean abort rather than
    // guessing at a world it cannot build.
    CHECK(harness.client.Handshake().level.addressing == LevelAddressing::None);

    harness.client.AbortJoin("the host is not running a level file");
    CHECK_FALSE(harness.client.IsAwaitingLevel());
    CHECK_FALSE(harness.client.IsSynchronized());
    CHECK(harness.client.RejectMessage() == "the host is not running a level file");

    harness.Step(20);
    CHECK(harness.client.SnapshotsApplied() == 0);
}

TEST_CASE("message framing is inside the handshake hash, not just the component table")
{
    // The component table cannot see a field added to ServerHello or a new
    // section in a snapshot, and two builds that disagree about either would
    // pair up and then misparse each other silently.
    CHECK(NetProtocolHash() != Core::Reflect::ProtocolHash());
    CHECK(NetProtocolHash() == NetProtocolHash());
    CHECK(NetProtocolSummary().find("net=") != std::string::npos);
}

TEST_CASE("replication policy is part of the protocol description")
{
    // The successor to P0's hash pin, which has been retired now that it has
    // done its job. That pin held one measured constant (6593563864785826454)
    // across the ACOMP(replicated) -> ACOMP(replicable) rename, proving the
    // rename did not disturb the *emitted* layout text and therefore could not
    // repartition deployed builds into incompatible pairs. P2a then moved the
    // hash deliberately, by giving `Replicated` its exclusion mask.
    //
    // Carrying the constant forward would have been worse than useless: it is
    // sensitive to every reflected component in this binary, so any unrelated
    // addition trips it, and the reflex that teaches is "bump the number" — the
    // opposite of the scrutiny a protocol change deserves. What is worth pinning
    // is the *property*, and it needs no magic number.
    const std::string description = Core::Reflect::ProtocolLayoutDescription();

    // The marker's policy field is inside the hash, so two builds that disagree
    // about what an entity may withhold refuse to pair rather than silently
    // sending each other different component sets.
    CHECK(description.find("excluded") != std::string::npos);
    // ...and the capability flag still is too — the v4 R1 decision this all
    // rests on. (TestBinaryCodec proves flipping it changes the hash; this
    // proves the real registry actually carries it.)
    CHECK(description.find(" replicated") != std::string::npos);
    CHECK(NetProtocolHash() == NetProtocolHash());
}

TEST_CASE("the structure revision moves when the mirrored world's shape does, and rests when it does not")
{
    Harness harness;
    harness.Step(4);

    const std::uint64_t atRest = harness.client.StructureRevision();
    harness.Step(20);
    // An idle world writes nothing, so nothing needs re-resolving.
    CHECK(harness.client.StructureRevision() == atRest);

    const ECS::Entity entity = SpawnReplicated(harness.serverScene, {0.f, 0.f, 0.f});
    harness.Step(12);
    const std::uint64_t afterSpawn = harness.client.StructureRevision();
    CHECK(afterSpawn > atRest);

    // A component arriving is a shape change too: it is the case that matters,
    // since a MeshRenderer's resolved GPU pointers are derived from data the
    // wire just wrote and nothing else in a frame loop knows to look.
    (void)harness.serverScene.Add<Test::Health>(entity, Test::Health{5, 0});
    harness.Step(12);
    const std::uint64_t afterComponent = harness.client.StructureRevision();
    CHECK(afterComponent > afterSpawn);

    harness.serverScene.Destroy(entity);
    harness.serverScene.FlushDestroyed();
    harness.Step(12);
    CHECK(harness.client.StructureRevision() > afterComponent);
}

TEST_CASE("a component type that is not ACOMP(replicable) never crosses the wire")
{
    Harness harness;
    harness.Step(4);

    const ECS::Entity entity = SpawnReplicated(harness.serverScene, {1.f, 0.f, 0.f});
    (void)harness.serverScene.Add<Test::Health>(entity, Test::Health{42, 0});
    (void)harness.serverScene.Add<Test::LocalOnly>(entity, Test::LocalOnly{9});

    harness.Step(12);

    const ECS::Entity mirror = harness.client.EntityOf(harness.server.NetIdOf(entity));
    REQUIRE(mirror != ECS::NullEntity);

    // The marked component arrives...
    REQUIRE(harness.clientScene.Get<Test::Health>(mirror) != nullptr);
    CHECK(harness.clientScene.Get<Test::Health>(mirror)->value == 42);

    // ...and the unmarked one does not exist on the client at all. Before wire
    // gating this was the other way round for *every* serializable component,
    // which is how a marked entity shipped a Camera that could take over the
    // receiving client's view.
    CHECK(harness.clientScene.Get<Test::LocalOnly>(mirror) == nullptr);

    // Mutating it later is still nobody else's business.
    harness.serverScene.GetMut<Test::LocalOnly>(entity)->value = 11;
    harness.Step(12);
    CHECK(harness.clientScene.Get<Test::LocalOnly>(mirror) == nullptr);
    CHECK(harness.client.SnapshotsRejected() == 0);
}

TEST_CASE("a norep field holds its client-side default while its siblings update")
{
    Harness harness;
    harness.Step(4);

    const ECS::Entity entity = SpawnReplicated(harness.serverScene, {0.f, 0.f, 0.f});
    (void)harness.serverScene.Add<Test::Health>(entity, Test::Health{75, 1234});
    harness.Step(12);

    const ECS::Entity mirror = harness.client.EntityOf(harness.server.NetIdOf(entity));
    REQUIRE(mirror != ECS::NullEntity);
    const Test::Health *replica = harness.clientScene.Get<Test::Health>(mirror);
    REQUIRE(replica != nullptr);
    CHECK(replica->value == 75);
    CHECK(replica->secret == Test::Health{}.secret); // its own default, never the server's

    // Both fields change; only one is on the wire. Nothing about `secret` is
    // recoverable from the packet, so the client's copy cannot drift toward the
    // server's value by accident.
    {
        Test::Health *authoritative = harness.serverScene.GetMut<Test::Health>(entity);
        REQUIRE(authoritative != nullptr);
        authoritative->value  = 30;
        authoritative->secret = 4321;
    }
    harness.Step(12);

    CHECK(harness.clientScene.Get<Test::Health>(mirror)->value == 30);
    CHECK(harness.clientScene.Get<Test::Health>(mirror)->secret == Test::Health{}.secret);
    CHECK(harness.client.SnapshotsRejected() == 0);
}

TEST_CASE("a norep field still round-trips to disk")
{
    // norep is a *wire* exclusion, not a serialization one. The JSON codec is
    // untouched, which is what makes it usable for authored server-side data
    // rather than only for runtime scratch (that is what transient is for).
    const Core::Reflect::ComponentMeta *meta =
        Core::Reflect::ComponentRegistry::Instance().ById(
            Core::Reflect::ComponentRegistry::Instance().IdOf(typeid(Test::Health)));
    REQUIRE(meta != nullptr);
    REQUIRE(meta->replicable);

    const Test::Health   source{55, 8888};
    const nlohmann::json json = meta->serialize(&source);
    CHECK(json.contains("value"));
    CHECK(json.contains("secret"));

    ECS::Scene        scene;
    const ECS::Entity entity = scene.Create();
    meta->addToScene(&scene, entity.index, entity.generation, json);

    const Test::Health *loaded = scene.Get<Test::Health>(entity);
    REQUIRE(loaded != nullptr);
    CHECK(loaded->value == 55);
    CHECK(loaded->secret == 8888);
}

TEST_CASE("a replicated component that never said `tracked` still deltas after spawn")
{
    // ACOMP(replicable) implies ACOMP(tracked), and this is why: an untracked
    // pool has no change-tick lane, ChangeTickById returns 0, and 0 reads as
    // "unchanged" — so the component would transmit once at spawn and then go
    // permanently silent no matter what the server did to it. MeshRenderer and
    // Name were both live instances of that bug; Test::Health stands in for them
    // here because NetSync deliberately does not link Runtime.
    Harness harness;
    harness.Step(4);

    const ECS::Entity entity = SpawnReplicated(harness.serverScene, {0.f, 0.f, 0.f});
    (void)harness.serverScene.Add<Test::Health>(entity, Test::Health{1, 0});
    harness.Step(12);

    const ECS::Entity mirror = harness.client.EntityOf(harness.server.NetIdOf(entity));
    REQUIRE(mirror != ECS::NullEntity);
    REQUIRE(harness.clientScene.Get<Test::Health>(mirror)->value == 1);

    for (int32_t i = 2; i <= 5; ++i)
    {
        harness.serverScene.GetMut<Test::Health>(entity)->value = i;
        harness.Step(12);
        CHECK(harness.clientScene.Get<Test::Health>(mirror)->value == i);
    }
}

TEST_CASE("a mirror is tagged Mirrored; the authoritative entity is not")
{
    Harness harness;
    harness.Step(4);

    const ECS::Entity entity = SpawnReplicated(harness.serverScene, {1.f, 2.f, 3.f});
    harness.Step(12);

    const ECS::Entity mirror = harness.client.EntityOf(harness.server.NetIdOf(entity));
    REQUIRE(mirror != ECS::NullEntity);

    // The tag is how everything downstream — the editor's read-only guard, the
    // inspector's replication-path line — tells "the server's crate" from "our
    // crate" without asking the session.
    CHECK(harness.clientScene.Has<Mirrored>(mirror));
    CHECK_FALSE(harness.serverScene.Has<Mirrored>(entity));

    // Transient: it registers for a ComponentId and nothing else, so it can
    // never be saved into a level file and reappear as authorable data.
    const Core::Reflect::ComponentMeta *meta = Core::Reflect::ComponentRegistry::Instance().ById(
        Core::Reflect::ComponentRegistry::Instance().IdOf(typeid(Mirrored)));
    REQUIRE(meta != nullptr);
    CHECK_FALSE(meta->serializable);
    CHECK_FALSE(meta->replicable);
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

TEST_CASE("an entity whose final change lands in a budget-starved snapshot still converges")
{
    // The bug this pins: the in-flight record's *global* scene change tick used
    // to become the connection's baseline when acked, including for entities the
    // budget had skipped. Their pending changes were then older than the
    // baseline, so "changed since" said no and they were never resent — stale
    // until something happened to touch them again. A continuously-moving world
    // re-stamps itself every tick, which is why nothing noticed; an entity whose
    // *last* change lands in a starved snapshot has nothing to re-stamp it.
    ReplicationConfig config;
    config.maxSnapshotBytes = 160; // room for a few transforms, not sixteen
    Harness harness(config);
    harness.Step(4);

    std::vector<ECS::Entity> entities;
    for (int i = 0; i < 16; ++i)
        entities.push_back(SpawnReplicated(harness.serverScene, {static_cast<float>(i), 0.f, 0.f}));

    harness.Step(300);
    REQUIRE(Converged(harness));

    // One burst of movement, far larger than a snapshot can carry, and then
    // stillness. Every entity's final change is inside that burst.
    for (std::size_t i = 0; i < entities.size(); ++i)
        harness.serverScene.GetMut<ECS::Transform>(entities[i])->position.y = 5.f + static_cast<float>(i);

    harness.Step(300);
    CHECK(Converged(harness));
    CHECK(harness.client.SnapshotsRejected() == 0);
}

TEST_CASE("the keyframe sweep re-anchors state that went wrong after it was delivered")
{
    // What the sweep is *for*. Delivery is already guaranteed — the acked
    // baseline resends every change until the client confirms it — so the
    // failure class left over is state that arrived, was acknowledged, and then
    // went wrong locally. Nothing about the delta path can notice that, because
    // from the server's side nothing has changed.
    ReplicationConfig config;
    config.keyframeIntervalTicks = 30;
    Harness harness(config);
    harness.Step(4);

    const ECS::Entity entity = SpawnReplicated(harness.serverScene, {1.f, 0.f, 0.f});
    (void)harness.serverScene.Add<Test::Health>(entity, Test::Health{42, 0});
    harness.Step(12);

    const ECS::Entity mirror = harness.client.EntityOf(harness.server.NetIdOf(entity));
    REQUIRE(mirror != ECS::NullEntity);
    REQUIRE(harness.clientScene.Get<Test::Health>(mirror)->value == 42);

    // Client-side damage the server has no way to know about.
    harness.clientScene.GetMut<Test::Health>(mirror)->value = 999;
    harness.Step(12);
    CHECK(harness.clientScene.Get<Test::Health>(mirror)->value == 999); // the delta path says nothing

    harness.Step(60); // past a sweep
    CHECK(harness.clientScene.Get<Test::Health>(mirror)->value == 42);

    const ConnectionDiagnostics *diagnostics = harness.server.Diagnostics(harness.serverSide());
    REQUIRE(diagnostics != nullptr);
    CHECK(diagnostics->keyframeSweeps > 0);
}

TEST_CASE("the sweep is off when the interval is zero")
{
    ReplicationConfig config;
    config.keyframeIntervalTicks = 0;
    Harness harness(config);
    harness.Step(4);

    const ECS::Entity entity = SpawnReplicated(harness.serverScene, {1.f, 0.f, 0.f});
    (void)harness.serverScene.Add<Test::Health>(entity, Test::Health{42, 0});
    harness.Step(12);

    const ECS::Entity mirror = harness.client.EntityOf(harness.server.NetIdOf(entity));
    REQUIRE(mirror != ECS::NullEntity);
    harness.clientScene.GetMut<Test::Health>(mirror)->value = 999;

    // "Wrong until the next sweep" becomes "wrong forever" — which is exactly
    // what the config comment warns about, pinned so the warning stays true.
    harness.Step(600);
    CHECK(harness.clientScene.Get<Test::Health>(mirror)->value == 999);
    CHECK(harness.server.Diagnostics(harness.serverSide())->keyframeSweeps == 0);
}

TEST_CASE("a late ack for a pre-sweep snapshot does not cancel the sweep")
{
    // The ring clear, which is the half of the sweep that is easy to leave out.
    // An ack for a snapshot sent *before* the sweep would otherwise fold that
    // record's per-entity ticks straight back into the baselines and silently
    // un-do the re-anchor for exactly the entities it covered.
    ReplicationConfig config;
    config.keyframeIntervalTicks = 60;
    Harness harness(config);
    harness.Step(4);

    const ECS::Entity entity = SpawnReplicated(harness.serverScene, {1.f, 0.f, 0.f});
    (void)harness.serverScene.Add<Test::Health>(entity, Test::Health{7, 0});
    harness.Step(20);

    const ECS::Entity mirror = harness.client.EntityOf(harness.server.NetIdOf(entity));
    REQUIRE(mirror != ECS::NullEntity);
    REQUIRE(harness.clientScene.Get<Test::Health>(mirror)->value == 7);

    // Damage the mirror, then stop the wire in stages. First hold the acks while
    // snapshots keep flowing, so a batch of *pre-sweep* acks accumulates (the
    // client only acks what it applies). The deltas are empty — the server's
    // copy has not changed — so nothing repairs the mirror on the way.
    harness.clientScene.GetMut<Test::Health>(mirror)->value = 999;
    harness.holdClientMessages = true;
    harness.Step(10);
    REQUIRE(!harness.heldClientMessages.empty());

    // Then drop snapshots too, and run to the sweep. Without this the sweep's
    // own full-state resend would reach the client before the late acks could do
    // any damage, and the test would pass for the wrong reason — it is a test
    // about ordering, so both directions have to be controlled.
    harness.dropServerMessages       = true;
    const std::uint64_t sweepsBefore = harness.server.Diagnostics(harness.serverSide())->keyframeSweeps;
    while (harness.server.Diagnostics(harness.serverSide())->keyframeSweeps == sweepsBefore)
        harness.Step();

    // Two more steps with snapshots still dropped, to discard the sweep's own
    // full-state resend: it went out on the very tick the sweep fired, so it is
    // still in the transport when the loop above exits.
    harness.Step(2);
    REQUIRE(harness.clientScene.Get<Test::Health>(mirror)->value == 999);

    // The late acks arrive after the sweep. Their records are gone from the
    // ring, so they find nothing and are ignored — had the ring survived, they
    // would have folded their per-entity ticks straight back in and the client
    // would be left holding 999 with nothing left to correct it.
    harness.ReleaseHeldMessages();
    harness.dropServerMessages = false;
    harness.Step(30);

    CHECK(harness.clientScene.Get<Test::Health>(mirror)->value == 7);
}

TEST_CASE("a despawned entity's baseline entry is gone once the despawn is acked")
{
    // NetIds are never reused, so a baseline map that is not pruned grows with
    // every entity that has *ever* replicated — unbounded under projectile-style
    // churn, and invisible until a long session runs out of memory.
    Harness harness;
    harness.Step(4);

    std::vector<ECS::Entity> entities;
    for (int i = 0; i < 6; ++i)
        entities.push_back(SpawnReplicated(harness.serverScene, {static_cast<float>(i), 0.f, 0.f}));
    harness.Step(20);

    const ConnectionDiagnostics *diagnostics = harness.server.Diagnostics(harness.serverSide());
    REQUIRE(diagnostics != nullptr);
    CHECK(diagnostics->baselineEntries == 6);

    for (const ECS::Entity entity : entities)
        harness.serverScene.Destroy(entity);
    harness.serverScene.FlushDestroyed();
    harness.Step(20);

    CHECK(diagnostics->baselineEntries == 0);
    CHECK(harness.client.ReplicatedEntityCount() == 0);

    // And a fresh one starts a fresh entry rather than inheriting a retired id's.
    SpawnReplicated(harness.serverScene, {9.f, 0.f, 0.f});
    harness.Step(20);
    CHECK(diagnostics->baselineEntries == 1);
    CHECK(Converged(harness));
}

TEST_CASE("under budget pressure, priority decides who is corrected more often — and nobody starves")
{
    // The Tribes-lineage accumulator. When bandwidth is not binding there is no
    // reason to correct less often than every snapshot tick, so this does
    // nothing; when it binds, fairness needs *some* order, and per-entity
    // priority makes that order an authored decision instead of whichever NetId
    // happened to be lowest. The debris pile yields to the door.
    ReplicationConfig config;
    config.maxSnapshotBytes      = 120; // room for a couple of transforms
    config.keyframeIntervalTicks = 0;   // a sweep would flatten the comparison
    Harness harness(config);
    harness.Step(4);

    std::vector<ECS::Entity> entities;
    for (int i = 0; i < 8; ++i)
        entities.push_back(SpawnReplicated(harness.serverScene, {static_cast<float>(i), 0.f, 0.f}));

    // First and last of the list, so neither can win by NetId order.
    harness.serverScene.GetMut<Replicated>(entities.front())->priority = 10.f;
    harness.serverScene.GetMut<Replicated>(entities.back())->priority  = 0.f; // the clamp's job

    harness.Step(300);
    REQUIRE(harness.client.ReplicatedEntityCount() == entities.size());

    // Keep the whole world moving and integrate how far each mirror lags. A
    // correction that arrives more often keeps a smaller error.
    std::vector<double> laggedError(entities.size(), 0.0);
    for (int step = 0; step < 400; ++step)
    {
        for (std::size_t i = 0; i < entities.size(); ++i)
            harness.serverScene.GetMut<ECS::Transform>(entities[i])->position.y = static_cast<float>(step) * 0.1f;

        harness.Step();

        for (std::size_t i = 0; i < entities.size(); ++i)
        {
            const ECS::Entity mirror = harness.client.EntityOf(harness.server.NetIdOf(entities[i]));
            if (mirror == ECS::NullEntity)
                continue;
            const ECS::Transform *truth = harness.serverScene.Get<ECS::Transform>(entities[i]);
            const ECS::Transform *shown = harness.clientScene.Get<ECS::Transform>(mirror);
            if (truth != nullptr && shown != nullptr)
                laggedError[i] += static_cast<double>(std::abs(truth->position.y - shown->position.y));
        }
    }

    CHECK(laggedError.front() < laggedError.back());

    // ...and the loser is behind, not abandoned. Only the entities that actually
    // went out reset their accumulators, so the ones that missed keep climbing
    // and eventually win a turn — which is why a priority of exactly 0.0 means
    // "last in line" rather than "never".
    for (std::size_t i = 0; i < entities.size(); ++i)
        harness.serverScene.GetMut<ECS::Transform>(entities[i])->position.y = 99.f;
    harness.Step(400);
    CHECK(Converged(harness));
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

TEST_CASE("interpolation renders between snapshots rather than stepping at the snapshot rate")
{
    // Snapshot every tick, so "the two most recently applied snapshots" are
    // unambiguously adjacent in the client's buffer. At the default 20 Hz a
    // step can straddle a snapshot boundary or not, and the test would be
    // asserting against timing it does not control.
    ReplicationConfig config;
    config.tickRateHz = 60;
    config.snapshotHz = 60;
    Harness harness(config);
    harness.Step(4);

    const ECS::Entity entity = SpawnReplicated(harness.serverScene, {0.f, 0.f, 0.f});
    harness.Step(6);

    const ECS::Entity mirror = harness.client.EntityOf(harness.server.NetIdOf(entity));
    REQUIRE(mirror != ECS::NullEntity);

    // Move it in a straight line and record what actually arrived, rather than
    // predicting it: the pipeline delay between "server sends" and "client
    // applies" is not something this test should be encoding.
    std::vector<std::pair<std::uint64_t, float>> samples;
    for (int i = 1; i <= 6; ++i)
    {
        ECS::Transform *transform = harness.serverScene.GetMut<ECS::Transform>(entity);
        REQUIRE(transform != nullptr);
        transform->position.x = static_cast<float>(i);
        harness.Step(1);

        const std::uint64_t applied = harness.client.LastAppliedTick();
        const float         shown   = harness.clientScene.Get<ECS::Transform>(mirror)->position.x;
        if (samples.empty() || samples.back().first != applied)
            samples.emplace_back(applied, shown);
    }
    REQUIRE(samples.size() >= 2);

    const auto [previousTick, previousX] = samples[samples.size() - 2];
    const auto [lastTick, lastX]         = samples.back();
    REQUIRE(lastTick > previousTick);
    REQUIRE(lastX != previousX);

    SUBCASE("halfway between two snapshots is halfway between two positions")
    {
        const double midpoint = (static_cast<double>(previousTick) + static_cast<double>(lastTick)) / 2.0;
        harness.client.Interpolate(midpoint);
        CHECK(harness.clientScene.Get<ECS::Transform>(mirror)->position.x ==
              doctest::Approx((previousX + lastX) / 2.f).epsilon(1e-3));
    }

    SUBCASE("exactly on a snapshot is exactly that snapshot")
    {
        harness.client.Interpolate(static_cast<double>(lastTick));
        CHECK(harness.clientScene.Get<ECS::Transform>(mirror)->position.x == doctest::Approx(lastX));

        harness.client.Interpolate(static_cast<double>(previousTick));
        CHECK(harness.clientScene.Get<ECS::Transform>(mirror)->position.x == doctest::Approx(previousX));
    }

    SUBCASE("past the newest sample holds still rather than extrapolating")
    {
        // A guess that turns out wrong has to be corrected with a visible snap.
        harness.client.Interpolate(static_cast<double>(lastTick) + 100.0);
        CHECK(harness.clientScene.Get<ECS::Transform>(mirror)->position.x == doctest::Approx(lastX));
    }

    SUBCASE("before the oldest sample shows the oldest, not garbage")
    {
        harness.client.Interpolate(0.0);
        const float shown = harness.clientScene.Get<ECS::Transform>(mirror)->position.x;
        CHECK(shown <= lastX);
        CHECK(std::isfinite(shown));
    }
}

TEST_CASE("the interpolation delay is two snapshot intervals of the server's rate")
{
    ReplicationConfig config;
    config.tickRateHz = 60;
    config.snapshotHz = 20;
    Harness harness(config);
    harness.Step(4);

    REQUIRE(harness.client.IsSynchronized());
    // 60/20 = 3 ticks per snapshot, so two intervals is 6 ticks.
    CHECK(harness.client.InterpolationDelayTicks() == doctest::Approx(6.0));
    CHECK(harness.client.RenderTimeFor(100.0) == doctest::Approx(94.0));
}

TEST_CASE("interpolating an entity with no history at all is harmless")
{
    Harness harness;
    harness.Step(4);
    // Nothing replicated, nothing buffered — this must be a no-op rather than
    // a walk off the end of an empty deque.
    harness.client.Interpolate(0.0);
    harness.client.Interpolate(1e9);
    CHECK(harness.client.ReplicatedEntityCount() == 0);
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
    ReplicationServer server(transport, serverScene, /*physics=*/nullptr, ReplicationConfig{});
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
