/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBlueprintReplication.cpp
/// @brief The server half of blueprint replication, driven directly.
///
/// The property everything else in stage 7b is built on: an instance's members
/// occupy **one contiguous NetId block**, so a single spawn record naming
/// `baseNetId` lets a client derive every member's id as `base + memberIndex`
/// without the server sending a list.
///
/// That is only worth anything if it holds under the conditions the server
/// actually runs in — ordinary entities being created in between, members
/// noticed in the wrong order, and an instance the provider cannot describe.
/// Those are the cases here (docs/blueprint-implementation-plan.md, stage 7b).

#include <doctest/doctest.h>

#include <ostream>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Net/NetTransport.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/NetSync/InstanceRecord.hpp>
#include <Assisi/NetSync/NetProtocol.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>
#include <Assisi/NetSync/ReplicationConfig.hpp>
#include <Assisi/NetSync/ReplicationProviders.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>
#include <Assisi/NetSync/TestMessageHandlers.hpp>
#include <Assisi/NetSync/TestNetComponents.hpp>

using namespace Assisi;
using namespace Assisi::NetSync;

namespace
{

/// Stands in for App's instance table. Only the three facts the server needs.
class FakeInstances final : public InstanceInfoProvider
{
public:
    void Add(ECS::InstanceId id, std::uint32_t memberCount, std::uint32_t blueprintIndex = 0)
    {
        InstanceInfo info;
        info.blueprintIndex = blueprintIndex;
        info.memberCount    = memberCount;
        info.placement.position = {static_cast<float>(id.value), 0.f, 0.f};
        _rows.emplace(id, info);
    }

    [[nodiscard]] bool Describe(ECS::InstanceId id, InstanceInfo &out) override
    {
        ++describeCalls;
        const auto it = _rows.find(id);
        if (it == _rows.end())
            return false;
        out = it->second;
        return true;
    }

    /// The strongest form of the authored-value saving: every component of every
    /// member is exactly what the file says, so anything the client is assumed to
    /// have derived is never sent at all. Off by default — the base class already
    /// answers "no" — because most cases here are about ids, not about bytes.
    [[nodiscard]] bool MatchesAuthored(ECS::InstanceId id, std::uint32_t memberIndex,
                                       Core::Reflect::ComponentId component, const void *data) override
    {
        (void)id;
        (void)memberIndex;
        (void)component;
        (void)data;
        return matchEverything;
    }

    int describeCalls   = 0;
    bool matchEverything = false;

private:
    std::unordered_map<ECS::InstanceId, InstanceInfo> _rows;
};

struct Fixture
{
    Net::NetTransport transport;
    ECS::Scene scene;
    ECS::Scene clientScene;
    std::pair<Net::ConnectionId, Net::ConnectionId> pair;
    ReplicationServer server;
    ReplicationClient client;
    FakeInstances *instances = nullptr;

    Fixture() : pair(transport.CreateLoopbackPair()), server(transport, scene), client(transport, clientScene, pair.second)
    {
        auto owned = std::make_unique<FakeInstances>();
        instances  = owned.get();
        server.SetInstanceInfoProvider(std::move(owned));
    }

    /// Bring a connection all the way to ready. Relevancy state only exists once
    /// snapshots are actually going out, so a test about who sees what has to
    /// get past the handshake.
    void Connect()
    {
        server.SetContentSetHash(0);
        client.SetContentSetHash(0);
        server.AddConnection(pair.first);
        Step(6);
    }

    /// While set, the client's acks are buffered instead of delivered. The only
    /// way to put a leave and a re-entry *inside* one round trip, which is the
    /// case the instance-granular re-entry rule exists for.
    bool holdAcks = false;
    std::vector<std::vector<std::byte>> heldAcks;

    /// While set, snapshots are built and counted as sent but never delivered.
    /// Packet loss, in other words — which is the only way to reach the states
    /// that only exist because a snapshot the server believes in never landed.
    bool dropSnapshots    = false;
    std::uint32_t snapshotsDropped = 0;

    /// One delivery pass, no tick. Separate because a snapshot built on a tick is
    /// only ever seen by the *next* poll: to lose the last snapshot of a lossy
    /// window, the window has to end with a poll rather than with a Tick.
    void Poll()
    {
        std::vector<Net::NetEvent> events;
        transport.Poll(events);
        for (const Net::NetEvent &event : events)
        {
            if (event.type != Net::NetEvent::Type::Message)
                continue;
            if (event.connection == pair.first)
            {
                if (holdAcks)
                    heldAcks.emplace_back(event.payload.begin(), event.payload.end());
                else
                    server.HandleMessage(pair.first, event.payload);
            }
            else if (dropSnapshots)
            {
                ++snapshotsDropped;
            }
            else
            {
                client.HandleMessage(event.payload);
            }
        }
    }

    void Step(int times)
    {
        for (int i = 0; i < times; ++i)
        {
            Poll();
            server.Tick(++tick);
        }
    }

    void ReleaseAcks()
    {
        holdAcks = false;
        for (const std::vector<std::byte> &payload : heldAcks)
            server.HandleMessage(pair.first, payload);
        heldAcks.clear();
    }

    /// Deliver exactly one of the acks being held, leaving the rest held. The
    /// acks are in arrival order, so index 0 is the oldest — which is what "an
    /// ack for a snapshot sent before the revoke" means in practice.
    void ReleaseHeldAck(std::size_t index)
    {
        REQUIRE(index < heldAcks.size());
        server.HandleMessage(pair.first, heldAcks[index]);
        heldAcks.erase(heldAcks.begin() + static_cast<std::ptrdiff_t>(index));
    }

    /// A replicated member of @p id at @p index.
    ECS::Entity Member(ECS::InstanceId id, std::uint32_t index)
    {
        const ECS::Entity entity = scene.Create();
        (void)scene.Add(entity, ECS::Transform{});
        (void)scene.Add(entity, Replicated{});
        (void)scene.Add(entity, ECS::BlueprintMember{.instanceId = id, .memberIndex = index});
        return entity;
    }

    /// A replicated entity that belongs to no instance.
    ECS::Entity Loose()
    {
        const ECS::Entity entity = scene.Create();
        (void)scene.Add(entity, ECS::Transform{});
        (void)scene.Add(entity, Replicated{});
        return entity;
    }

    /// Assign ids the way the server does: one pass of the snapshot walk.
    ///
    /// Driven through Tick rather than the internal id call, so these cases test
    /// what a running server does. The walk visits entities in creation order,
    /// which is also how a member gets noticed "first" in practice.
    void AssignIds() { server.Tick(++tick); }

    std::uint64_t tick = 0;
};

} // namespace

TEST_CASE("Blueprint replication: an instance's members are one contiguous block")
{
    Fixture fixture;
    fixture.instances->Add(ECS::InstanceId{1}, 3);

    const ECS::Entity body  = fixture.Member(ECS::InstanceId{1}, 0);
    const ECS::Entity left  = fixture.Member(ECS::InstanceId{1}, 1);
    const ECS::Entity right = fixture.Member(ECS::InstanceId{1}, 2);

    fixture.AssignIds();

    const NetId base = fixture.server.NetIdOf(body);
    REQUIRE(base != InvalidNetId);
    CHECK(fixture.server.NetIdOf(left) == NetId{base.value + 1});
    CHECK(fixture.server.NetIdOf(right) == NetId{base.value + 2});

    // One lookup for the instance, not one per member — the block is what is
    // cached, and re-describing per member would let the answer change midway.
    CHECK(fixture.instances->describeCalls == 1);
}

TEST_CASE("Blueprint replication: a loose entity created mid-instance lands outside the block")
{
    Fixture fixture;
    fixture.instances->Add(ECS::InstanceId{1}, 3);

    // The case a lazily-grown block would fail: something else asks for an id
    // between two members of the same instance.
    const ECS::Entity body   = fixture.Member(ECS::InstanceId{1}, 0);
    const ECS::Entity other  = fixture.Loose();
    const ECS::Entity lid    = fixture.Member(ECS::InstanceId{1}, 1);
    fixture.AssignIds();

    const NetId first  = fixture.server.NetIdOf(body);
    const NetId loose  = fixture.server.NetIdOf(other);
    const NetId second = fixture.server.NetIdOf(lid);

    CHECK(second == NetId{first.value + 1});
    CHECK(loose != NetId{first.value + 1});
    CHECK(loose.value >= first.value + 3); // past the whole reserved range
}

TEST_CASE("Blueprint replication: members noticed out of order still derive from the base")
{
    Fixture fixture;
    fixture.instances->Add(ECS::InstanceId{1}, 4);

    // An event send can hand an id to any member first — the block must not
    // depend on member 0 being the one that triggers it.
    const ECS::Entity late  = fixture.Member(ECS::InstanceId{1}, 2);
    const ECS::Entity early = fixture.Member(ECS::InstanceId{1}, 0);
    fixture.AssignIds();

    const NetId third = fixture.server.NetIdOf(late);
    const NetId zero  = fixture.server.NetIdOf(early);

    REQUIRE(third != InvalidNetId);
    REQUIRE(zero != InvalidNetId);
    CHECK(third == NetId{zero.value + 2});
}

TEST_CASE("Blueprint replication: two instances of one blueprint get disjoint blocks")
{
    Fixture fixture;
    fixture.instances->Add(ECS::InstanceId{1}, 3);
    fixture.instances->Add(ECS::InstanceId{2}, 3);

    const ECS::Entity first  = fixture.Member(ECS::InstanceId{1}, 0);
    const ECS::Entity second = fixture.Member(ECS::InstanceId{2}, 0);
    fixture.AssignIds();

    const NetId a = fixture.server.NetIdOf(first);
    const NetId b = fixture.server.NetIdOf(second);

    CHECK(a != b);
    CHECK(b.value >= a.value + 3);
}

TEST_CASE("Blueprint replication: an instance the provider cannot describe replicates loosely")
{
    Fixture fixture; // nothing added, so Describe always fails

    const ECS::Entity body = fixture.Member(ECS::InstanceId{9}, 0);
    const ECS::Entity lid  = fixture.Member(ECS::InstanceId{9}, 1);
    fixture.AssignIds();

    const NetId first  = fixture.server.NetIdOf(body);
    const NetId second = fixture.server.NetIdOf(lid);

    // Still replicated — correct, just without the record's saving — and the ids
    // are ordinary consecutive ones rather than a block that means nothing.
    REQUIRE(first != InvalidNetId);
    REQUIRE(second != InvalidNetId);
    CHECK(second == NetId{first.value + 1});
}

