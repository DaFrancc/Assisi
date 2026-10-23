/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPlayerSessionWalls.cpp
/// @brief A client's own character stepped again where it was stepped live
///        while it meets the level: sliding along a wall, stepping onto a ledge
///        and off it, and pushing a crate into a wall.
///
/// The sync test steps every tick a second time and compares it with the first.
/// Any field that comes out different is something a rollback does not put
/// back, which leaves the character off from where the host will have it.

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <memory>

#include "PlayerSessionTesting.hpp"

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>

using namespace Assisi;
using namespace Assisi::App::Testing;

namespace
{

/// Frames each case walks for with the sync test on: long enough to reach
/// what it walks into and stay against it for a few seconds.
constexpr std::int32_t kWalkFrames = 300;

/// A wall ahead of the client's start, across its way and far to either side,
/// so a character walking into it at an angle slides along it the whole time.
constexpr float kWallAhead = 1.5f;
constexpr glm::vec3 kWallHalf{20.f, 1.f, 0.25f};

/// How much of a step forward the client's controls ask for to the side, so it
/// walks into the wall at an angle rather than straight on.
constexpr float kSideways = -0.5f;

/// Nearer the wall's face than this (m), the character is against it: its
/// capsule's radius and the padding the character keeps, with room.
constexpr float kAgainst = 0.5f;

/// A step ahead of the start, lower than the character climbs, and long enough
/// to walk onto and off its far end in the frames given.
constexpr float kStepAhead = 1.f;
constexpr glm::vec3 kStepHalf{2.f, 0.1f, 1.f};

/// Higher than this (m), the character is on the step rather than the floor.
constexpr float kOnStep = 0.15f;

/// A crate between the start and the wall, which the character pushes into it.
constexpr float kCrateAhead = 0.8f;
constexpr float kCrateHalf = 0.3f;
constexpr float kCrateMass = 10.f;

/// A box in the level on every machine, as level geometry is: not sent, each
/// machine has its own.
void AddStaticBox(Session &session, glm::vec3 centre, glm::vec3 half)
{
    for (const std::unique_ptr<Machine> &machine : session.machines)
    {
        const ECS::Entity box = machine->world.scene.Create();
        (void)machine->world.scene.Add<ECS::Transform>(box, ECS::Transform{.position = centre});
        Physics::Collider collider;
        collider.shape = Physics::ColliderShape::Box;
        collider.halfExtents = half;
        (void)machine->world.scene.Add<Physics::Collider>(box, collider);
    }
}

/// A crate on the host at @p at, sent to the client, which simulates it while
/// its player is near.
ECS::Entity AddCrate(ECS::Scene &host, glm::vec3 at)
{
    const ECS::Entity crate = host.Create();
    (void)host.Add<ECS::Transform>(crate, ECS::Transform{.position = at});
    Physics::Collider collider;
    collider.shape = Physics::ColliderShape::Box;
    collider.halfExtents = glm::vec3{kCrateHalf};
    (void)host.Add<Physics::Collider>(crate, collider);
    Physics::RigidBody body;
    body.mass = kCrateMass;
    (void)host.Add<Physics::RigidBody>(crate, body);
    (void)host.Add<NetSync::Replicated>(crate, NetSync::Replicated{});
    return crate;
}

/// Walks @p walker forward, and @p side to the side.
void Walk(ECS::Scene &scene, ECS::Entity walker, float side)
{
    if (App::PlayerInput *controls = scene.GetMut<App::PlayerInput>(walker))
    {
        controls->move = {side, 1.f};
    }
}

/// A host and a client joined on a floor, with the client's player and its
/// start, and the sync test on.
struct Walker
{
    Session session;
    Machine *host = nullptr;
    Machine *client = nullptr;
    ECS::Entity player{ECS::NullEntity};
    glm::vec3 start{0.f};

    explicit Walker(std::string_view root)
    {
        WritePlayerBlueprint(FreshRoot(root));
        host = &session.Add();
        client = &session.Add();
        JoinOnFloor(session, *host, *client);
        player = LocalPlayerOf(client->world.scene);
        REQUIRE(player != ECS::NullEntity);
        start = client->world.scene.Get<ECS::Transform>(player)->position;
    }

    [[nodiscard]] NetSync::ReplicationClient &Replication() { return *client->play.Session().Client(); }
    [[nodiscard]] glm::vec3 Position() { return client->world.scene.Get<ECS::Transform>(player)->position; }
};

} // namespace

TEST_CASE("With the sync test on, a client sliding along a wall steps each tick again as it did live")
{
    Walker walker("assisi_players_walls_slide");
    const float face = walker.start.z - kWallAhead + kWallHalf.z;
    AddStaticBox(walker.session, walker.start + glm::vec3{0.f, kWallHalf.y, -kWallAhead}, kWallHalf);
    walker.session.Run(kCatchUpFrames);
    walker.Replication().SetSyncTest(true);

    std::int32_t framesAgainst = 0;
    walker.session.Run(kWalkFrames,
                       [&]
    {
        Walk(walker.client->world.scene, walker.player, kSideways);
        framesAgainst += walker.Position().z - face < kAgainst ? 1 : 0;
    });

    REQUIRE(framesAgainst > kWalkFrames / 2);
    CHECK(walker.Replication().SyncTestResults().replays > 0u);
    CHECK(walker.Replication().SyncTestResults().differences == 0u);
}

