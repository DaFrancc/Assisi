/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestInstancePolicy.cpp
/// @brief Per-entity replication policy: `Replicated::excluded`.
///
/// The policy half of the capability/policy split. `ACOMP(replicable)` on a type
/// says it *can* cross the wire; the mask here decides, per entity, which of
/// those capabilities are actually used — so an engine module can no longer set
/// a game's network policy by editing one of its own headers.
///
/// Two properties carry most of the weight:
///
///  - **The default costs nothing.** An empty mask must produce byte-identical
///    output to having no policy mechanism at all, or every existing level pays
///    for a feature it does not use.
///  - **Both directions of a policy change work.** Excluding rides the removal
///    diff, which already existed. *Re*-including needs D11, the force-send,
///    because policy moving does not touch the component's change tick — the
///    ordinary delta gate would skip it until the next keyframe sweep.

#include <doctest/doctest.h>

#include <Assisi/Core/Reflect/ComponentMask.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Net/NetTransport.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>
#include <Assisi/NetSync/ReplicationConfig.hpp>
#include <Assisi/NetSync/ReplicationProviders.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>
#include <Assisi/NetSync/TestNetComponents.hpp>

#include <cstdint>
#include <typeindex>
#include <vector>

using namespace Assisi;
using namespace Assisi::NetSync;

namespace
{

struct Harness
{
    Net::NetTransport transport;
    ECS::Scene serverScene;
    ECS::Scene clientScene;

    std::pair<Net::ConnectionId, Net::ConnectionId> pair;

    ReplicationServer server;
    ReplicationClient client;

    std::uint64_t tick = 0;

    explicit Harness(ReplicationConfig config = {})
        : pair(transport.CreateLoopbackPair()), server(transport, serverScene, /*physics=*/ nullptr, config),
        client(transport, clientScene, pair.second)
    {
        server.SetContentSetHash(0);
        client.SetContentSetHash(0);
        server.AddConnection(pair.first);
    }

    /// One full network step: deliver everything in flight, then advance the
    /// server a tick. Mirrors the shape TestReplication's harness uses.
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

        server.Tick(tick++);
    }

    void Step(std::uint32_t times)
    {
        for (std::uint32_t i = 0; i < times; ++i)
            Step();
    }

    [[nodiscard]] std::uint64_t BytesSent() const
    {
        const ConnectionDiagnostics *diagnostics = server.Diagnostics(pair.first);
        return diagnostics == nullptr ? 0 : diagnostics->bytesSent;
    }
};

std::size_t OrdinalOf(const std::type_info &type)
{
    const Core::Reflect::ComponentRegistry &registry = Core::Reflect::ComponentRegistry::Instance();
    return registry.ReplicableOrdinalOf(registry.IdOf(std::type_index(type)));
}

/// An entity carrying a Transform and a Health — two replicable components, so
/// a test can exclude one and watch the other carry on. (Runtime's Name and
/// MeshRenderer would make a richer fixture, but this suite deliberately does
/// not link Runtime; two is enough to prove exclusion is per component rather
/// than a switch on the entity.)
ECS::Entity SpawnRich(ECS::Scene &scene, Core::Reflect::ComponentMask excluded = {})
{
    const ECS::Entity entity = scene.Create();
    (void)scene.Add<ECS::Transform>(entity, ECS::Transform{});
    (void)scene.Add<Test::Health>(entity, Test::Health{100, 0});

    Replicated marker;
    marker.excluded = excluded;
    (void)scene.Add<Replicated>(entity, marker);
    return entity;
}

ECS::Entity SoleMirror(const Harness &harness)
{
    return harness.client.EntityOf(harness.server.NetIdOf(
                                       [&]
        {
            for (auto [entity, marker] : const_cast<ECS::Scene &>(harness.serverScene).Query<Replicated>())
            {
                (void)marker;
                return entity;
            }
            return ECS::NullEntity;
        }()));
}

} // namespace