TEST_CASE("Blueprint replication: with no provider installed, nothing blocks")
{
    Net::NetTransport transport;
    ECS::Scene scene;
    ReplicationServer server{transport, scene};

    const ECS::Entity entity = scene.Create();
    (void)scene.Add(entity, ECS::Transform{});
    (void)scene.Add(entity, Replicated{});
    (void)scene.Add(entity, ECS::BlueprintMember{.instanceId = ECS::InstanceId{1}, .memberIndex = 7});

    // The default. A game that never installs one pays nothing, and a tag with
    // an out-of-range index cannot cost it an id it should not have.
    CHECK(server.Instances() == nullptr);
    server.Tick(1);
    CHECK(server.NetIdOf(entity) != InvalidNetId);
}

TEST_CASE("Blueprint replication: a member index outside the block is refused, not aliased")
{
    Fixture fixture;
    fixture.instances->Add(ECS::InstanceId{1}, 2);

    const ECS::Entity body    = fixture.Member(ECS::InstanceId{1}, 0);
    const ECS::Entity strayer = fixture.Member(ECS::InstanceId{1}, 5);
    fixture.AssignIds();

    const NetId base   = fixture.server.NetIdOf(body);
    const NetId beyond = fixture.server.NetIdOf(strayer);

    // A tag disagreeing with the definition is a bug either way, but the wrong
    // answer here is a NetId that collides with whatever was allocated next.
    //
    // Exactly the id after the block, not merely one past it: the aliased answer
    // is `base + 5`, which is also "past the block", so a `>=` here would pass
    // with the refusal deleted. The block is two wide, so the next ordinary
    // counter id is `base + 2`.
    REQUIRE(beyond != InvalidNetId);
    CHECK(beyond == NetId{base.value + 2});
}

TEST_CASE("Blueprint replication: the record survives a round trip and is idempotent")
{
    Net::NetTransport transport;
    ECS::Scene serverScene;
    ECS::Scene clientScene;
    const auto pair = transport.CreateLoopbackPair();

    ReplicationServer server{transport, serverScene};
    ReplicationClient client{transport, clientScene, pair.second};

    auto owned     = std::make_unique<FakeInstances>();
    auto *instances = owned.get();
    instances->Add(ECS::InstanceId{1}, 2, /*blueprintIndex=*/ 4);
    server.SetInstanceInfoProvider(std::move(owned));

    for (std::uint32_t index = 0; index < 2; ++index)
    {
        const ECS::Entity entity = serverScene.Create();
        (void)serverScene.Add(entity, ECS::Transform{});
        (void)serverScene.Add(entity, Replicated{});
        (void)serverScene.Add(entity,
                              ECS::BlueprintMember{.instanceId = ECS::InstanceId{1}, .memberIndex = index});
    }

    // Both sides agree trivially on the content set: this is about the record
    // crossing, not about what is on disk.
    server.SetContentSetHash(0);
    client.SetContentSetHash(0);
    server.AddConnection(pair.first);

    // Several steps: the record is resent until acked, so this also covers the
    // resend arriving at a client that already has it — which must change
    // nothing rather than accumulate a second row.
    std::uint64_t tick = 1;
    for (int step = 0; step < 8; ++step)
    {
        std::vector<Net::NetEvent> events;
        transport.Poll(events);
        for (const Net::NetEvent &event : events)
        {
            if (event.type != Net::NetEvent::Type::Message)
                continue;
            if (event.connection == pair.first)
                server.HandleMessage(pair.first, event.payload);
            else
                client.HandleMessage(event.payload);
        }
        server.Tick(tick++);
    }

    const auto &records = client.InstanceRecords();
    REQUIRE(records.size() == 1);

    const InstanceRecord &entry = records.begin()->second;
    CHECK(entry.blueprintIndex == 4);
    CHECK(entry.memberCount == 2);
    CHECK(entry.base.IsValid());
    // The placement is what every member's transform is composed from, so it
    // travels at full precision rather than quantized.
    CHECK(entry.placement.position.x == doctest::Approx(1.f));
}

namespace
{

/// Stands in for App's blueprint expansion. Builds @p memberCount bare entities,
/// which is all the binding needs to be checked.
class FakeExpander final : public InstanceExpander
{
public:
    explicit FakeExpander(std::uint32_t produce = 0) : _produce(produce) {}

    [[nodiscard]] bool Expand(const InstanceRecord &record, std::vector<ECS::Entity> &out,
                              ECS::InstanceId &outInstance) override
    {
        ++calls;
        if (fail)
            return false;
        // A local id of this machine's own choosing — deliberately not the
        // server's, which is the whole point of the translation.
        outInstance = ECS::InstanceId{100u + static_cast<std::uint32_t>(calls)};
        expandedByBase[record.base] = outInstance;
        const std::uint32_t count   = _produce != 0 ? _produce : record.memberCount;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const ECS::Entity entity = scene->Create();
            (void)scene->Add(entity, ECS::Transform{});
            out.push_back(entity);
        }
        return true;
    }

    void Collapse(ECS::InstanceId localInstance) override { collapsed.push_back(localInstance); }

    ECS::Scene *scene = nullptr;
    bool fail  = false;
    int calls = 0;
    std::vector<ECS::InstanceId> collapsed;
    /// What this expander answered for each record, so a test can name the id it
    /// chose rather than the server's — which is the whole point of the pair.
    std::unordered_map<NetId, ECS::InstanceId> expandedByBase;

private:
    std::uint32_t _produce = 0;
};

} // namespace

TEST_CASE("Blueprint replication: the client expands a record into bound members")
{
    Net::NetTransport transport;
    ECS::Scene serverScene;
    ECS::Scene clientScene;
    const auto pair = transport.CreateLoopbackPair();

    ReplicationServer server{transport, serverScene};
    ReplicationClient client{transport, clientScene, pair.second};

    auto ownedInfo = std::make_unique<FakeInstances>();
    ownedInfo->Add(ECS::InstanceId{1}, 3);
    server.SetInstanceInfoProvider(std::move(ownedInfo));

    auto ownedExpander = std::make_unique<FakeExpander>();
    auto *expander      = ownedExpander.get();
    expander->scene     = &clientScene;
    client.SetInstanceExpander(std::move(ownedExpander));

    for (std::uint32_t index = 0; index < 3; ++index)
    {
        const ECS::Entity entity = serverScene.Create();
        (void)serverScene.Add(entity, ECS::Transform{});
        (void)serverScene.Add(entity, Replicated{});
        (void)serverScene.Add(entity,
                              ECS::BlueprintMember{.instanceId = ECS::InstanceId{1}, .memberIndex = index});
    }

    server.SetContentSetHash(0);
    client.SetContentSetHash(0);
    server.AddConnection(pair.first);

    std::uint64_t tick = 1;
    for (int i = 0; i < 10; ++i)
    {
        std::vector<Net::NetEvent> events;
        transport.Poll(events);
        for (const Net::NetEvent &event : events)
        {
            if (event.type != Net::NetEvent::Type::Message)
                continue;
            if (event.connection == pair.first)
                server.HandleMessage(pair.first, event.payload);
            else
                client.HandleMessage(event.payload);
        }
        server.Tick(tick++);
    }

    // Expanded once despite the record being resent until acked, and the members
    // it built are the ones the server's ids now name.
    CHECK(expander->calls == 1);
    CHECK(client.ReplicatedEntityCount() == 3);

    const InstanceRecord &entry = client.InstanceRecords().begin()->second;
    for (std::uint32_t member = 0; member < entry.memberCount; ++member)
    {
        const ECS::Entity mirror = client.EntityOf(NetId{entry.base.value + member});
        CHECK(mirror != ECS::NullEntity);
        CHECK(clientScene.IsAlive(mirror));
    }
}

TEST_CASE("Blueprint replication: a replicated tag names the client's instance, not the server's")
{
    Net::NetTransport transport;
    ECS::Scene serverScene;
    ECS::Scene clientScene;
    const auto pair = transport.CreateLoopbackPair();

    ReplicationServer server{transport, serverScene};
    ReplicationClient client{transport, clientScene, pair.second};

    // A server-side id chosen to be nothing the client would produce, so a tag
    // that crossed untranslated is visible rather than coincidentally right.
    const ECS::InstanceId serverInstance{7};

    auto ownedInfo = std::make_unique<FakeInstances>();
    ownedInfo->Add(serverInstance, 2);
    server.SetInstanceInfoProvider(std::move(ownedInfo));

    auto ownedExpander = std::make_unique<FakeExpander>();
    auto *expander      = ownedExpander.get();
    expander->scene     = &clientScene;
    client.SetInstanceExpander(std::move(ownedExpander));

    for (std::uint32_t index = 0; index < 2; ++index)
    {
        const ECS::Entity entity = serverScene.Create();
        (void)serverScene.Add(entity, ECS::Transform{});
        (void)serverScene.Add(entity, Replicated{});
        (void)serverScene.Add(entity, ECS::BlueprintMember{.instanceId = serverInstance, .memberIndex = index});
    }

    server.SetContentSetHash(0);
    client.SetContentSetHash(0);
    server.AddConnection(pair.first);

    std::uint64_t tick = 1;
    for (int i = 0; i < 12; ++i)
    {
        std::vector<Net::NetEvent> events;
        transport.Poll(events);
        for (const Net::NetEvent &event : events)
        {
            if (event.type != Net::NetEvent::Type::Message)
                continue;
            if (event.connection == pair.first)
                server.HandleMessage(pair.first, event.payload);
            else
                client.HandleMessage(event.payload);
        }
        server.Tick(tick++);
    }

    const InstanceRecord &entry  = client.InstanceRecords().begin()->second;
    const ECS::Entity mirror = client.EntityOf(entry.base);
    REQUIRE(mirror != ECS::NullEntity);

    const ECS::BlueprintMember *tag = clientScene.Get<ECS::BlueprintMember>(mirror);
    REQUIRE(tag != nullptr);

    // Without the codec hooks the tag would arrive carrying the server's
    // per-world counter, which names nothing here.
    CHECK(tag->instanceId != serverInstance);
    CHECK(tag->instanceId == ECS::InstanceId{101});
    CHECK(tag->memberIndex == 0);
}

