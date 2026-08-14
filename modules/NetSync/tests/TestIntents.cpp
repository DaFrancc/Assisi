/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestIntents.cpp
/// @brief The one door a client may speak through, and everything that turns it
/// away.
///
/// Every documented exploit in the RPC survey has the same shape: attacker-made
/// messages meeting hand-written parsing spread across many receive sites. So
/// the feature under test is not "intents work" — it is that there is exactly
/// one place they arrive, that the checks there run in an order where a flood
/// costs a comparison rather than a parse, and that each refusal is counted
/// separately, because "intents dropped" is not a diagnosis.
///
/// The host is tested alongside the clients on purpose. A listen server's player
/// is not a connection, so without deliberate work it would be the one
/// participant whose intents skipped the door.
///
/// See docs/replication-messaging-relevancy-plan-v1.md M4.

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

#include <cstdint>
#include <vector>

using namespace Assisi;
using namespace Assisi::NetSync;
using namespace Assisi::NetSync::Test;

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
        HandlerLog::Instance().Clear();
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

    [[nodiscard]] const ConnectionDiagnostics &Diagnostics() const { return *server.Diagnostics(pair.first); }

    /// Send @p intent and pump until the server has seen it.
    template <typename T>
    void Send(const T &intent)
    {
        REQUIRE(client.SendIntent(intent, tick));
        Step(2);
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

TEST_CASE("an intent reaches its handler with the sender the wire says it came from")
{
    Harness harness;
    harness.Step(4);
    REQUIRE(harness.client.IsSynchronized());

    harness.Send(TestPlaceMarker{ /*target=*/ 17, /*slot=*/ 3});

    CHECK(HandlerLog::Instance().placeMarkerCalls == 1);
    CHECK(HandlerLog::Instance().lastPlaceMarker.target == 17);
    CHECK(HandlerLog::Instance().lastPlaceMarker.slot == 3);

    // Never taken from the payload — a client that could name its own sender
    // could name anyone's. It is the connection the bytes arrived on.
    CHECK(HandlerLog::Instance().lastSender == harness.server.ClientIdOf(harness.serverSide()));
    CHECK(harness.Diagnostics().intentsAccepted == 1);
}

TEST_CASE("an unreliable intent takes the same door as a reliable one")
{
    // Reliability is about delivery, not about belief: both forms are equally
    // untrusted and both pass every check.
    Harness harness;
    harness.Step(4);

    harness.Send(TestPing{ /*x=*/ 1.f, /*y=*/ 2.f});

    CHECK(HandlerLog::Instance().pingCalls == 1);
    CHECK(HandlerLog::Instance().lastPing.x == doctest::Approx(1.f));
    CHECK(harness.Diagnostics().intentsAccepted == 1);
}

TEST_CASE("an out-of-range field is rejected, not clamped")
{
    // The contrast with the input path is the point. Input clamps because a
    // stick can legitimately saturate; an intent field outside its declared
    // range means the client is lying or the two builds disagree, and clamping
    // would convert a detectable attack into a silently accepted one.
    Harness harness;
    harness.Step(4);

    harness.Send(TestPing{ /*x=*/ 5000.f, /*y=*/ 0.f});

    CHECK(HandlerLog::Instance().pingCalls == 0);
    CHECK(harness.Diagnostics().intentsOutOfRange == 1);
    CHECK(harness.Diagnostics().intentsAccepted == 0);
    // Not clamped to the bound and delivered — the handler never ran at all.
    CHECK(HandlerLog::Instance().lastPing.x == doctest::Approx(0.f));

    // The connection survives: a rejected intent is a refusal, not a fault.
    harness.Send(TestPing{ /*x=*/ 1.f, /*y=*/ 1.f});
    CHECK(HandlerLog::Instance().pingCalls == 1);
}

TEST_CASE("an intent about an entity the sender does not control is dropped and counted")
{
    Harness harness;
    harness.Step(4);

    const ECS::Entity pawn = SpawnReplicated(harness.serverScene);
    harness.Step(8);

    const NetId netId  = harness.server.NetIdOf(pawn);
    const ECS::Entity mirror = harness.client.EntityOf(netId);
    REQUIRE(mirror != ECS::NullEntity);

    // Nobody controls it yet, so the claim is false.
    harness.Send(TestMovePawn{mirror, ECS::NullEntity, /*mode=*/ 1});
    CHECK(HandlerLog::Instance().movePawnCalls == 0);
    CHECK(harness.Diagnostics().intentsNotYours == 1);

    // Now it is theirs, and the same message goes through.
    harness.server.SetControl(pawn, harness.server.ClientIdOf(harness.serverSide()));
    harness.Step(4);
    harness.Send(TestMovePawn{mirror, ECS::NullEntity, /*mode=*/ 1});
    CHECK(HandlerLog::Instance().movePawnCalls == 1);
    CHECK(harness.Diagnostics().intentsNotYours == 1); // unchanged
}

TEST_CASE("only the field marked as the intent's subject is control-checked")
{
    // A client naming something it does not own is ordinary — "shoot at that" —
    // while a client acting *through* something it does not own is not. If the
    // dispatch site checked every entity reference, the first would be
    // impossible to express.
    Harness harness;
    harness.Step(4);

    const ECS::Entity pawn      = SpawnReplicated(harness.serverScene);
    const ECS::Entity somebodys = SpawnReplicated(harness.serverScene);
    harness.Step(8);

    harness.server.SetControl(pawn, harness.server.ClientIdOf(harness.serverSide()));
    harness.server.SetControl(somebodys, ClientId{999}); // emphatically not ours
    harness.Step(4);

    const ECS::Entity myMirror    = harness.client.EntityOf(harness.server.NetIdOf(pawn));
    const ECS::Entity theirMirror = harness.client.EntityOf(harness.server.NetIdOf(somebodys));
    REQUIRE(myMirror != ECS::NullEntity);
    REQUIRE(theirMirror != ECS::NullEntity);

    harness.Send(TestMovePawn{myMirror, theirMirror, /*mode=*/ 2});

    CHECK(HandlerLog::Instance().movePawnCalls == 1);
    CHECK(harness.Diagnostics().intentsNotYours == 0);
    // ...and the reference survived the trip as a local handle on the server.
    CHECK(HandlerLog::Instance().lastMovePawn.pawn == pawn);
    CHECK(HandlerLog::Instance().lastMovePawn.target == somebodys);
}

TEST_CASE("an event sent as an intent is turned away at the direction check")
{
    // The static_assert on SendIntent makes this uncompilable through the normal
    // API, so the packet is forged by hand — which is exactly what a hostile
    // client would do, and the reason the check exists on the receive side at
    // all rather than only at the send site.
    Harness harness;
    harness.Step(4);

    const Core::Reflect::MessageRegistry &registry = Core::Reflect::MessageRegistry::Instance();
    const Core::Reflect::MessageMeta *announce = registry.Find("TestAnnounce");
    REQUIRE(announce != nullptr);

    const TestAnnounce forged{ /*round=*/ 7};
    Core::BitWriter writer;
    WriteMessageType(MessageType::Intent, writer);
    writer.WriteVarUInt64(harness.tick);
    REQUIRE(Core::Reflect::WriteMessage(*announce, &forged, writer));

    harness.server.HandleMessage(harness.serverSide(), writer.Data());

    CHECK(HandlerLog::Instance().announceCalls == 0);
    CHECK(harness.Diagnostics().intentsWrongWay == 1);
}

TEST_CASE("a flood is limited per message type, before the payload is decoded")
{
    ReplicationConfig config;
    config.maxIntentsPerTypePerSecond = 4;

    Harness harness(config);
    harness.Step(4);

    // Twenty pings inside one rate window. Four get through.
    for (std::uint32_t i = 0; i < 20; ++i)
        REQUIRE(harness.client.SendIntent(TestPing{}, harness.tick));
    harness.Step(2);

    CHECK(HandlerLog::Instance().pingCalls == 4);
    CHECK(harness.Diagnostics().intentsRateLimited == 16);

    // ...and a *different* type still has its own budget, so a client spamming
    // one message cannot squeeze out its own use of another.
    REQUIRE(harness.client.SendIntent(TestPlaceMarker{1, 1}, harness.tick));
    harness.Step(2);
    CHECK(HandlerLog::Instance().placeMarkerCalls == 1);
}

TEST_CASE("a stale or future-dated intent is dropped by the tick window")
{
    ReplicationConfig config;
    config.intentStaleWindowTicks = 10;
    config.intentLeadWindowTicks  = 5;

    Harness harness(config);
    harness.Step(60); // get the server's tick well past the stale window

    // Far in the past: a late unreliable intent must not time-travel into a
    // world that has moved on.
    REQUIRE(harness.client.SendIntent(TestPing{}, /*clientTick=*/ 1));
    harness.Step(2);
    CHECK(HandlerLog::Instance().pingCalls == 0);
    CHECK(harness.Diagnostics().intentsStale == 1);

    // Far in the future: a tick the server has not reached and will not for
    // seconds is a claim it cannot act on.
    REQUIRE(harness.client.SendIntent(TestPing{}, harness.tick + 5000));
    harness.Step(2);
    CHECK(HandlerLog::Instance().pingCalls == 0);
    CHECK(harness.Diagnostics().intentsStale == 2);

    // ...and a current one goes through, so the window is a window rather than
    // a wall.
    REQUIRE(harness.client.SendIntent(TestPing{}, harness.tick));
    harness.Step(2);
    CHECK(HandlerLog::Instance().pingCalls == 1);
}

TEST_CASE("malformed and unknown intents are counted, and the connection lives")
{
    Harness harness;
    harness.Step(4);

    // An id no build has ever issued.
    {
        Core::BitWriter writer;
        WriteMessageType(MessageType::Intent, writer);
        writer.WriteVarUInt64(harness.tick);
        writer.WriteVarUInt32(9999);
        writer.WriteVarUInt32(0);
        harness.server.HandleMessage(harness.serverSide(), writer.Data());
    }
    CHECK(harness.Diagnostics().intentsMalformed == 1);

    // A truncated packet: the envelope starts and then stops.
    {
        Core::BitWriter writer;
        WriteMessageType(MessageType::Intent, writer);
        harness.server.HandleMessage(harness.serverSide(), writer.Data());
    }
    CHECK(harness.Diagnostics().intentsMalformed == 2);

    // ...and the connection is still perfectly usable afterwards.
    harness.Send(TestPlaceMarker{1, 1});
    CHECK(HandlerLog::Instance().placeMarkerCalls == 1);
}

TEST_CASE("the host's own intent goes through the same door")
{
    // A listen server's player is not a connection, so without this it would be
    // the one participant whose intents skipped every check — and the one whose
    // path no fuzz test ever covered.
    Harness harness;
    harness.Step(4);

    harness.server.SubmitLocalIntent(TestPlaceMarker{ /*target=*/ 5, /*slot=*/ 9});

    CHECK(HandlerLog::Instance().placeMarkerCalls == 1);
    CHECK(HandlerLog::Instance().lastPlaceMarker.target == 5);
    // Named as the host rather than as nobody, so a handler's authority checks
    // read the same way for the host as for anyone else.
    CHECK(HandlerLog::Instance().lastSender == HostClientId);
    CHECK(harness.server.HostDiagnostics().intentsAccepted == 1);
}

TEST_CASE("the host is not exempt from validation")
{
    // Trusted is not the same as correct. A host whose own build sends an
    // out-of-range value has a bug, and hiding it because the sender happens to
    // be local is how it survives to ship.
    Harness harness;
    harness.Step(4);

    harness.server.SubmitLocalIntent(TestPing{ /*x=*/ 9999.f, /*y=*/ 0.f});

    CHECK(HandlerLog::Instance().pingCalls == 0);
    CHECK(harness.server.HostDiagnostics().intentsOutOfRange == 1);
}

TEST_CASE("the host's control checks are its own, not everyone's")
{
    Harness harness;
    harness.Step(4);

    const ECS::Entity pawn = SpawnReplicated(harness.serverScene);
    harness.Step(4);

    // Controlled by a remote client, so the host does not own it either.
    harness.server.SetControl(pawn, harness.server.ClientIdOf(harness.serverSide()));
    harness.Step(2);
    harness.server.SubmitLocalIntent(TestMovePawn{pawn, ECS::NullEntity, 0});
    CHECK(HandlerLog::Instance().movePawnCalls == 0);
    CHECK(harness.server.HostDiagnostics().intentsNotYours == 1);

    // ...and once the host controls it, the same submission goes through.
    harness.server.SetControl(pawn, HostClientId);
    harness.Step(2);
    harness.server.SubmitLocalIntent(TestMovePawn{pawn, ECS::NullEntity, 0});
    CHECK(HandlerLog::Instance().movePawnCalls == 1);
}

TEST_CASE("an intent nobody handles is dropped and counted")
{
    // Normal rather than exceptional: the sender's build may care about
    // something this one does not. What matters is that it is visible, so a
    // handler somebody meant to write is a number rather than a mystery.
    Harness harness;
    harness.Step(4);

    harness.Send(TestUnhandled{ /*value=*/ 1});

    CHECK(harness.Diagnostics().intentsUnhandled == 1);
    CHECK(harness.Diagnostics().intentsAccepted == 0);

    // ...and the connection carries on, because nobody being home is not a
    // fault.
    harness.Send(TestPlaceMarker{1, 1});
    CHECK(HandlerLog::Instance().placeMarkerCalls == 1);
}