TEST_CASE("an empty policy sends exactly what no policy would")
{
    // The regression net for the whole feature: the default must be free. If
    // this ever diverges, every existing level starts paying for a mechanism it
    // does not use.
    Harness harness;
    SpawnRich(harness.serverScene);
    harness.Step(12);

    const ECS::Entity mirror = SoleMirror(harness);
    REQUIRE(mirror != ECS::NullEntity);
    CHECK(harness.clientScene.Get<ECS::Transform>(mirror) != nullptr);
    CHECK(harness.clientScene.Get<Test::Health>(mirror) != nullptr);
    CHECK(harness.client.SnapshotsRejected() == 0);
}

TEST_CASE("an excluded component never reaches a client that never had it")
{
    Core::Reflect::ComponentMask excluded;
    excluded.Set(OrdinalOf(typeid(Test::Health)));

    Harness harness;
    SpawnRich(harness.serverScene, excluded);
    harness.Step(12);

    const ECS::Entity mirror = SoleMirror(harness);
    REQUIRE(mirror != ECS::NullEntity);
    // The sibling is unaffected — exclusion is per component, not a switch on
    // the entity.
    CHECK(harness.clientScene.Get<ECS::Transform>(mirror) != nullptr);
    CHECK(harness.clientScene.Get<Test::Health>(mirror) == nullptr);
}

TEST_CASE("excluding a component mid-session removes it from the mirror")
{
    // Rides the removal diff, which already existed: the component drops out of
    // the entity's presence list, the diff against the acked slice notices, and
    // a removal goes out. No new wire machinery.
    Harness harness;
    const ECS::Entity entity = SpawnRich(harness.serverScene);
    harness.Step(12);

    const ECS::Entity mirror = SoleMirror(harness);
    REQUIRE(mirror != ECS::NullEntity);
    REQUIRE(harness.clientScene.Get<Test::Health>(mirror) != nullptr);

    Replicated *marker = harness.serverScene.GetMut<Replicated>(entity);
    REQUIRE(marker != nullptr);
    marker->excluded.Set(OrdinalOf(typeid(Test::Health)));

    harness.Step(12);
    CHECK(harness.clientScene.Get<Test::Health>(mirror) == nullptr);
    CHECK(harness.clientScene.Get<ECS::Transform>(mirror) != nullptr);
}

TEST_CASE("re-including a component delivers it on the next snapshot, not the next sweep")
{
    // **The D11 test.** This is the one that fails without the force-send, and
    // it fails in the most misleading possible way: it passes if you wait long
    // enough, because the keyframe sweep eventually re-anchors everything.
    //
    // The mechanism: re-including changes the *policy*, not the component, so
    // the component's change tick still predates the client's baseline. The
    // ordinary delta gate asks "did this value change since the client last saw
    // it", which is the wrong question when what changed is whether the client
    // is allowed to have it at all. Without D11 the mirror stays short a
    // component the server believes it delivered, for up to a full sweep
    // interval.
    ReplicationConfig config;
    config.keyframeIntervalTicks = 0; // no sweep to rescue us — this must work on its own
    Harness harness(config);

    Core::Reflect::ComponentMask excluded;
    excluded.Set(OrdinalOf(typeid(Test::Health)));
    const ECS::Entity entity = SpawnRich(harness.serverScene, excluded);
    harness.Step(12);

    const ECS::Entity mirror = SoleMirror(harness);
    REQUIRE(mirror != ECS::NullEntity);
    REQUIRE(harness.clientScene.Get<Test::Health>(mirror) == nullptr);

    // Let the component go stale: change it, deliver nothing (it is excluded),
    // then re-include. Its change tick is now firmly in the past.
    {
        Test::Health *health = harness.serverScene.GetMut<Test::Health>(entity);
        REQUIRE(health != nullptr);
        health->value = 42;
    }
    harness.Step(12);
    REQUIRE(harness.clientScene.Get<Test::Health>(mirror) == nullptr);

    Replicated *marker = harness.serverScene.GetMut<Replicated>(entity);
    REQUIRE(marker != nullptr);
    marker->excluded = Core::Reflect::ComponentMask{};

    harness.Step(12);
    const Test::Health *received = harness.clientScene.Get<Test::Health>(mirror);
    REQUIRE(received != nullptr);
    // Full state, not a stale default: D11 sends the component's actual value.
    CHECK(received->value == 42);
}