TEST_CASE("Blueprint replication: an expansion that comes up short is refused")
{
    Net::NetTransport transport;
    ECS::Scene serverScene;
    ECS::Scene clientScene;
    const auto pair = transport.CreateLoopbackPair();

    ReplicationServer server{transport, serverScene};
    ReplicationClient client{transport, clientScene, pair.second};

    auto ownedInfo = std::make_unique<FakeInstances>();
    ownedInfo->Add(ECS::InstanceId{1}, 3);
    server.SetInstanceInfoProvider(std::move(ownedInfo));

    // Two members where the record says three: the disagreement that would bind
    // member ids to the wrong entities if it were tolerated.
    auto ownedExpander = std::make_unique<FakeExpander>(/*produce=*/ 2);
    auto *expander      = ownedExpander.get();
    expander->scene     = &clientScene;
    client.SetInstanceExpander(std::move(ownedExpander));

    for (std::uint32_t index = 0; index < 3; ++index)
    {
        const ECS::Entity entity = serverScene.Create();
        (void)serverScene.Add(entity, ECS::Transform{});
        (void)serverScene.Add(entity, Replicated{});
        (void)serverScene.Add(entity,
                              ECS::BlueprintMember{.instanceId = ECS::InstanceId{1}, .memberIndex = index});
    }

    server.SetContentSetHash(0);
    client.SetContentSetHash(0);
    server.AddConnection(pair.first);

    std::uint64_t tick = 1;
    for (int i = 0; i < 6; ++i)
    {
        std::vector<Net::NetEvent> events;
        transport.Poll(events);
        for (const Net::NetEvent &event : events)
        {
            if (event.type != Net::NetEvent::Type::Message)
                continue;
            if (event.connection == pair.first)
                server.HandleMessage(pair.first, event.payload);
            else
                client.HandleMessage(event.payload);
        }
        server.Tick(tick++);
    }

    // The record is not kept, so a retry is a clean retry rather than a partial
    // instance nothing will ever finish.
    CHECK(client.InstanceRecords().empty());
}

namespace
{

/// A live session holding one replicated instance, named by two different
/// numbers: the server's own id and the different one the client's expander
/// made for it. Both message tests below need exactly that and nothing else.
struct InstanceSession
{
    /// Deliberately nothing FakeExpander would produce — it counts from 101 — so
    /// an id that crossed untranslated is visible rather than coincidentally
    /// right.
    static constexpr ECS::InstanceId kServerInstance{7};

    Net::NetTransport transport;
    ECS::Scene serverScene;
    ECS::Scene clientScene;
    std::pair<Net::ConnectionId, Net::ConnectionId> pair;
    ReplicationServer server;
    ReplicationClient client;
    std::uint64_t tick = 1;

    InstanceSession()
        : pair(transport.CreateLoopbackPair()), server(transport, serverScene),
        client(transport, clientScene, pair.second)
    {
        auto ownedInfo = std::make_unique<FakeInstances>();
        ownedInfo->Add(kServerInstance, 2);
        server.SetInstanceInfoProvider(std::move(ownedInfo));

        auto ownedExpander   = std::make_unique<FakeExpander>();
        ownedExpander->scene = &clientScene;
        client.SetInstanceExpander(std::move(ownedExpander));

        for (std::uint32_t index = 0; index < 2; ++index)
        {
            const ECS::Entity entity = serverScene.Create();
            (void)serverScene.Add(entity, ECS::Transform{});
            (void)serverScene.Add(entity, Replicated{});
            (void)serverScene.Add(entity,
                                  ECS::BlueprintMember{.instanceId = kServerInstance, .memberIndex = index});
        }

        server.SetContentSetHash(0);
        client.SetContentSetHash(0);
        server.AddConnection(pair.first);
        NetSync::Test::HandlerLog::Instance().Clear();
        Step(12);
    }

    void Step(std::uint32_t times)
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
                    server.HandleMessage(pair.first, event.payload);
                else
                    client.HandleMessage(event.payload);
            }
            server.Tick(tick++);
        }
    }

    /// The id the client's own expansion took — what a client-side system reads
    /// off a mirror and would naturally put in a message.
    [[nodiscard]] ECS::InstanceId ClientInstance()
    {
        const auto &records = client.InstanceRecords();
        if (records.empty())
            return ECS::NullInstance;
        const ECS::Entity mirror = client.EntityOf(records.begin()->second.base);
        if (mirror == ECS::NullEntity)
            return ECS::NullInstance;
        const ECS::BlueprintMember *tag = clientScene.Get<ECS::BlueprintMember>(mirror);
        return tag != nullptr ? tag->instanceId : ECS::NullInstance;
    }
};

} // namespace

TEST_CASE("Blueprint replication: an intent naming an instance arrives as the server's instance id")
{
    InstanceSession session;
    REQUIRE(session.client.IsSynchronized());

    const ECS::InstanceId local = session.ClientInstance();
    REQUIRE(local.IsValid());
    REQUIRE(local != InstanceSession::kServerInstance); // otherwise the test proves nothing

    REQUIRE(session.client.SendIntent(NetSync::Test::TestTagInstance{.instance = local, .note = 3}, session.tick));
    session.Step(2);

    const NetSync::Test::HandlerLog &log = NetSync::Test::HandlerLog::Instance();
    REQUIRE(log.tagInstanceCalls == 1);
    CHECK(log.lastTagInstance.note == 3); // the ordinary field, to place the failure

    // With no hooks on the intent path the server's handler is handed the
    // client's own per-world counter, which names an unrelated instance here —
    // or nothing — and does it silently.
    CHECK(log.lastTagInstance.instance == InstanceSession::kServerInstance);
}

TEST_CASE("Blueprint replication: an event naming an instance arrives as the client's instance id")
{
    InstanceSession session;
    REQUIRE(session.client.IsSynchronized());

    const ECS::InstanceId local = session.ClientInstance();
    REQUIRE(local.IsValid());
    REQUIRE(local != InstanceSession::kServerInstance);

    session.server.Send(NetSync::Test::TestInstanceNamed{.instance = InstanceSession::kServerInstance});
    session.Step(3);

    const NetSync::Test::HandlerLog &log = NetSync::Test::HandlerLog::Instance();
    REQUIRE(log.instanceNamedCalls >= 1);

    // The mirror of the intent case: the client's handler must be told which
    // instance *here* the event is about, not which one there.
    CHECK(log.lastInstanceNamedOnClient.instance == local);

    // The host's own copy of the same event travels the same codec pair in
    // process, so it comes back naming the instance it started as.
    CHECK(log.lastInstanceNamedOnHost.instance == InstanceSession::kServerInstance);
}

namespace
{

/// Names exactly the NetIds it is told to, so a test can say "the provider sees
/// one wheel" and nothing else.
class PickyProvider final : public RelevancyProvider
{
public:
    void Compute(const RelevancyQuery &query, std::vector<NetId> &out) override
    {
        (void)query;
        out = named;
    }

    std::vector<NetId> named;
};

} // namespace

TEST_CASE("Blueprint replication: naming one member pulls the whole instance")
{
    Fixture fixture;
    fixture.instances->Add(ECS::InstanceId{1}, 4);

    std::vector<ECS::Entity> members;
    for (std::uint32_t index = 0; index < 4; ++index)
        members.push_back(fixture.Member(ECS::InstanceId{1}, index));
    const ECS::Entity loose = fixture.Loose();

    fixture.AssignIds();
    const NetId base = fixture.server.NetIdOf(members[0]);
    REQUIRE(base != InvalidNetId);

    auto owned    = std::make_unique<PickyProvider>();
    auto *provider = owned.get();
    // One wheel, and nothing else at all — not even the loose entity.
    provider->named = {NetId{base.value + 2}};
    fixture.server.SetRelevancyProvider(std::move(owned));

    fixture.Connect();

    // The whole car is relevant, because the client derives all four members
    // from one record and a partial instance is one it cannot attribute.
    for (std::uint32_t index = 0; index < 4; ++index)
        CHECK(fixture.server.IsRelevant(fixture.pair.first,NetId{base.value + index}));

    // ...and escalation pulls in the instance, not the neighbourhood.
    CHECK_FALSE(fixture.server.IsRelevant(fixture.pair.first,fixture.server.NetIdOf(loose)));
}

TEST_CASE("Blueprint replication: leaving and re-entering resends the record")
{
    Fixture fixture;
    fixture.instances->Add(ECS::InstanceId{1}, 3);

    std::vector<ECS::Entity> members;
    for (std::uint32_t index = 0; index < 3; ++index)
        members.push_back(fixture.Member(ECS::InstanceId{1}, index));

    fixture.AssignIds();
    const NetId base = fixture.server.NetIdOf(members[0]);

    auto owned     = std::make_unique<PickyProvider>();
    auto *provider  = owned.get();
    provider->named = {base};
    fixture.server.SetRelevancyProvider(std::move(owned));
    fixture.Connect();

    REQUIRE(fixture.client.InstanceRecords().size() == 1);

    // Acks stop landing, so the server's knownInstances keeps saying the client
    // has the record. Without holding them the ordinary cumulative-set mechanism
    // clears it anyway and this case never arises at all.
    fixture.holdAcks = true;

    // Out of the set: the client despawns the whole block as one run, which takes
    // its record with it.
    provider->named.clear();
    fixture.Step(4);
    CHECK(fixture.client.InstanceRecords().empty());

    // ...and straight back in, still inside the round trip. The record has to
    // come again — the client threw it away, and a member arriving with no
    // record is one it cannot attribute to any instance.
    provider->named = {base};
    fixture.Step(4);
    fixture.ReleaseAcks();
    fixture.Step(4);

    CHECK(fixture.client.InstanceRecords().size() == 1);
    CHECK(fixture.client.ReplicatedEntityCount() == 3);
}

TEST_CASE("Blueprint replication: a dead member is not resurrected by its siblings")
{
    Fixture fixture;
    fixture.instances->Add(ECS::InstanceId{1}, 3);

    std::vector<ECS::Entity> members;
    for (std::uint32_t index = 0; index < 3; ++index)
        members.push_back(fixture.Member(ECS::InstanceId{1}, index));

    fixture.AssignIds();
    const NetId base = fixture.server.NetIdOf(members[0]);

    auto owned    = std::make_unique<PickyProvider>();
    auto *provider = owned.get();
    provider->named = {base};
    fixture.server.SetRelevancyProvider(std::move(owned));
    fixture.Connect();

    // One member dies on its own — pruning and per-member destruction are both
    // legal, so escalation must go back through the live set rather than
    // trusting the block's width.
    const NetId dead = fixture.server.NetIdOf(members[1]);
    fixture.scene.Destroy(members[1]);
    fixture.scene.FlushDestroyed();
    fixture.Step(6); // enough for a snapshot to actually go out and recompute

    CHECK(fixture.server.IsRelevant(fixture.pair.first,base));
    CHECK(fixture.server.IsRelevant(fixture.pair.first,NetId{base.value + 2}));
    CHECK_FALSE(fixture.server.IsRelevant(fixture.pair.first,dead));
}