TEST_CASE("With the sync test on, a client stepping onto a ledge and off it steps each tick again as it did live")
{
    Walker walker("assisi_players_walls_ledge");
    AddStaticBox(walker.session, walker.start + glm::vec3{0.f, kStepHalf.y, -kStepAhead - kStepHalf.z}, kStepHalf);
    walker.session.Run(kCatchUpFrames);
    walker.Replication().SetSyncTest(true);

    std::int32_t framesOnStep = 0;
    walker.session.Run(kWalkFrames,
                       [&]
    {
        Walk(walker.client->world.scene, walker.player, 0.f);
        framesOnStep += walker.Position().y > kOnStep ? 1 : 0;
    });

    // Onto it and past its far end, which is where it steps off.
    REQUIRE(framesOnStep > 0);
    REQUIRE(walker.Position().z < walker.start.z - kStepAhead - 2.f * kStepHalf.z);
    CHECK(walker.Replication().SyncTestResults().replays > 0u);
    CHECK(walker.Replication().SyncTestResults().differences == 0u);
}

TEST_CASE("With the sync test on, a client walking into a crate at an angle steps each tick again as it did live")
{
    // It shoves the crate aside and slides off it, so it touches it on edges
    // and corners as well as faces.
    Walker walker("assisi_players_walls_glance");
    ECS::Scene &hostScene = walker.host->world.scene;
    const glm::vec3 crateAt = walker.start + glm::vec3{0.f, kCrateHalf, -kCrateAhead};
    const ECS::Entity crate = AddCrate(hostScene, crateAt);
    walker.session.Run(kCatchUpFrames);
    walker.Replication().SetSyncTest(true);

    walker.session.Run(kWalkFrames, [&] { Walk(walker.client->world.scene, walker.player, kSideways); });

    REQUIRE(glm::length(hostScene.Get<ECS::Transform>(crate)->position - crateAt) > kCrateHalf);
    CHECK(walker.Replication().SyncTestResults().replays > 0u);
    CHECK(walker.Replication().SyncTestResults().differences == 0u);
}

TEST_CASE("With the sync test on, a client pushing a crate into a wall steps each tick again as it did live")
{
    Walker walker("assisi_players_walls_crate");
    AddStaticBox(walker.session, walker.start + glm::vec3{0.f, kWallHalf.y, -kWallAhead}, kWallHalf);
    const ECS::Entity crate =
        AddCrate(walker.host->world.scene, walker.start + glm::vec3{0.f, kCrateHalf, -kCrateAhead});
    walker.session.Run(kCatchUpFrames);
    walker.Replication().SetSyncTest(true);

    walker.session.Run(kWalkFrames, [&] { Walk(walker.client->world.scene, walker.player, 0.f); });

    const ECS::Entity mirror =
        walker.Replication().EntityOf(walker.host->play.Session().Server()->NetIdOf(crate));
    REQUIRE(mirror != ECS::NullEntity);
    REQUIRE(walker.Replication().Simulates(mirror));
    CHECK(walker.Replication().SyncTestResults().replays > 0u);
    CHECK(walker.Replication().SyncTestResults().differences == 0u);
}

namespace
{

/// A crate as NetPush's heavy one is: a metre a side and as dense as water, so
/// a character leans on it rather than shoving it along.
constexpr float kHeavyHalf = 0.5f;
constexpr float kHeavyAhead = 1.5f;

/// The way walked into it: enough to the side to catch its corner and slide
/// round it.
constexpr float kAcrossHeavy = -0.33f;

/// Rollbacks a walk past it may take: the host's copy slides as the client's
/// did, so its word has next to nothing to correct.
constexpr std::uint64_t kMostRollbacksPastHeavy = 3;

} // namespace

TEST_CASE("A client walking into a heavy crate at an angle slides past it, and the host's copy slides with it")
{
    // A crate the client holds still has to reach the host still. Sent creeping
    // at half a quantization step, it pushed the host's copy of the character
    // into its corner and held it there, and the client was put back to it
    // every few ticks.
    Walker walker("assisi_players_walls_heavy");
    const glm::vec3 crateAt = walker.start + glm::vec3{0.f, kHeavyHalf, -kHeavyAhead};
    ECS::Scene &hostScene = walker.host->world.scene;
    const ECS::Entity crate = hostScene.Create();
    (void)hostScene.Add<ECS::Transform>(crate, ECS::Transform{.position = crateAt});
    Physics::Collider collider;
    collider.shape = Physics::ColliderShape::Box;
    collider.halfExtents = glm::vec3{kHeavyHalf};
    (void)hostScene.Add<Physics::Collider>(crate, collider);
    (void)hostScene.Add<Physics::RigidBody>(crate, Physics::RigidBody{});
    (void)hostScene.Add<NetSync::Replicated>(crate, NetSync::Replicated{});
    walker.session.Run(kCatchUpFrames);
    const std::uint64_t rollbacksBefore = walker.Replication().Corrections().predictionsReplayed;

    walker.session.Run(kWalkFrames, [&] { Walk(walker.client->world.scene, walker.player, kAcrossHeavy); });

    CHECK(walker.Position().z < crateAt.z - kHeavyHalf);
    CHECK(walker.Replication().Corrections().predictionsReplayed - rollbacksBefore <= kMostRollbacksPastHeavy);
}