TEST_CASE("an exclusion survives the keyframe sweep")
{
    // The sweep re-anchors from the empty baseline, which takes the full-state
    // path — a path that must consult policy too, or every sweep would leak the
    // components an entity spent the whole session declining to send.
    ReplicationConfig config;
    config.keyframeIntervalTicks = 8;
    Harness harness(config);

    Core::Reflect::ComponentMask excluded;
    excluded.Set(OrdinalOf(typeid(Test::Health)));
    SpawnRich(harness.serverScene, excluded);

    harness.Step(60); // several sweeps
    const ECS::Entity mirror = SoleMirror(harness);
    REQUIRE(mirror != ECS::NullEntity);
    CHECK(harness.clientScene.Get<Test::Health>(mirror) == nullptr);
    CHECK(harness.clientScene.Get<ECS::Transform>(mirror) != nullptr);
}

TEST_CASE("policy is read live, so an untracked write still takes effect")
{
    // `Replicated` is deliberately plain ACOMP() — not tracked — because the
    // server reads the mask fresh every snapshot and caches nothing. This pins
    // that: a write through the *non-stamping* path (Query, not GetMut) must
    // still be obeyed. If a cache is ever introduced, this test is what notices.
    Harness harness;
    SpawnRich(harness.serverScene);
    harness.Step(12);

    const ECS::Entity mirror = SoleMirror(harness);
    REQUIRE(mirror != ECS::NullEntity);
    REQUIRE(harness.clientScene.Get<Test::Health>(mirror) != nullptr);

    for (auto [entity, marker] : harness.serverScene.Query<Replicated>())
    {
        (void)entity;
        marker.excluded.Set(OrdinalOf(typeid(Test::Health)));
    }

    harness.Step(12);
    CHECK(harness.clientScene.Get<Test::Health>(mirror) == nullptr);
}

TEST_CASE("an entity may exclude a component another entity still sends")
{
    // Policy is per instance, which is the entire reason it lives on the marker
    // rather than on the type. Two entities of identical shape, different
    // policies, same session.
    Harness harness;

    Core::Reflect::ComponentMask excluded;
    excluded.Set(OrdinalOf(typeid(Test::Health)));

    const ECS::Entity quiet = SpawnRich(harness.serverScene, excluded);
    const ECS::Entity loud  = SpawnRich(harness.serverScene);
    harness.Step(16);

    const ECS::Entity quietMirror = harness.client.EntityOf(harness.server.NetIdOf(quiet));
    const ECS::Entity loudMirror  = harness.client.EntityOf(harness.server.NetIdOf(loud));
    REQUIRE(quietMirror != ECS::NullEntity);
    REQUIRE(loudMirror != ECS::NullEntity);

    CHECK(harness.clientScene.Get<Test::Health>(quietMirror) == nullptr);
    CHECK(harness.clientScene.Get<Test::Health>(loudMirror) != nullptr);
}

TEST_CASE("excluding a component stops costing bandwidth for it")
{
    // The point of the feature, stated as a measurement rather than a hope: a
    // component that changes every tick costs nothing once excluded.
    Harness harness;
    const ECS::Entity entity = SpawnRich(harness.serverScene);
    harness.Step(20);

    const auto churn = [&](std::uint32_t steps)
                       {
                           for (std::uint32_t i = 0; i < steps; ++i)
                           {
                               Test::Health *health = harness.serverScene.GetMut<Test::Health>(entity);
                               health->value        = static_cast<std::int32_t>(i);
                               harness.Step(1);
                           }
                       };

    const std::uint64_t before = harness.BytesSent();
    churn(40);
    const std::uint64_t withHealth = harness.BytesSent() - before;

    Replicated *marker = harness.serverScene.GetMut<Replicated>(entity);
    marker->excluded.Set(OrdinalOf(typeid(Test::Health)));
    harness.Step(12); // let the removal land and settle

    const std::uint64_t settled = harness.BytesSent();
    churn(40);
    const std::uint64_t withoutHealth = harness.BytesSent() - settled;

    CAPTURE(withHealth);
    CAPTURE(withoutHealth);
    CHECK(withoutHealth < withHealth);
}