TEST_CASE("Blueprint replication: destroying an instance costs one despawn run")
{
    Net::NetTransport transport;
    ECS::Scene serverScene;
    ECS::Scene clientScene;
    const auto pair = transport.CreateLoopbackPair();

    ReplicationServer server{transport, serverScene};
    ReplicationClient client{transport, clientScene, pair.second};

    auto owned     = std::make_unique<FakeInstances>();
    auto *instances = owned.get();
    instances->Add(ECS::InstanceId{1}, 6);
    server.SetInstanceInfoProvider(std::move(owned));

    std::vector<ECS::Entity> members;
    for (std::uint32_t index = 0; index < 6; ++index)
    {
        const ECS::Entity entity = serverScene.Create();
        (void)serverScene.Add(entity, ECS::Transform{});
        (void)serverScene.Add(entity, Replicated{});
        (void)serverScene.Add(entity,
                              ECS::BlueprintMember{.instanceId = ECS::InstanceId{1}, .memberIndex = index});
        members.push_back(entity);
    }

    server.SetContentSetHash(0);
    client.SetContentSetHash(0);
    server.AddConnection(pair.first);

    std::uint64_t tick = 1;
    const auto step = [&](int times)
                      {
                          for (int i = 0; i < times; ++i)
                          {
                              std::vector<Net::NetEvent> events;
                              transport.Poll(events);
                              for (const Net::NetEvent &event : events)
                              {
                                  if (event.type != Net::NetEvent::Type::Message)
                                      continue;
                                  if (event.connection == pair.first)
                                      server.HandleMessage(pair.first, event.payload);
                                  else
                                      client.HandleMessage(event.payload);
                              }
                              server.Tick(tick++);
                          }
                      };

    step(10);
    REQUIRE(client.ReplicatedEntityCount() == 6);
    REQUIRE(client.InstanceRecords().size() == 1);

    for (const ECS::Entity member : members)
        serverScene.Destroy(member);
    serverScene.FlushDestroyed();

    step(10);

    // All six gone, and — the point of the run — the record went with them,
    // because the run covered the whole block. A client holding a record for an
    // instance the server no longer describes would compose future members
    // against a placement nobody is maintaining.
    CHECK(client.ReplicatedEntityCount() == 0);
    CHECK(client.InstanceRecords().empty());
}

TEST_CASE("Blueprint replication: a retired record collapses the instance the expander built")
{
    Net::NetTransport transport;
    ECS::Scene serverScene;
    ECS::Scene clientScene;
    const auto pair = transport.CreateLoopbackPair();

    ReplicationServer server{transport, serverScene};
    ReplicationClient client{transport, clientScene, pair.second};

    auto owned     = std::make_unique<FakeInstances>();
    auto *instances = owned.get();
    instances->Add(ECS::InstanceId{1}, 3);
    instances->Add(ECS::InstanceId{2}, 3);
    server.SetInstanceInfoProvider(std::move(owned));

    auto ownedExpander = std::make_unique<FakeExpander>();
    auto *expander      = ownedExpander.get();
    expander->scene     = &clientScene;
    client.SetInstanceExpander(std::move(ownedExpander));

    // Two instances, so this measures *which* one was collapsed rather than just
    // that something was. One alone passes against a fix that collapses every
    // instance it has whenever any record retires.
    const auto build = [&](ECS::InstanceId instanceId)
                       {
                           std::vector<ECS::Entity> members;
                           for (std::uint32_t index = 0; index < 3; ++index)
                           {
                               const ECS::Entity entity = serverScene.Create();
                               (void)serverScene.Add(entity, ECS::Transform{});
                               (void)serverScene.Add(entity, Replicated{});
                               (void)serverScene.Add(entity, ECS::BlueprintMember{.instanceId = instanceId, .memberIndex = index});
                               members.push_back(entity);
                           }
                           return members;
                       };
    const std::vector<ECS::Entity> first  = build(ECS::InstanceId{1});
    const std::vector<ECS::Entity> second = build(ECS::InstanceId{2});

    server.SetContentSetHash(0);
    client.SetContentSetHash(0);
    server.AddConnection(pair.first);

    std::uint64_t tick = 1;
    const auto step = [&](int times)
                      {
                          for (int i = 0; i < times; ++i)
                          {
                              std::vector<Net::NetEvent> events;
                              transport.Poll(events);
                              for (const Net::NetEvent &event : events)
                              {
                                  if (event.type != Net::NetEvent::Type::Message)
                                      continue;
                                  if (event.connection == pair.first)
                                      server.HandleMessage(pair.first, event.payload);
                                  else
                                      client.HandleMessage(event.payload);
                              }
                              server.Tick(tick++);
                          }
                      };

    step(10);
    REQUIRE(client.InstanceRecords().size() == 2);
    const NetId firstBase  = server.NetIdOf(first[0]);
    const NetId secondBase = server.NetIdOf(second[0]);
    REQUIRE(expander->expandedByBase.contains(firstBase));
    REQUIRE(expander->expandedByBase.contains(secondBase));

    // Expanded is not retired: a live instance must not be collapsed out from
    // under itself just because a snapshot arrived.
    CHECK(expander->collapsed.empty());

    for (const ECS::Entity member : second)
        serverScene.Destroy(member);
    serverScene.FlushDestroyed();
    step(10);

    REQUIRE(client.InstanceRecords().size() == 1);

    // Once, and naming the id the *expander* chose — 100-and-up here, deliberately
    // nothing like the server's InstanceId{2}, because the server's id means
    // nothing on this machine. Without this call the expander is never told, and
    // whatever it recorded when it expanded outlives every member of the
    // instance.
    REQUIRE(expander->collapsed.size() == 1);
    CHECK(expander->collapsed[0].value == expander->expandedByBase[secondBase].value);
    CHECK(expander->collapsed[0].value != expander->expandedByBase[firstBase].value);
}

TEST_CASE("Blueprint replication: a block is allocated once and outlives the tick")
{
    Fixture fixture;
    fixture.instances->Add(ECS::InstanceId{1}, 3);

    const ECS::Entity body = fixture.Member(ECS::InstanceId{1}, 0);
    fixture.AssignIds();
    const NetId first = fixture.server.NetIdOf(body);
    REQUIRE(first != InvalidNetId);

    // A member that arrives later takes its id from the block allocated on the
    // earlier tick — the ids of an instance cannot depend on when its members
    // happened to be noticed, or a record sent once would stop describing them.
    const ECS::Entity lid = fixture.Member(ECS::InstanceId{1}, 1);
    fixture.AssignIds();

    CHECK(fixture.server.NetIdOf(body) == first);
    CHECK(fixture.server.NetIdOf(lid) == NetId{first.value + 1});
    CHECK(fixture.instances->describeCalls == 1);
}

// ── The instance-record lifecycle ─────────────────────────────────────────────
// What a record means over time: who may see a member, when a record is thrown
// away, which members a record implies exist, and which acks may reinstate one.
// The record carries which members exist, dies with its last member, and pays
// the snapshot byte budget; forgetting an instance reaches the in-flight ring;
// and the ControllerOnly filter is re-applied after block escalation, which the
// pair of cases below pins in both directions.

namespace
{

/// A replicated member with a relevance class other than Default. `Member` above
/// deliberately builds the ordinary kind, and the escape classes are only read
/// when a relevancy provider is installed.
ECS::Entity ClassifiedMember(Fixture &fixture, ECS::InstanceId id, std::uint32_t index, Relevance relevance)
{
    const ECS::Entity entity = fixture.scene.Create();
    (void)fixture.scene.Add(entity, ECS::Transform{});
    Replicated marker;
    marker.relevance = relevance;
    (void)fixture.scene.Add(entity, marker);
    (void)fixture.scene.Add(entity, ECS::BlueprintMember{.instanceId = id, .memberIndex = index});
    return entity;
}

/// Give the fixture's client an expander, so a record actually becomes entities
/// and "was this rebuilt or did a bare mirror appear" is answerable.
FakeExpander *InstallExpander(Fixture &fixture)
{
    auto owned = std::make_unique<FakeExpander>();
    auto *raw   = owned.get();
    raw->scene  = &fixture.clientScene;
    fixture.client.SetInstanceExpander(std::move(owned));
    return raw;
}

/// Does this client hold a live mirror for @p netId? Both halves matter: a
/// binding to a destroyed entity is as wrong as no binding at all.
bool HasLiveMirror(Fixture &fixture, NetId netId)
{
    const ECS::Entity mirror = fixture.client.EntityOf(netId);
    return mirror != ECS::NullEntity && fixture.clientScene.IsAlive(mirror);
}

} // namespace

TEST_CASE("Blueprint replication: escalation does not hand out a ControllerOnly member")
{
    // ControllerOnly runs again over what escalation produced. Escalation
    // re-adds every member of any block a surviving sibling belongs to,
    // re-intersecting only against the live set, so with the filter applied only
    // once — before escalation — naming any wheel of the car buys the private
    // member too.
    Fixture fixture;
    fixture.instances->Add(ECS::InstanceId{1}, 3);

    const ECS::Entity body = fixture.Member(ECS::InstanceId{1}, 0);
    // Controlled by nobody, which is the honest reading of "only the controller
    // may see it": no connection qualifies, so no connection is told.
    (void)ClassifiedMember(fixture, ECS::InstanceId{1}, 1, Relevance::ControllerOnly);
    (void)fixture.Member(ECS::InstanceId{1}, 2);

    fixture.AssignIds();
    const NetId base = fixture.server.NetIdOf(body);
    REQUIRE(base != InvalidNetId);

    auto owned    = std::make_unique<PickyProvider>();
    auto *provider = owned.get();
    // One ordinary member, and nothing else. Everything else about this instance
    // reaches the connection through escalation.
    provider->named = {base};
    fixture.server.SetRelevancyProvider(std::move(owned));

    fixture.Connect();

    // Escalation still works — the car is visible...
    REQUIRE(fixture.server.IsRelevant(fixture.pair.first, base));
    REQUIRE(fixture.server.IsRelevant(fixture.pair.first, NetId{base.value + 2}));

    // ...and that one part of it is not.
    CHECK_FALSE(fixture.server.IsRelevant(fixture.pair.first, NetId{base.value + 1}));

    // The relevant set is what the snapshot is built from, so the withheld member
    // has no mirror on the other side either. Checked on the wire as well as at
    // the policy, because a set that says no while the bytes go out anyway is the
    // failure this finding is about.
    CHECK(fixture.client.EntityOf(NetId{base.value + 1}) == ECS::NullEntity);
    CHECK(fixture.client.ReplicatedEntityCount() == 2);
}

