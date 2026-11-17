/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestAnimationReplication.cpp
/// @brief What a host sends about an animated character. Its animation
/// components reach a client when it spawns; after that, while game code
/// leaves them alone, the animation systems running every frame send nothing
/// more about them. What each machine works out for itself is kept beside
/// them, and let go when they are removed.

#include <doctest/doctest.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <Assisi/App/AnimationSystems.hpp>
#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/AssetStore.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Net/NetTransport.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>
#include <Assisi/NetSync/ReplicationProviders.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>
#include <Assisi/Physics/CollisionSource.hpp>
#include <Assisi/Runtime/AnimatorGraph.hpp>
#include <Assisi/Runtime/Components.hpp>

using namespace Assisi;

namespace
{

constexpr float kFrame = 1.f / 60.f;

/// Ticks for the handshake, and then for a spawn to arrive and be acked.
constexpr int32_t kJoinTicks = 4;
constexpr int32_t kSettleTicks = 12;

/// Ticks measured while nothing but the animation systems runs.
constexpr int32_t kIdleTicks = 30;

/// What a snapshot with nothing to say may cost: its header and framing, less
/// than one component's worth of payload. NetSync's own idle test holds an
/// unmoved entity to the same bound.
constexpr uint64_t kIdleSnapshotBytes = 24;

std::unique_ptr<Net::NetTransport> MakeTransport()
{
    std::expected<std::unique_ptr<Net::NetTransport>, Net::NetTransportError> created = Net::NetTransport::Create();
    REQUIRE_MESSAGE(created.has_value(), "NetTransport::Create failed: ",
                    created.has_value() ? std::string_view{} : Net::ToString(created.error()));
    return std::move(*created);
}

/// A host and a client over a loopback pair, each running the animation
/// systems every tick as a frame does.
struct Fixture
{
    std::unique_ptr<Net::NetTransport> transport = MakeTransport();
    App::World host{Physics::NoCollisionAssets()};
    App::World guest{Physics::NoCollisionAssets()};
    std::pair<Net::ConnectionId, Net::ConnectionId> pair;
    NetSync::ReplicationServer server;
    NetSync::ReplicationClient client;
    Core::AssetStore assets;
    Core::EventQueue universal{Core::EventScope::Universal};
    uint64_t tick = 0;

    Fixture()
        : pair(transport->CreateLoopbackPair()), server(*transport, host.scene),
        client(*transport, guest.scene, pair.second)
    {
        server.SetContentSetHash(0);
        client.SetContentSetHash(0);
        server.AddConnection(pair.first);
    }

    void Animate(App::World &world)
    {
        App::SystemContext ctx{.world = world,
                               .dt = kFrame,
                               .simTick = tick,
                               .input = nullptr,
                               .actions = nullptr,
                               .universalEvents = universal,
                               .isActiveWorld = true,
                               .worldManager = nullptr,
                               .assets = &assets};
        App::AnimatorSystem(ctx);
        App::AnimationPlayerSystem(ctx);
    }

    void Step(int32_t times)
    {
        for (int32_t i = 0; i < times; ++i)
        {
            std::vector<Net::NetEvent> events;
            transport->Poll(events);
            for (const Net::NetEvent &event : events)
            {
                if (event.type != Net::NetEvent::Type::Message)
                {
                    continue;
                }
                if (event.connection == pair.first)
                {
                    server.HandleMessage(pair.first, event.payload);
                }
                else
                {
                    client.HandleMessage(event.payload);
                }
            }
            Animate(host);
            Animate(guest);
            server.Tick(++tick);
        }
    }

    [[nodiscard]] uint64_t BytesSent() const { return server.Diagnostics(pair.first)->bytesSent; }
    [[nodiscard]] uint64_t SnapshotsSent() const { return server.Diagnostics(pair.first)->snapshotsSent; }
};

/// A character made in code: a skinned mesh with a player and an Animator,
/// marked for replication. Its files are not on disk, so nothing loads and
/// nothing is drawn; the systems still visit it every frame.
ECS::Entity MakeCharacter(ECS::Scene &scene)
{
    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add(entity, ECS::Transform{}) != nullptr);
    REQUIRE(scene.Add(entity, NetSync::Replicated{}) != nullptr);
    Runtime::AnimationPlayer player;
    player.animation = Core::DerivedAssetId("characters/walk.glb");
    REQUIRE(scene.Add(entity, std::move(player)) != nullptr);
    Runtime::Animator animator;
    animator.machine = Core::DerivedAssetId("characters/hero.sgl");
    REQUIRE(scene.Add(entity, std::move(animator)) != nullptr);
    return entity;
}

} // namespace