// ── Game-scope policy (P3) ──────────────────────────────────────────────────
//
// The gate between a type's capability and an entity's own exclusion mask: a
// game says once that it never sends something, instead of saying it on every
// entity that happens to carry it. This is the direct answer to the incident
// that started the plan — an engine module marking Physics::Bounce replicable to
// serve one test level, and thereby setting policy for every game.

TEST_CASE("a game-vetoed component never reaches any client")
{
    ReplicationConfig config;
    config.neverReplicate = {"Health"};
    Harness harness(config);

    SpawnRich(harness.serverScene);
    harness.Step(16);

    const ECS::Entity mirror = SoleMirror(harness);
    REQUIRE(mirror != ECS::NullEntity);
    CHECK(harness.clientScene.Get<Test::Health>(mirror) == nullptr);
    // The veto is per component, not a switch on the session.
    CHECK(harness.clientScene.Get<ECS::Transform>(mirror) != nullptr);
}

TEST_CASE("a game veto outranks an entity that would happily send")
{
    // The two gates are an intersection, not alternatives: an entity with an
    // empty exclusion mask still cannot override its game.
    ReplicationConfig config;
    config.neverReplicate = {"Health"};
    Harness harness(config);

    const ECS::Entity entity = SpawnRich(harness.serverScene);
    REQUIRE(harness.serverScene.Get<Replicated>(entity)->excluded.Empty());

    harness.Step(16);
    CHECK(harness.clientScene.Get<Test::Health>(SoleMirror(harness)) == nullptr);
}

TEST_CASE("naming the same component in both gates is harmless")
{
    ReplicationConfig config;
    config.neverReplicate = {"Health"};
    Harness harness(config);

    Core::Reflect::ComponentMask excluded;
    excluded.Set(OrdinalOf(typeid(Test::Health)));
    SpawnRich(harness.serverScene, excluded);

    harness.Step(16);
    const ECS::Entity mirror = SoleMirror(harness);
    REQUIRE(mirror != ECS::NullEntity);
    CHECK(harness.clientScene.Get<Test::Health>(mirror) == nullptr);
    CHECK(harness.clientScene.Get<ECS::Transform>(mirror) != nullptr);
}

TEST_CASE("an unresolvable veto name changes nothing")
{
    // A typo must not quietly widen or narrow what a game sends. It warns (the
    // author is told) and is otherwise inert.
    ReplicationConfig config;
    config.neverReplicate = {"NoSuchComponentAnywhere"};
    Harness harness(config);

    SpawnRich(harness.serverScene);
    harness.Step(16);

    const ECS::Entity mirror = SoleMirror(harness);
    REQUIRE(mirror != ECS::NullEntity);
    CHECK(harness.clientScene.Get<Test::Health>(mirror) != nullptr);
    CHECK(harness.clientScene.Get<ECS::Transform>(mirror) != nullptr);
}

TEST_CASE("an empty veto list is exactly today's behaviour")
{
    ReplicationConfig config; // neverReplicate default-empty
    Harness harness(config);

    SpawnRich(harness.serverScene);
    harness.Step(16);

    const ECS::Entity mirror = SoleMirror(harness);
    REQUIRE(mirror != ECS::NullEntity);
    CHECK(harness.clientScene.Get<Test::Health>(mirror) != nullptr);
    CHECK(harness.clientScene.Get<ECS::Transform>(mirror) != nullptr);
}

TEST_CASE("a game veto does not renumber the ordinals level files are authored against")
{
    // The subtle one. Exclusion masks in level files index the *registry's*
    // replicable numbering, so if the game filter renumbered the server's view,
    // every authored exclusion would silently re-aim at a different component.
    ReplicationConfig config;
    config.neverReplicate = {"Health"};
    Harness harness(config);

    // Transform's ordinal is whatever the registry says, veto or no veto — and
    // excluding it must still exclude Transform rather than something else.
    Core::Reflect::ComponentMask excluded;
    excluded.Set(OrdinalOf(typeid(ECS::Transform)));
    SpawnRich(harness.serverScene, excluded);

    harness.Step(16);
    const ECS::Entity mirror = SoleMirror(harness);
    REQUIRE(mirror != ECS::NullEntity);
    CHECK(harness.clientScene.Get<ECS::Transform>(mirror) == nullptr);
}