TEST_CASE("Blueprint replication: ControllerOnly is ignored when no relevancy provider is installed" *
          doctest::should_fail())
{
    // ENG-129, open. Every other ControllerOnly case installs a provider, which
    // is what hid this: RelevancyConfig::Provider defaults to All, that leaves
    // _relevancy null, and ComputeEffective returns the live set outright before
    // either ApplyControllerOnly call is reached. The class is inert in the
    // default configuration, and the entity goes out on the wire — the client
    // below really does build a mirror of a member it must not know about.
    //
    // should_fail until ENG-129 lands; the fix removes this decorator. Setup is
    // deliberately identical to the escalation case above except for the one
    // missing SetRelevancyProvider call, so the diff between them is the finding.
    Fixture fixture;
    fixture.instances->Add(ECS::InstanceId{1}, 3);

    const ECS::Entity body = fixture.Member(ECS::InstanceId{1}, 0);
    (void)ClassifiedMember(fixture, ECS::InstanceId{1}, 1, Relevance::ControllerOnly);
    (void)fixture.Member(ECS::InstanceId{1}, 2);

    fixture.AssignIds();
    const NetId base = fixture.server.NetIdOf(body);
    REQUIRE(base != InvalidNetId);

    fixture.Connect();

    CHECK_FALSE(fixture.server.IsRelevant(fixture.pair.first, NetId{base.value + 1}));
    CHECK(fixture.client.EntityOf(NetId{base.value + 1}) == ECS::NullEntity);
}

TEST_CASE("Blueprint replication: escalation still delivers a ControllerOnly member to its controller")
{
    // The other half, and the one that makes this a filter rather than a
    // ban: re-applying ControllerOnly after escalation must not cost the
    // controlling connection its own private member. Reversing the comparison in
    // the re-applied filter, or dropping every ControllerOnly member outright,
    // passes the case above and dies here.
    Fixture fixture;
    fixture.instances->Add(ECS::InstanceId{1}, 3);

    const ECS::Entity body    = fixture.Member(ECS::InstanceId{1}, 0);
    const ECS::Entity private_ = ClassifiedMember(fixture, ECS::InstanceId{1}, 1, Relevance::ControllerOnly);
    (void)fixture.Member(ECS::InstanceId{1}, 2);

    fixture.AssignIds();
    const NetId base = fixture.server.NetIdOf(body);
    REQUIRE(base != InvalidNetId);

    auto owned    = std::make_unique<PickyProvider>();
    auto *provider = owned.get();
    // Again only the ordinary member: the private one arrives, if it arrives at
    // all, through escalation.
    provider->named = {base};
    fixture.server.SetRelevancyProvider(std::move(owned));

    fixture.Connect();
    fixture.server.SetControl(private_, fixture.server.ClientIdOf(fixture.pair.first));
    fixture.Step(6);

    CHECK(fixture.server.IsRelevant(fixture.pair.first, NetId{base.value + 1}));
    CHECK(fixture.client.EntityOf(NetId{base.value + 1}) != ECS::NullEntity);
    CHECK(fixture.client.ReplicatedEntityCount() == 3);
}

TEST_CASE("Blueprint replication: escalation costs each relevant block its member count, once")
{
    // Escalation pushes a block once, not once per already-relevant member of
    // it: the per-member form costs memberCount² pushes and a sort over the
    // result, which is invisible at three members and ruinous at scale — 100
    // cars × 20 members is 40k pushes and a 40k sort, per connection, per
    // snapshot.
    //
    // Pinned on the counter rather than on a clock: what matters is the work
    // done, and a timing assertion at test scale measures the machine.
    Fixture fixture;
    fixture.instances->Add(ECS::InstanceId{1}, 4);
    fixture.instances->Add(ECS::InstanceId{2}, 4);
    fixture.instances->Add(ECS::InstanceId{3}, 4);

    std::vector<ECS::Entity> first;
    std::vector<ECS::Entity> second;
    for (std::uint32_t index = 0; index < 4; ++index)
        first.push_back(fixture.Member(ECS::InstanceId{1}, index));
    for (std::uint32_t index = 0; index < 4; ++index)
        second.push_back(fixture.Member(ECS::InstanceId{2}, index));
    for (std::uint32_t index = 0; index < 4; ++index)
        (void)fixture.Member(ECS::InstanceId{3}, index);

    fixture.AssignIds();
    const NetId firstBase  = fixture.server.NetIdOf(first[0]);
    const NetId secondBase = fixture.server.NetIdOf(second[0]);
    REQUIRE(firstBase != InvalidNetId);
    REQUIRE(secondBase != InvalidNetId);

    auto owned    = std::make_unique<PickyProvider>();
    auto *provider = owned.get();
    // The worst case for a per-member loop, and the ordinary case in a real
    // session: every member of two instances is independently relevant. The
    // third is named by nothing, and must cost nothing.
    for (std::uint32_t index = 0; index < 4; ++index)
    {
        provider->named.push_back(NetId{firstBase.value + index});
        provider->named.push_back(NetId{secondBase.value + index});
    }
    std::sort(provider->named.begin(), provider->named.end());
    fixture.server.SetRelevancyProvider(std::move(owned));

    fixture.Connect();

    const ConnectionDiagnostics *diagnostics = fixture.server.Diagnostics(fixture.pair.first);
    REQUIRE(diagnostics != nullptr);

    // Two blocks of four, each escalated once: eight. A per-member loop would
    // push each block once per member of it already in the set — 4² + 4² = 32.
    CHECK(diagnostics->escalationPushes == 8);

    // ...and the saving is in the work, not in the answer: both cars are still
    // whole, which is the only reason escalation exists.
    for (std::uint32_t index = 0; index < 4; ++index)
    {
        CHECK(fixture.server.IsRelevant(fixture.pair.first, NetId{firstBase.value + index}));
        CHECK(fixture.server.IsRelevant(fixture.pair.first, NetId{secondBase.value + index}));
    }
}

TEST_CASE("Blueprint replication: skipping over an escalated block still sees the next one")
{
    // The hazard the skip-ahead introduces, and the reason this case sits beside
    // the one above: having pushed a block, the walk advances past the rest of
    // that block's ids in `effective`. Advancing one id too far, or by the block's
    // member count from the current position, both still pass the counter case
    // above and lose an instance here.
    //
    // Two *adjacent* blocks is what makes it bite. The second block's base is the
    // first block's end, so a skip that stops one id late consumes it, and the
    // only member of the second instance anything named goes with it — leaving
    // three of its four members with no way into the set. A loose entity between
    // the blocks would open a gap and hide exactly that.
    Fixture fixture;
    fixture.instances->Add(ECS::InstanceId{1}, 4);
    fixture.instances->Add(ECS::InstanceId{2}, 4);

    std::vector<ECS::Entity> first;
    for (std::uint32_t index = 0; index < 4; ++index)
        first.push_back(fixture.Member(ECS::InstanceId{1}, index));
    std::vector<ECS::Entity> second;
    for (std::uint32_t index = 0; index < 4; ++index)
        second.push_back(fixture.Member(ECS::InstanceId{2}, index));

    fixture.AssignIds();
    const NetId firstBase  = fixture.server.NetIdOf(first[0]);
    const NetId secondBase = fixture.server.NetIdOf(second[0]);
    REQUIRE(firstBase != InvalidNetId);
    REQUIRE(secondBase != InvalidNetId);
    // The boundary the case is about: nothing sits between the two blocks.
    REQUIRE(secondBase.value == firstBase.value + 4);

    auto owned    = std::make_unique<PickyProvider>();
    auto *provider = owned.get();
    // The last member of the first block and the first of the second — the two
    // ids either side of the seam, and nothing else.
    provider->named = {NetId{firstBase.value + 3}, secondBase};
    fixture.server.SetRelevancyProvider(std::move(owned));

    fixture.Connect();

    for (std::uint32_t index = 0; index < 4; ++index)
    {
        CHECK(fixture.server.IsRelevant(fixture.pair.first, NetId{firstBase.value + index}));
        CHECK(fixture.server.IsRelevant(fixture.pair.first, NetId{secondBase.value + index}));
    }

    const ConnectionDiagnostics *diagnostics = fixture.server.Diagnostics(fixture.pair.first);
    REQUIRE(diagnostics != nullptr);
    // Both blocks, each once, from one named member apiece.
    CHECK(diagnostics->escalationPushes == 8);
}

TEST_CASE("Blueprint replication: two adjacent instances leaving together take both records")
{
    // Despawns are run-length encoded over the whole set and do not respect block
    // boundaries, so the client erases a record on "no member left" rather than
    // on a run matching its base and memberCount. Two 3-member instances at
    // adjacent bases leave as one run of 6: matching on the run would find the
    // first record's count disagreeing and never probe the second's base, leaving
    // both records behind entities that are destroyed all the same.
    Fixture fixture;
    fixture.instances->Add(ECS::InstanceId{1}, 3);
    fixture.instances->Add(ECS::InstanceId{2}, 3);

    const ECS::Entity firstBody = fixture.Member(ECS::InstanceId{1}, 0);
    (void)fixture.Member(ECS::InstanceId{1}, 1);
    (void)fixture.Member(ECS::InstanceId{1}, 2);
    const ECS::Entity secondBody = fixture.Member(ECS::InstanceId{2}, 0);
    (void)fixture.Member(ECS::InstanceId{2}, 1);
    (void)fixture.Member(ECS::InstanceId{2}, 2);

    fixture.AssignIds();
    const NetId firstBase  = fixture.server.NetIdOf(firstBody);
    const NetId secondBase = fixture.server.NetIdOf(secondBody);
    REQUIRE(firstBase != InvalidNetId);
    // Adjacent is the ordinary case, not a contrivance: `_nextNetId` only climbs,
    // so two instances placed one after the other always land back to back.
    REQUIRE(secondBase == NetId{firstBase.value + 3});

    FakeExpander *expander = InstallExpander(fixture);

    auto owned     = std::make_unique<PickyProvider>();
    auto *provider  = owned.get();
    provider->named = {firstBase, secondBase};
    fixture.server.SetRelevancyProvider(std::move(owned));
    fixture.Connect();

    REQUIRE(fixture.client.InstanceRecords().size() == 2);
    REQUIRE(fixture.client.ReplicatedEntityCount() == 6);
    REQUIRE(expander->calls == 2);

    // Both leave at once, which is one contiguous despawn run of six.
    provider->named.clear();
    fixture.Step(6);

    // The entities go, as they should...
    CHECK(fixture.client.ReplicatedEntityCount() == 0);
    // ...and so must the records: a record whose members no longer exist is one
    // the client will never re-expand, because the resend is idempotent on the
    // record it still holds.
    CHECK(fixture.client.InstanceRecords().empty());

    // Back in. The server resends both records — they left `knownInstances` when
    // the despawn was acked — so a client that kept them skips the expansion and
    // the six members arrive as bare mirrors with no instance to attribute them
    // to. A client that erased them rebuilds.
    provider->named = {firstBase, secondBase};
    fixture.Step(8);

    CHECK(expander->calls == 4);
    CHECK(fixture.client.ReplicatedEntityCount() == 6);
    CHECK(HasLiveMirror(fixture, firstBase));
    CHECK(HasLiveMirror(fixture, secondBase));
}