TEST_CASE("Animation replication: a character's animation reaches a client, then sends nothing while left alone")
{
    Fixture fixture;
    fixture.Step(kJoinTicks);
    const ECS::Entity character = MakeCharacter(fixture.host.scene);
    fixture.Step(kSettleTicks);

    const ECS::Entity mirror = fixture.client.EntityOf(fixture.server.NetIdOf(character));
    REQUIRE(mirror != ECS::NullEntity);
    REQUIRE(fixture.guest.scene.Get<Runtime::SkinnedMesh>(mirror) != nullptr);
    REQUIRE(fixture.guest.scene.Get<Runtime::AnimationPlayer>(mirror) != nullptr);
    REQUIRE(fixture.guest.scene.Get<Runtime::Animator>(mirror) != nullptr);
    CHECK(fixture.guest.scene.Get<Runtime::AnimationPlayer>(mirror)->animation ==
          Core::DerivedAssetId("characters/walk.glb"));
    CHECK(fixture.guest.scene.Get<Runtime::Animator>(mirror)->machine == Core::DerivedAssetId("characters/hero.sgl"));
    // What the client works its own animation out in, made there rather than sent.
    CHECK(fixture.guest.scene.Get<Runtime::SkinnedPose>(mirror) != nullptr);
    CHECK(fixture.guest.scene.Get<Runtime::AnimationPlayback>(mirror) != nullptr);
    CHECK(fixture.guest.scene.Get<Runtime::AnimatorRun>(mirror) != nullptr);

    const uint64_t bytesBefore = fixture.BytesSent();
    const uint64_t snapshotsBefore = fixture.SnapshotsSent();
    fixture.Step(kIdleTicks);
    const uint64_t snapshots = fixture.SnapshotsSent() - snapshotsBefore;
    REQUIRE(snapshots > 0);
    const uint64_t bytesPerSnapshot = (fixture.BytesSent() - bytesBefore) / snapshots;
    INFO("bytes per idle snapshot: ", bytesPerSnapshot);
    CHECK(bytesPerSnapshot < kIdleSnapshotBytes);
}

TEST_CASE("Animation replication: a host that does not animate runs no animation, by its switch rather than a missing store")
{
    App::World world{Physics::NoCollisionAssets()};
    Core::AssetStore assets;
    Core::EventQueue universal{Core::EventScope::Universal};
    const ECS::Entity character = MakeCharacter(world.scene);
    App::SystemContext ctx{.world = world,
                           .dt = kFrame,
                           .simTick = 0,
                           .input = nullptr,
                           .actions = nullptr,
                           .universalEvents = universal,
                           .isActiveWorld = true,
                           .worldManager = nullptr,
                           .assets = &assets,
                           .animates = false};

    // As a headless host: it has assets, and still nothing plays.
    App::AnimatorSystem(ctx);
    App::AnimationPlayerSystem(ctx);
    CHECK(world.scene.Get<Runtime::AnimationPlayback>(character)->request.base.animation.IsNil());

    ctx.animates = true;
    App::AnimationPlayerSystem(ctx);
    CHECK(world.scene.Get<Runtime::AnimationPlayback>(character)->request.base.animation ==
          Core::DerivedAssetId("characters/walk.glb"));
}

TEST_CASE("Animation replication: an Animator removed leaves no bound run behind, so one added back binds again")
{
    App::World world{Physics::NoCollisionAssets()};
    Core::AssetStore assets;
    Core::EventQueue universal{Core::EventScope::Universal};
    App::SystemContext ctx{.world = world,
                           .dt = kFrame,
                           .simTick = 0,
                           .input = nullptr,
                           .actions = nullptr,
                           .universalEvents = universal,
                           .isActiveWorld = true,
                           .worldManager = nullptr,
                           .assets = &assets};
    const ECS::Entity character = MakeCharacter(world.scene);
    Runtime::AnimatorRun *run = world.scene.Get<Runtime::AnimatorRun>(character);
    REQUIRE(run != nullptr);
    // As BindAnimator leaves it once the file has loaded.
    run->graph = std::make_shared<const Runtime::AnimatorGraph>();

    App::AnimationPlayerSystem(ctx);
    CHECK(run->graph != nullptr);

    REQUIRE(world.scene.Remove<Runtime::Animator>(character) == ECS::RemoveResult::Removed);
    App::AnimationPlayerSystem(ctx);
    CHECK(run->graph == nullptr);
}
