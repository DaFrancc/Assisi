/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPlayerSessionPlayers.cpp
/// @brief Every player of a session has its own character on every machine, made
///        whole from its blueprint and gone when the player leaves.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>

#include "PlayerSessionTesting.hpp"

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>
#include <Assisi/Runtime/Components.hpp>

using namespace Assisi;
using namespace Assisi::App::Testing;

namespace
{

/// Steps of walking: long enough for commands to cross and be applied.
constexpr std::int32_t kWalkSteps = 60;

/// Steps for a disconnect to reach the host and its despawn to reach a client.
constexpr std::int32_t kLeaveSteps = 90;

/// How far from the same facing two machines may be: a rotation crosses the
/// wire quantized.
constexpr double kFacingTolerance = 1e-3;

/// The host's queue of a settled client's commands: within a tick of the
/// cushion of two the client's clock aims for.
constexpr std::uint32_t kCushionLow = 1;
constexpr std::uint32_t kCushionHigh = 3;

/// Frames for a settled client's controls to reach the host over loopback:
/// the cushion, a frame for the step that samples them, and slack for the
/// transport's own thread.
constexpr std::int32_t kMostFramesForControls = 6;

} // namespace

TEST_CASE("Each player of a session plays its own character, and takes it with them when they leave")
{
    const std::filesystem::path root = FreshRoot("assisi_players_players");
    WritePlayerBlueprint(root);

    Session session;
    Machine &host = session.Add();
    Machine &first = session.Add();
    Machine &second = session.Add();

    // Where players start, in the host's level only: a client is told what to
    // make.
    const ECS::Entity start = host.world.scene.Create();
    (void)host.world.scene.Add<ECS::Transform>(start);
    App::PlayerStart startAt;
    startAt.blueprint = Core::AssetPath{std::string_view{"player.abp"}};
    (void)host.world.scene.Add<App::PlayerStart>(start, startAt);

    const std::uint16_t port = HostOnFreePort(host.play);
    REQUIRE_MESSAGE(port != 0, "could not bind any port in 27240-27279: ", host.play.Session().LastError());
    for (Machine *client : {&first, &second})
    {
        REQUIRE_MESSAGE(client->play.Join("127.0.0.1", port), client->play.Session().LastError());
    }

    session.Run(kJoinSteps);

    // A character each, made at the start and handed over.
    const NetSync::ClientId firstId = first.play.Session().LocalClientId();
    const NetSync::ClientId secondId = second.play.Session().LocalClientId();
    REQUIRE(firstId.IsValid());
    REQUIRE(secondId.IsValid());
    const ECS::Entity hostsOwn = PlayerOf(host, NetSync::HostClientId);
    const ECS::Entity firstsOnHost = PlayerOf(host, firstId);
    const ECS::Entity secondsOnHost = PlayerOf(host, secondId);
    REQUIRE(hostsOwn != ECS::NullEntity);
    REQUIRE(firstsOnHost != ECS::NullEntity);
    REQUIRE(secondsOnHost != ECS::NullEntity);
    CHECK(host.world.instances.Size() == 3);

    // Every machine sees all three, and each is the player of exactly one.
    for (const std::unique_ptr<Machine> &machine : session.machines)
    {
        CHECK(Count<App::Player>(machine->world.scene) == 3);
        CHECK(LocalPlayerOf(machine->world.scene) != ECS::NullEntity);
    }
    const ECS::Entity firstsOwn = LocalPlayerOf(first.world.scene);
    REQUIRE(firstsOwn != ECS::NullEntity);
    REQUIRE(first.world.scene.Get<NetSync::ControlledBy>(firstsOwn) != nullptr);
    CHECK(first.world.scene.Get<NetSync::ControlledBy>(firstsOwn)->client == firstId.value);

    // The client's clock has settled: it never jumped after joining, and the
    // host holds about the cushion of its commands it asks for.
    const NetSync::SessionStats settled = first.play.Session().Stats();
    CHECK(settled.clockCorrections == 0);
    CHECK(settled.inputBufferDepth >= kCushionLow);
    CHECK(settled.inputBufferDepth <= kCushionHigh);

    // The first client walks forward; nobody else touches anything. Its
    // controls reach the host within a few frames.
    const glm::vec3 before = host.world.scene.Get<ECS::Transform>(firstsOnHost)->position;
    std::int32_t frames = 0;
    std::int32_t arrived = -1;
    session.Run(kWalkSteps,
                [&]
    {
        if (App::PlayerInput *controls = first.world.scene.GetMut<App::PlayerInput>(firstsOwn))
        {
            controls->move = {0.f, 1.f};
        }
        const App::PlayerInput *onHost = host.world.scene.Get<App::PlayerInput>(firstsOnHost);
        if (arrived < 0 && onHost != nullptr && onHost->move.y > 0.f)
        {
            arrived = frames;
        }
        ++frames;
    });
    CHECK(arrived >= 0);
    CHECK(arrived <= kMostFramesForControls);

    // Its controls arrived on its own character on the host, and only there.
    REQUIRE(host.world.scene.Get<App::PlayerInput>(firstsOnHost) != nullptr);
    CHECK(host.world.scene.Get<App::PlayerInput>(firstsOnHost)->move.y == doctest::Approx(1.f));
    if (const App::PlayerInput *others = host.world.scene.Get<App::PlayerInput>(secondsOnHost))
    {
        CHECK(others->move.y == doctest::Approx(0.f));
    }
    if (const App::PlayerInput *own = host.world.scene.Get<App::PlayerInput>(hostsOwn))
    {
        CHECK(own->move.y == doctest::Approx(0.f));
    }
    const glm::vec3 after = host.world.scene.Get<ECS::Transform>(firstsOnHost)->position;
    CHECK(glm::length(after - before) > 0.f);

    // It turns round and looks up; the other client sees it face the other way,
    // and its view stays its own.
    const float turn = glm::radians(180.f);
    const float lookUp = glm::radians(20.f);
    bool turned = false;
    session.Run(kWalkSteps,
                [&first, firstsOwn, turn, lookUp, &turned]
    {
        App::PlayerInput *controls = first.world.scene.GetMut<App::PlayerInput>(firstsOwn);
        if (controls != nullptr && !turned)
        {
            controls->lookYaw = turn;
            controls->lookPitch = lookUp;
            turned = true;
        }
    });
    const glm::quat facing = first.world.scene.Get<ECS::Transform>(firstsOwn)->rotation;
    CHECK(std::abs(glm::dot(host.world.scene.Get<ECS::Transform>(firstsOnHost)->rotation, facing)) ==
          doctest::Approx(1.f).epsilon(kFacingTolerance));
    const ECS::Entity firstsOnSecond = second.play.Session().Client()->EntityOf(host.play.Session().Server()->NetIdOf(firstsOnHost));
    REQUIRE(firstsOnSecond != ECS::NullEntity);
    CHECK(std::abs(glm::dot(second.world.scene.Get<ECS::Transform>(firstsOnSecond)->rotation, facing)) ==
          doctest::Approx(1.f).epsilon(kFacingTolerance));

    // Its own camera keeps the look up it gave it, and the other client is
    // shown where it looks.
    CHECK(PitchOfEye(first.world.scene, firstsOwn) == doctest::Approx(lookUp).epsilon(kFacingTolerance));
    CHECK(PitchOfEye(second.world.scene, firstsOnSecond) == doctest::Approx(lookUp).epsilon(kFacingTolerance));

    // The first leaves: its character goes, camera and all, here and for the
    // one still playing.
    first.play.Session().Disconnect();
    session.Run(kLeaveSteps);

    CHECK(PlayerOf(host, firstId) == ECS::NullEntity);
    CHECK(host.world.instances.Size() == 2);
    CHECK(Count<App::Player>(host.world.scene) == 2);
    CHECK(Count<Runtime::Camera>(host.world.scene) == 2);
    CHECK(Count<App::Player>(second.world.scene) == 2);
    CHECK(Count<Runtime::Camera>(second.world.scene) == 2);
}

TEST_CASE("Every player reaches a client as a whole blueprint, the host's own included")
{
    // The host makes its own player on its first step. Sent before the host
    // could name blueprints on the wire, its members arrived as loose entities,
    // and the camera, whose Transform the blueprint supplies, with none at all.
    const std::filesystem::path root = FreshRoot("assisi_players_players");
    WritePlayerBlueprint(root);

    Session session;
    Machine &host = session.Add();
    Machine &client = session.Add();
    JoinOnFloor(session, host, client);

    CHECK(client.world.instances.Size() == 2);
    CHECK(Count<App::Player>(client.world.scene) == 2);
    for (auto [entity, member] : client.world.scene.Query<ECS::BlueprintMember>())
    {
        (void)member;
        CAPTURE(entity.index);
        CHECK(client.world.scene.Has<ECS::Transform>(entity));
    }
}