TEST_CASE("Blueprint replication: a member pruned before a client joins is not resurrected on it")
{
    // `InstanceInfo::memberCount` is the definition's count, captured once when
    // the block is allocated, so the record carries one presence bit per member
    // to say which of them still exist. Without those bits the client expands all
    // of them and binds `base + i` for every i, and a member destroyed on the
    // host before this connection existed becomes a live phantom here that no
    // despawn names and no delta ever touches.
    Fixture fixture;
    fixture.instances->Add(ECS::InstanceId{1}, 3);

    const ECS::Entity body  = fixture.Member(ECS::InstanceId{1}, 0);
    const ECS::Entity wheel = fixture.Member(ECS::InstanceId{1}, 1);
    (void)fixture.Member(ECS::InstanceId{1}, 2);

    fixture.AssignIds();
    const NetId base = fixture.server.NetIdOf(body);
    REQUIRE(base != InvalidNetId);

    // Pruned on the host before anyone joins. The block keeps its width — the
    // ids of the surviving members must not shift — but the instance is now two
    // members, not three.
    fixture.scene.Destroy(wheel);
    fixture.scene.FlushDestroyed();
    fixture.AssignIds();

    FakeExpander *expander = InstallExpander(fixture);
    fixture.Connect();
    fixture.Step(6);

    REQUIRE(expander->calls == 1);
    CHECK(HasLiveMirror(fixture, base));
    CHECK(HasLiveMirror(fixture, NetId{base.value + 2}));
    // The phantom. Nothing the host has corresponds to it, and nothing will ever
    // tell this client to remove it.
    CHECK_FALSE(HasLiveMirror(fixture, NetId{base.value + 1}));
}

TEST_CASE("Blueprint replication: a stale ack cannot reinstate an instance the client dropped")
{
    // `HandleAck` installs `SentSnapshot::instances` into `knownInstances`
    // wholesale, so forgetting an instance has to scrub the in-flight ring as
    // well — `ForgetAckedInstance`'s job. Left unscrubbed, an ack for a snapshot
    // sent before the instance left puts it back into `knownInstances`, the
    // record is never resent, and the client threw its copy away when the
    // despawn landed.
    Fixture fixture;
    fixture.instances->Add(ECS::InstanceId{1}, 3);

    const ECS::Entity body = fixture.Member(ECS::InstanceId{1}, 0);
    (void)fixture.Member(ECS::InstanceId{1}, 1);
    (void)fixture.Member(ECS::InstanceId{1}, 2);

    fixture.AssignIds();
    const NetId base = fixture.server.NetIdOf(body);
    REQUIRE(base != InvalidNetId);

    FakeExpander *expander = InstallExpander(fixture);

    auto owned     = std::make_unique<PickyProvider>();
    auto *provider  = owned.get();
    provider->named = {base};
    fixture.server.SetRelevancyProvider(std::move(owned));
    fixture.Connect();

    REQUIRE(fixture.client.InstanceRecords().size() == 1);
    REQUIRE(expander->calls == 1);

    // From here the acks are held, so the server's view of what this client has
    // stops advancing on its own — the state this needs is one where an *old* ack
    // arrives after the world has moved on, and an ack stream that keeps up
    // clears the set before it can do any damage.
    fixture.holdAcks = true;
    fixture.Step(3);
    const std::size_t preLeaveAcks = fixture.heldAcks.size();
    REQUIRE(preLeaveAcks >= 1); // at least one snapshot sent while the instance was known

    // Out of relevancy: the run covers the whole block, so the client destroys
    // its members and — this instance is alone, so no neighbouring block shares
    // the run — erases the record.
    provider->named.clear();
    fixture.Step(4);
    REQUIRE(fixture.client.InstanceRecords().empty());
    REQUIRE(fixture.client.ReplicatedEntityCount() == 0);

    // Straight back in, still inside the round trip. The re-entry rule drops the
    // instance from `knownInstances`, so the next snapshot carries the record
    // again — and that snapshot is the one that goes missing.
    provider->named = {base};
    fixture.dropSnapshots = true;
    fixture.Step(4);
    fixture.Poll(); // and the one built on the last of those ticks
    fixture.dropSnapshots = false;
    REQUIRE(fixture.snapshotsDropped >= 1);
    REQUIRE(fixture.client.InstanceRecords().empty()); // the resend really was lost

    // Now the straggler: an ack for a snapshot sent before the instance ever
    // left, carrying the cumulative set from back then.
    fixture.ReleaseHeldAck(0);
    fixture.holdAcks = false;
    fixture.heldAcks.clear();

    fixture.Step(12);

    // The client has to end up holding the record again — by a resend, since it
    // has nothing else to work from. Without it the members arrive as bare
    // mirrors attached to no instance, permanently.
    CHECK(fixture.client.InstanceRecords().size() == 1);
    CHECK(expander->calls == 2);
    CHECK(HasLiveMirror(fixture, base));
}

TEST_CASE("Blueprint replication: the record section pays the snapshot byte budget")
{
    // The record section is paginated against `maxSnapshotBytes`: what fits goes,
    // the rest waits, and no instance is left behind. Written without consulting
    // the budget, a join carrying more fresh instances than it allows produces
    // one oversized packet per snapshot until it is acked, with the entity loop
    // starved behind a section that has already spent everything.
    Net::NetTransport transport;
    ECS::Scene serverScene;
    ECS::Scene clientScene;
    const auto pair = transport.CreateLoopbackPair();

    // Small enough that twenty records cannot possibly share one snapshot with
    // sixty entity blocks: a record is ~52 bytes, so the section alone wants
    // ~1 kB against a 300-byte budget.
    ReplicationConfig config;
    config.maxSnapshotBytes = 300;

    ReplicationServer server{transport, serverScene, nullptr, config};
    ReplicationClient client{transport, clientScene, pair.second};

    constexpr std::uint32_t kInstances = 20;
    constexpr std::uint32_t kMembers   = 3;

    auto ownedInfo = std::make_unique<FakeInstances>();
    for (std::uint32_t instance = 1; instance <= kInstances; ++instance)
        ownedInfo->Add(ECS::InstanceId{instance}, kMembers);
    server.SetInstanceInfoProvider(std::move(ownedInfo));

    auto ownedExpander = std::make_unique<FakeExpander>();
    auto *expander      = ownedExpander.get();
    expander->scene     = &clientScene;
    client.SetInstanceExpander(std::move(ownedExpander));

    for (std::uint32_t instance = 1; instance <= kInstances; ++instance)
    {
        for (std::uint32_t index = 0; index < kMembers; ++index)
        {
            const ECS::Entity entity = serverScene.Create();
            (void)serverScene.Add(entity, ECS::Transform{});
            (void)serverScene.Add(entity, Replicated{});
            (void)serverScene.Add(
                entity, ECS::BlueprintMember{.instanceId = ECS::InstanceId{instance}, .memberIndex = index});
        }
    }

    server.SetContentSetHash(0);
    client.SetContentSetHash(0);
    server.AddConnection(pair.first);

    std::size_t largestSnapshot = 0;
    std::uint64_t tick            = 1;
    for (int step = 0; step < 60; ++step)
    {
        std::vector<Net::NetEvent> events;
        transport.Poll(events);
        for (const Net::NetEvent &event : events)
        {
            if (event.type != Net::NetEvent::Type::Message)
                continue;
            if (event.connection == pair.first)
            {
                server.HandleMessage(pair.first, event.payload);
            }
            else
            {
                largestSnapshot = std::max(largestSnapshot, event.payload.size());
                client.HandleMessage(event.payload);
            }
        }
        server.Tick(tick++);
    }

    // A soft cap, and the overshoot is bounded by what every other section is
    // already allowed: the last record written, the last entity block written,
    // and the event allowance. Blowing past it by a *kilobyte* is not.
    CHECK(largestSnapshot <= config.maxSnapshotBytes + 256);

    // ...and paginating is not dropping. Every instance is named and every member
    // is bound, just over several snapshots instead of one.
    CHECK(client.InstanceRecords().size() == kInstances);
    CHECK(expander->calls == static_cast<int>(kInstances));
    CHECK(client.ReplicatedEntityCount() == kInstances * kMembers);
}

