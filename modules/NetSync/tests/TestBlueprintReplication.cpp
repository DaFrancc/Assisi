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
/// Those are the cases here (docs/blueprint-implementation-plan.md, stage 7b,
/// R5/R7).

#include <doctest/doctest.h>

#include <ostream>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Net/NetTransport.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/NetSync/Replication.hpp>

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

    int describeCalls = 0;

  private:
    std::unordered_map<ECS::InstanceId, InstanceInfo> _rows;
};

struct Fixture
{
    Net::NetTransport transport;
    ECS::Scene        scene;
    ReplicationServer server{transport, scene};
    FakeInstances    *instances = nullptr;

    Fixture()
    {
        auto owned = std::make_unique<FakeInstances>();
        instances  = owned.get();
        server.SetInstanceInfoProvider(std::move(owned));
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
    ECS::Scene        scene;
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
    REQUIRE(beyond != InvalidNetId);
    CHECK(beyond.value >= base.value + 2);
}

TEST_CASE("Blueprint replication: the record survives a round trip and is idempotent")
{
    Net::NetTransport transport;
    ECS::Scene        serverScene;
    ECS::Scene        clientScene;
    const auto        pair = transport.CreateLoopbackPair();

    ReplicationServer server{transport, serverScene};
    ReplicationClient client{transport, clientScene, pair.second};

    auto  owned     = std::make_unique<FakeInstances>();
    auto *instances = owned.get();
    instances->Add(ECS::InstanceId{1}, 2, /*blueprintIndex=*/4);
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

TEST_CASE("Blueprint replication: destroying an instance costs one despawn run")
{
    Net::NetTransport transport;
    ECS::Scene        serverScene;
    ECS::Scene        clientScene;
    const auto        pair = transport.CreateLoopbackPair();

    ReplicationServer server{transport, serverScene};
    ReplicationClient client{transport, clientScene, pair.second};

    auto  owned     = std::make_unique<FakeInstances>();
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
    const auto    step = [&](int times)
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