TEST_CASE("Blueprint replication: a member the client never expanded is not elided against the file")
{
    // The hole the presence bits would otherwise open. The authored-value elision
    // says "the client expanded this from the same file, so a component still
    // equal to the file is already correct over there" — true only while the
    // client's copy exists. A member destroyed and then revived was despawned
    // there, and a member that appeared after the record went out was never named
    // in it; both hit the elision's own gate (`sinceChangeTick == 0 &&
    // !clientHasIt`) and would arrive stripped of every authored-equal component,
    // permanently, since nothing re-stamps an unchanging value.
    Fixture fixture;
    fixture.instances->Add(ECS::InstanceId{1}, 3);
    // Every Transform matches the file, which is the strongest form of the case:
    // with the elision unguarded, nothing about this member's Transform is ever
    // sent.
    fixture.instances->matchEverything = true;

    std::vector<ECS::Entity> members;
    for (std::uint32_t index = 0; index < 3; ++index)
        members.push_back(fixture.Member(ECS::InstanceId{1}, index));

    fixture.AssignIds();
    const NetId base = fixture.server.NetIdOf(members[0]);
    REQUIRE(base != InvalidNetId);

    (void)InstallExpander(fixture);
    fixture.Connect();
    fixture.Step(6);
    REQUIRE(HasLiveMirror(fixture, NetId{base.value + 1}));

    // Gone, and the despawn takes the client's copy with it.
    fixture.scene.Destroy(members[1]);
    fixture.scene.FlushDestroyed();
    fixture.Step(6);
    REQUIRE_FALSE(HasLiveMirror(fixture, NetId{base.value + 1}));

    // Back — same instance, same member index, so the block hands it the same
    // NetId. The client has nothing for it: not a mirror, and not an expansion,
    // because the record it holds was never resent.
    const ECS::Entity revived = fixture.Member(ECS::InstanceId{1}, 1);
    if (ECS::Transform *pose = fixture.scene.Get<ECS::Transform>(revived))
        pose->position = {12.f, 0.f, 0.f};
    fixture.Step(8);

    REQUIRE(HasLiveMirror(fixture, NetId{base.value + 1}));
    const ECS::Entity mirror = fixture.client.EntityOf(NetId{base.value + 1});
    const ECS::Transform *const seen  = fixture.clientScene.Get<ECS::Transform>(mirror);
    REQUIRE(seen != nullptr); // unguarded, the elision leaves the mirror with no Transform at all
    CHECK(seen->position.x == doctest::Approx(12.f));
}

TEST_CASE("Blueprint replication: an instance's block is dropped when its last member dies")
{
    // The server's block bookkeeping tracks the live world rather than
    // everything the session has seen: a block, its reverse entry and its range
    // are keyed by an instance, and retire with that instance's last member. The
    // bound this holds is on a session that spawns and destroys instances, where
    // the ranges are binary-searched per relevant entity per connection per
    // snapshot.
    Fixture fixture;
    fixture.instances->Add(ECS::InstanceId{1}, 3);
    fixture.instances->Add(ECS::InstanceId{2}, 3);

    std::vector<ECS::Entity> first;
    std::vector<ECS::Entity> second;
    for (std::uint32_t index = 0; index < 3; ++index)
    {
        first.push_back(fixture.Member(ECS::InstanceId{1}, index));
        second.push_back(fixture.Member(ECS::InstanceId{2}, index));
    }

    (void)InstallExpander(fixture);
    fixture.Connect();
    fixture.Step(6);
    REQUIRE(fixture.server.InstanceBlockCount() == 2);

    const NetId firstBase = fixture.server.NetIdOf(first[0]);
    REQUIRE(firstBase != InvalidNetId);

    // Two of three gone is not gone: the block's width is the definition's, and
    // the survivor's id is derived from a base that must not move under it.
    fixture.scene.Destroy(second[0]);
    fixture.scene.Destroy(second[1]);
    fixture.scene.FlushDestroyed();
    fixture.Step(6);
    CHECK(fixture.server.InstanceBlockCount() == 2);

    fixture.scene.Destroy(second[2]);
    fixture.scene.FlushDestroyed();
    fixture.Step(6);

    // Exactly one, not zero: with two instances in play, a sweep that clears the
    // map wholesale fails here rather than passing for the wrong reason.
    CHECK(fixture.server.InstanceBlockCount() == 1);

    // ...and the survivor is untouched — same base, same block, same ids.
    CHECK(fixture.server.NetIdOf(first[0]) == firstBase);
    CHECK(fixture.server.NetIdOf(first[2]) == NetId{firstBase.value + 2});

    // A retired instance is a stranger, not a hole. One appearing again under a
    // forgotten id allocates a fresh block above everything live, which is what
    // the client — which retired its record on the same event — expands against.
    const ECS::Entity revived = fixture.Member(ECS::InstanceId{2}, 0);
    fixture.Step(6);
    CHECK(fixture.server.InstanceBlockCount() == 2);
    const NetId revivedBase = fixture.server.NetIdOf(revived);
    REQUIRE(revivedBase != InvalidNetId);
    CHECK(revivedBase.value > firstBase.value + 2);
}

TEST_CASE("Blueprint replication: a member destroyed and respawned inside one tick keeps both directions of its id")
{
    // The case above is the *cross-tick* one, where the block had already retired
    // and the instance came back a stranger. This is the same-tick one, and it is
    // the only shape in which a live entity can legitimately ask for an id a dead
    // entity still holds: `PackEntity` carries the generation, so an ordinary
    // replicated entity that comes back is a new key and takes a fresh counter
    // id, colliding with nothing. A blueprint member derives `base + memberIndex`
    // from a block that outlives it, so the id it comes back to is the same one.
    Fixture fixture;
    fixture.instances->Add(ECS::InstanceId{1}, 2);

    const ECS::Entity body = fixture.Member(ECS::InstanceId{1}, 0);
    const ECS::Entity lid  = fixture.Member(ECS::InstanceId{1}, 1);
    fixture.AssignIds();

    const NetId base = fixture.server.NetIdOf(body);
    REQUIRE(base != InvalidNetId);
    REQUIRE(fixture.server.EntityOf(base) == body);

    // Destroyed and rebuilt with no tick in between, so one reconcile pass sees a
    // dead member holding the id and a live one asking for it.
    fixture.scene.Destroy(body);
    fixture.scene.FlushDestroyed();
    const ECS::Entity respawned = fixture.Member(ECS::InstanceId{1}, 0);
    REQUIRE(respawned != body);
    fixture.AssignIds();

    // Both directions, and the reverse is the one that was lost. Checking only
    // `NetIdOf` passes against the defect: the forward row is exactly what
    // survived, and it survived *without* its counterpart.
    CHECK(fixture.server.NetIdOf(respawned) == base);
    CHECK(fixture.server.EntityOf(base) == respawned);

    // The sibling is the control. A repair that rebuilt the maps from scratch
    // would be free to move an id a record already sent describes.
    CHECK(fixture.server.NetIdOf(lid) == NetId{base.value + 1});

    // The instance never stopped existing, so nothing here may retire its block —
    // the retirement pass counts members through the reverse map, and a member
    // missing from it reads as a member that is gone.
    CHECK(fixture.server.InstanceBlockCount() == 1);

    // Nothing later would repair a lost reverse row, so the reconcile has to get
    // it right on the tick: the assign pass writes the reverse map only where the
    // forward map has no row, and the respawned member has one; the cleanup pass
    // walks the reverse map, where it no longer appears. Neither pass can see it.
    fixture.AssignIds();
    fixture.AssignIds();
    CHECK(fixture.server.EntityOf(base) == respawned);
}

TEST_CASE("Blueprint replication: a member respawned before the destroy is flushed takes its id a tick later")
{
    // The same case as it actually reaches a running server. `Scene::Destroy` is
    // deferred to the end of the frame and ReconcileNetIds runs inside the fixed
    // update, so a member destroyed and respawned in one frame is reconciled
    // while *both* claimants of `base + memberIndex` are still alive. Neither can
    // be told from the other — the server cannot see the destroy queue — so the
    // newcomer waits rather than taking an id that is still held.
    Fixture fixture;
    fixture.instances->Add(ECS::InstanceId{1}, 2);

    const ECS::Entity body = fixture.Member(ECS::InstanceId{1}, 0);
    (void)fixture.Member(ECS::InstanceId{1}, 1);
    fixture.AssignIds();

    const NetId base = fixture.server.NetIdOf(body);
    REQUIRE(base != InvalidNetId);

    // Queued, not flushed: `body` is still alive, still replicating, and still
    // the honest answer for `base` on this tick.
    fixture.scene.Destroy(body);
    const ECS::Entity respawned = fixture.Member(ECS::InstanceId{1}, 0);
    fixture.AssignIds();

    CHECK(fixture.server.EntityOf(base) == body);
    CHECK(fixture.server.NetIdOf(respawned) == InvalidNetId);

    // End of frame, and the tick after it. The contention is over, and the id the
    // block reserved for member 0 goes to the member that holds that slot —
    // rather than to a loose id outside the block a record already describes.
    fixture.scene.FlushDestroyed();
    fixture.AssignIds();

    CHECK(fixture.server.NetIdOf(respawned) == base);
    CHECK(fixture.server.EntityOf(base) == respawned);
    CHECK(fixture.server.InstanceBlockCount() == 1);
}

TEST_CASE("Blueprint replication: a member respawned inside one tick goes on replicating")
{
    // What the lost reverse row costs on the wire. Every snapshot resolves the
    // ids it is about through `EntityOf`, so a member with a forward row and no
    // reverse one is in `_liveNetIds`, is never despawned, and is skipped by the
    // entity walk of every snapshot from then on: the client keeps a mirror that
    // no longer receives anything.
    Fixture fixture;
    fixture.instances->Add(ECS::InstanceId{1}, 2);

    const ECS::Entity body = fixture.Member(ECS::InstanceId{1}, 0);
    (void)fixture.Member(ECS::InstanceId{1}, 1);

    (void)InstallExpander(fixture);
    fixture.Connect();
    fixture.Step(6);

    const NetId base = fixture.server.NetIdOf(body);
    REQUIRE(base != InvalidNetId);
    REQUIRE(fixture.client.ReplicatedEntityCount() == 2);
    REQUIRE(HasLiveMirror(fixture, base));

    fixture.scene.Destroy(body);
    fixture.scene.FlushDestroyed();
    const ECS::Entity respawned = fixture.Member(ECS::InstanceId{1}, 0);

    // A position nothing else in this case would produce, so "the mirror caught
    // up" cannot be satisfied by the state the dead member left behind.
    ECS::Transform *moved = fixture.scene.GetMut<ECS::Transform>(respawned);
    REQUIRE(moved != nullptr);
    moved->position = {17.f, 0.f, 0.f};

    fixture.Step(10);

    REQUIRE(HasLiveMirror(fixture, base));
    const ECS::Transform *mirrored = fixture.clientScene.Get<ECS::Transform>(fixture.client.EntityOf(base));
    REQUIRE(mirrored != nullptr);
    CHECK(mirrored->position.x == doctest::Approx(17.f));
}

TEST_CASE("Blueprint replication: without an expander a member arrives missing its authored components")
{
    // What an expander costs to leave out, which is more than assembly. The
    // authored-value elision is gated on the block's `derivable` bits — what the
    // *file* holds — and the server has no signal for whether the peer expanded
    // that file, so the components the expansion would have supplied are exactly
    // the ones never sent, and nothing re-stamps an unchanging value.
    Fixture fixture;
    fixture.instances->Add(ECS::InstanceId{1}, 3);
    // Every component matches the file, which is the strongest form of the case.
    fixture.instances->matchEverything = true;

    std::vector<ECS::Entity> members;
    for (std::uint32_t index = 0; index < 3; ++index)
    {
        const ECS::Entity member = fixture.Member(ECS::InstanceId{1}, index);
        if (ECS::Transform *pose = fixture.scene.Get<ECS::Transform>(member))
            pose->position = {static_cast<float>(index), 0.f, 0.f};
        members.push_back(member);
    }

    fixture.AssignIds();
    const NetId base = fixture.server.NetIdOf(members[0]);
    REQUIRE(base != InvalidNetId);

    // No expander installed. That is the whole case.
    fixture.Connect();
    fixture.Step(8);

    // The record lands and the members arrive as ordinary entities...
    REQUIRE(fixture.client.InstanceRecords().size() == 1);
    REQUIRE(HasLiveMirror(fixture, NetId{base.value + 1}));

    // ...bare, not merely unassembled: a mirror with no Transform is one the
    // renderer cannot place and the inspector shows as an empty row.
    const ECS::Entity mirror = fixture.client.EntityOf(NetId{base.value + 1});
    CHECK(fixture.clientScene.Get<ECS::Transform>(mirror) == nullptr);
}

TEST_CASE("Blueprint replication: Reset forgets the session, not just the mirrors")
{
    // Reset drops the whole session, not only the entities it built: the
    // instance records and both halves of the base/instance translation go with
    // the mirrors. What makes this reachable is a rejoin on the same object,
    // which is the case Reset exists for — NetSession::Disconnect destroys the
    // client instead, and asks nothing of any of it.
    Fixture fixture;
    fixture.instances->Add(ECS::InstanceId{1}, 3);
    std::vector<ECS::Entity> members;
    for (std::uint32_t index = 0; index < 3; ++index)
        members.push_back(fixture.Member(ECS::InstanceId{1}, index));

    FakeExpander *expander = InstallExpander(fixture);
    fixture.Connect();
    fixture.Step(8);
    const NetId base = fixture.server.NetIdOf(members[0]);
    REQUIRE(base != InvalidNetId);
    REQUIRE(fixture.client.InstanceRecords().size() == 1);
    REQUIRE(expander->calls == 1);
    REQUIRE(expander->expandedByBase.contains(base));
    REQUIRE(expander->collapsed.empty());

    fixture.client.Reset();
    fixture.clientScene.FlushDestroyed();

    // The records go with the mirrors. A record left behind is a base that still
    // resolves to an instance whose members are destroyed, and — because a record
    // is expanded only the first time it arrives — an instance the next join
    // never rebuilds.
    CHECK(fixture.client.InstanceRecords().empty());
    CHECK(fixture.client.ReplicatedEntityCount() == 0);

    // And the expander is told, naming the id it chose, so whatever it recorded
    // beside the entities goes too. It is the only thing that can reach those.
    REQUIRE(expander->collapsed.size() == 1);
    CHECK(expander->collapsed[0].value == expander->expandedByBase[base].value);
}

namespace
{

/// One instance record as the record section carries it — only the three fields
/// a sender chooses. The placement is fixed below; none of these cases is about
/// the pose.
struct ForgedRecord
{
    std::uint32_t blueprintIndex = 0;
    NetId base           = InvalidNetId;
    std::uint32_t memberCount    = 0;
};

/// A snapshot packet written by hand, for delivery straight to a client.
///
/// The server allocates ids from a counter that only climbs, so no sequence of
/// calls to it produces a block sitting at the top of the 32-bit space — the
/// encoder cannot be talked into emitting one. But `base` and a despawn run's
/// `start` are just varints on the wire, and a peer picks them. Forging the
/// packet is the only way to put one on this client's wire, which is also
/// precisely what a hostile peer does.
///
/// Same layout as WriteSnapshot: the record section, the despawn runs, then the
/// entity, body and event sections, each of which ends on a `false` bit and so
/// is written here as nothing but its terminator.
std::vector<std::byte> ForgeSnapshot(std::uint64_t serverTick, const std::vector<ForgedRecord> &records,
                                     const std::vector<std::pair<NetId, std::uint32_t>> &despawnRuns)
{
    Core::BitWriter writer;
    WriteMessageType(MessageType::Snapshot, writer);

    SnapshotHeader header;
    header.serverTick = serverTick;
    WriteSnapshotHeader(header, writer);

    writer.WriteBool(!records.empty());
    if (!records.empty())
        writer.WriteVarUInt32(static_cast<std::uint32_t>(records.size()));
    for (const ForgedRecord &record : records)
    {
        writer.WriteVarUInt32(record.blueprintIndex);
        writer.WriteVarUInt32(record.base.value); // wire write
        writer.WriteVarUInt32(record.memberCount);
        writer.WriteBool(true); // every member present, the ordinary case

        const float placement[10] = {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f}; // identity
        for (const float value : placement)
            writer.WriteFloat(value);
    }

    writer.WriteVarUInt32(static_cast<std::uint32_t>(despawnRuns.size()));
    for (const auto &[start, length] : despawnRuns)
    {
        writer.WriteVarUInt32(start.value); // wire write
        writer.WriteVarUInt32(length);
    }

    writer.WriteBool(false); // no entity blocks
    writer.WriteBool(false); // no body states
    writer.WriteBool(false); // no events

    return std::vector<std::byte>(writer.Data().begin(), writer.Data().end());
}

/// A client holding one ordinary mirror at `NetId{1}` — the id both wraps below
/// land on, and the lowest one the server ever hands out, so it belongs to
/// whatever entity was replicated first.
ECS::Entity VictimAtNetIdOne(Fixture &fixture)
{
    const ECS::Entity victim = fixture.Loose();
    fixture.AssignIds();
    REQUIRE(fixture.server.NetIdOf(victim) == NetId{1});

    fixture.Connect();
    fixture.Step(4);
    REQUIRE(HasLiveMirror(fixture, NetId{1}));
    return fixture.client.EntityOf(NetId{1});
}

} // namespace

TEST_CASE("Blueprint replication: a record whose block wraps the id space is refused")
{
    // `base` comes off the wire and is added to once per member, so a bound is
    // needed rather than an `IsValid()` check: `base = 0xFFFFFFFF` with three
    // members binds 0xFFFFFFFF, then 0, then **1**, and that last one is a real
    // entity's mapping overwritten by an instance member. Silent — every later
    // delta for NetId 1 lands on the wrong entity, and nothing says so.
    //
    // 0xFFFFFFFF rather than 0xFFFFFFFE: that one stops at NetId{0}, which the
    // entity path already refuses, so it would pass with the guard deleted.
    Fixture fixture;
    const ECS::Entity mirror   = VictimAtNetIdOne(fixture);
    FakeExpander *expander = InstallExpander(fixture);

    const std::uint64_t rejectedBefore = fixture.client.SnapshotsRejected();
    fixture.client.HandleMessage(ForgeSnapshot(
                                     /*serverTick=*/ 10'000, {ForgedRecord{.blueprintIndex = 0, .base = NetId{0xFFFFFFFFu}, .memberCount = 3}},
                                     {}));

    CHECK(fixture.client.SnapshotsRejected() == rejectedBefore + 1);
    // Refused before the expansion, not after it: a record that cannot be bound
    // must not build entities either, or the members are leaked into the scene
    // with nothing naming them.
    CHECK(expander->calls == 0);
    CHECK(fixture.client.InstanceRecords().empty());

    // The point of the whole thing: somebody else's entity is still somebody
    // else's.
    CHECK(fixture.client.EntityOf(NetId{1}) == mirror);
    CHECK(HasLiveMirror(fixture, NetId{1}));
}

TEST_CASE("Blueprint replication: a despawn run that wraps the id space is refused")
{
    // The same wrap on the run's `start + offset` — lesser than the record's,
    // since it destroys mirrors the server would heal rather than rebinding them,
    // but it is the same unchecked addition on the same attacker-chosen input,
    // and a peer that can delete another entity's mirror at will is a peer that
    // can blank the world.
    Fixture fixture;
    const ECS::Entity mirror = VictimAtNetIdOne(fixture);

    const std::uint64_t rejectedBefore = fixture.client.SnapshotsRejected();
    fixture.client.HandleMessage(ForgeSnapshot(/*serverTick=*/ 10'000, {}, {{NetId{0xFFFFFFFFu}, 3}}));

    CHECK(fixture.client.SnapshotsRejected() == rejectedBefore + 1);
    CHECK(fixture.client.EntityOf(NetId{1}) == mirror);
    CHECK(HasLiveMirror(fixture, NetId{1}));
}

TEST_CASE("Blueprint replication: a block and a run ending at the last id are still honoured")
{
    // The other side of both guards, and the reason they are bounds rather than
    // bans: a block occupying 0xFFFFFFFD..0xFFFFFFFF fits the id space exactly and
    // is legitimate. An off-by-one guard — `>=` where `>` belongs, or comparing
    // `base + memberCount` against the last id rather than one past it — refuses
    // this and passes both cases above.
    Fixture fixture;
    FakeExpander *expander = InstallExpander(fixture);
    fixture.Connect();
    fixture.Step(4);

    constexpr NetId kTopBase{0xFFFFFFFDu};
    const std::uint64_t rejectedBefore = fixture.client.SnapshotsRejected();
    fixture.client.HandleMessage(ForgeSnapshot(
                                     /*serverTick=*/ 10'000, {ForgedRecord{.blueprintIndex = 0, .base = kTopBase, .memberCount = 3}}, {}));

    CHECK(fixture.client.SnapshotsRejected() == rejectedBefore);
    CHECK(expander->calls == 1);
    CHECK(fixture.client.InstanceRecords().size() == 1);
    for (std::uint32_t member = 0; member < 3; ++member)
        CHECK(HasLiveMirror(fixture, NetId{kTopBase.value + member}));

    // ...and the run that takes it away again ends on the last id too.
    fixture.client.HandleMessage(ForgeSnapshot(/*serverTick=*/ 10'001, {}, {{kTopBase, 3}}));

    CHECK(fixture.client.SnapshotsRejected() == rejectedBefore);
    for (std::uint32_t member = 0; member < 3; ++member)
        CHECK_FALSE(HasLiveMirror(fixture, NetId{kTopBase.value + member}));
    CHECK(fixture.client.InstanceRecords().empty());
}
