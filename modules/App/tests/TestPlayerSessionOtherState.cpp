/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPlayerSessionOtherState.cpp
/// @brief Another player's CharacterState on a client says what the host's
///        does: how it moves and stands, and every jump and landing, even one
///        that happened between the host's updates.

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "PlayerSessionTesting.hpp"

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/BodyState.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>

using namespace Assisi;
using namespace Assisi::App::Testing;

namespace
{

/// Frames for a character to settle into what it was asked, and for every
/// machine to hear so: long enough for a jump to land and an eye to ease.
constexpr std::int32_t kSettleFrames = 90;

/// Steps the host sends nothing for while its player jumps: a second, longer
/// than a jump takes to come down.
constexpr std::int32_t kSilentJumpSteps = 60;

/// How far apart a direction on two machines may be (unit vector): well under
/// a degree.
constexpr float kDirectionTolerance = 0.01f;

/// How far apart an eye height on two machines may be (m).
constexpr float kEyeTolerance = 0.001f;

/// Starts for a host and a client, 4 m apart (m): clear of each other.
constexpr std::array<glm::dvec3, 2> kTwoStarts{glm::dvec3{0.0}, glm::dvec3{4.0, 0.0, 0.0}};

/// The host's player and the client's copy of it, joined on a floor.
struct HostPlayer
{
    ECS::Entity onHost = ECS::NullEntity;
    ECS::Entity onClient = ECS::NullEntity;
};

HostPlayer JoinWithHostPlayer(Session &session, Machine &host, Machine &client)
{
    JoinOnFloor(session, host, client);
    HostPlayer player;
    player.onHost = PlayerOf(host, NetSync::HostClientId);
    player.onClient = client.play.Session().Client()->EntityOf(host.play.Session().Server()->NetIdOf(player.onHost));
    REQUIRE(player.onClient != ECS::NullEntity);
    return player;
}

const Physics::CharacterState &StateOf(Machine &machine, ECS::Entity character)
{
    return *machine.world.scene.Get<Physics::CharacterState>(character);
}

/// A character's counts at some moment.
struct Counts
{
    std::uint32_t jumps = 0;
    std::uint32_t landings = 0;
};

Counts CountsOf(const Physics::CharacterState &state)
{
    return Counts{.jumps = state.jumps, .landings = state.landings};
}

/// Every field of the client's copy, @p onClient, against the host's,
/// @p onHost. An entity is checked for being there or not: each machine has
/// its own. A count is checked for having changed as much on both since
/// @p hostFrom and @p clientFrom: the numbers themselves differ.
void CheckSame(const Physics::CharacterState &onHost, const Physics::CharacterState &onClient, Counts hostFrom,
               Counts clientFrom)
{
    CHECK(onClient.jumps - clientFrom.jumps == onHost.jumps - hostFrom.jumps);
    CHECK(onClient.landings - clientFrom.landings == onHost.landings - hostFrom.landings);
    const float speedTolerance = NetSync::Smoothing().predictionVelocityTolerance;
    CHECK(glm::length(onClient.velocity - onHost.velocity) < speedTolerance);
    CHECK(glm::length(onClient.groundNormal - onHost.groundNormal) < kDirectionTolerance);
    CHECK(glm::length(onClient.groundVelocity - onHost.groundVelocity) < speedTolerance);
    CHECK((onClient.groundEntity == ECS::NullEntity) == (onHost.groundEntity == ECS::NullEntity));
    CHECK((onClient.baseEntity == ECS::NullEntity) == (onHost.baseEntity == ECS::NullEntity));
    CHECK(onClient.timeSinceGrounded == doctest::Approx(onHost.timeSinceGrounded));
    CHECK(onClient.eyeHeight == doctest::Approx(onHost.eyeHeight).epsilon(kEyeTolerance));
    CHECK(std::abs(onClient.groundSpeed - onHost.groundSpeed) < speedTolerance);
    CHECK(onClient.ground == onHost.ground);
    CHECK(onClient.stance == onHost.stance);
    CHECK(onClient.canJump == onHost.canJump);
    CHECK(onClient.sprinting == onHost.sprinting);
}

/// How the host's player is driven through one part of a case.
struct Phase
{
    std::string name;
    glm::vec2 move{0.f};
    App::PlayerButtons held{};
    bool jump = false;
};

/// Drives @p character on @p host as @p phase says, pressing a jump only the
/// first time, which @p pressed remembers.
void Drive(Machine &host, ECS::Entity character, const Phase &phase, bool &pressed)
{
    App::PlayerInput &controls = *host.world.scene.GetMut<App::PlayerInput>(character);
    controls.move = phase.move;
    controls.held = phase.held;
    if (phase.jump && !pressed)
    {
        controls.pressed = App::PlayerButtons::Of(App::PlayerAction::Jump);
        pressed = true;
    }
}

/// A character's counts as last seen, and how many times each has changed.
struct CountChanges
{
    std::uint32_t jumps = 0;
    std::uint32_t landings = 0;
    std::int32_t jumpChanges = 0;
    std::int32_t landingChanges = 0;

    void Saw(const Physics::CharacterState &state)
    {
        jumpChanges += state.jumps != jumps ? 1 : 0;
        landingChanges += state.landings != landings ? 1 : 0;
        jumps = state.jumps;
        landings = state.landings;
    }
};

} // namespace

TEST_CASE("Another player's state on a client matches the host's, standing, sprinting, crouched and after a jump")
{
    const std::filesystem::path root = FreshRoot("assisi_players_other_state");
    WritePlayerBlueprint(root);
    Session session;
    Machine &host = session.Add();
    Machine &client = session.Add();
    const HostPlayer player = JoinWithHostPlayer(session, host, client);

    const App::PlayerButtons sprint = App::PlayerButtons::Of(App::PlayerAction::Sprint);
    const App::PlayerButtons crouch = App::PlayerButtons::Of(App::PlayerAction::Crouch);
    const Phase phases[] = {
        {.name = "standing"},
        {.name = "sprinting", .move = {0.f, 1.f}, .held = sprint},
        {.name = "crouched", .held = crouch},
        {.name = "landed", .jump = true},
    };
    session.Run(kSettleFrames);
    const Counts hostFrom = CountsOf(StateOf(host, player.onHost));
    const Counts clientFrom = CountsOf(StateOf(client, player.onClient));
    for (const Phase &phase : phases)
    {
        CAPTURE(phase.name);
        bool pressed = false;
        session.Run(kSettleFrames, [&] { Drive(host, player.onHost, phase, pressed); });
        CheckSame(StateOf(host, player.onHost), StateOf(client, player.onClient), hostFrom, clientFrom);
    }
    CHECK(StateOf(host, player.onHost).jumps > hostFrom.jumps);
}

TEST_CASE("Another player in the air after a jump cannot jump on a client, as on the host")
{
    // A client moved into the air by the host's word, without jumping itself,
    // read as just off the ground: within coyote time, it could still jump.
    const std::filesystem::path root = FreshRoot("assisi_players_other_state");
    WritePlayerBlueprint(root);
    Session session;
    Machine &host = session.Add();
    Machine &client = session.Add();
    const HostPlayer player = JoinWithHostPlayer(session, host, client);
    session.Run(kSettleFrames);

    std::int32_t inAir = 0;
    std::int32_t couldJumpInAir = 0;
    client.afterStep = [&](std::uint64_t)
                       {
                           const Physics::CharacterState &state = StateOf(client, player.onClient);
                           inAir += state.ground == Physics::GroundState::InAir ? 1 : 0;
                           couldJumpInAir += state.ground == Physics::GroundState::InAir && state.canJump ? 1 : 0;
                       };
    host.world.scene.GetMut<App::PlayerInput>(player.onHost)->pressed =
        App::PlayerButtons::Of(App::PlayerAction::Jump);
    session.Run(kSettleFrames);
    client.afterStep = nullptr;

    REQUIRE(inAir > 0);
    CHECK(couldJumpInAir == 0);
}

TEST_CASE("A client that joins after another player jumped sees no jump as it appears, and the next one")
{
    // The host's counts are its own numbers: taken as they were on the first
    // word about a character, a client's copy jumped as it appeared.
    const std::filesystem::path root = FreshRoot("assisi_players_other_state");
    WritePlayerBlueprint(root);
    Session session;
    Machine &host = session.Add();
    Machine &client = session.Add();
    AddFloors(session, glm::dvec3{0.0});
    App::PlayerStart startAt;
    startAt.blueprint = Core::AssetPath{std::string_view{"player.abp"}};
    for (const glm::dvec3 &at : kTwoStarts)
    {
        const ECS::Entity start = host.world.scene.Create();
        (void)host.world.scene.Add<ECS::Transform>(start, ECS::Transform{.position = at});
        (void)host.world.scene.Add<App::PlayerStart>(start, startAt);
    }
    const std::uint16_t port = HostOnFreePort(host.play);
    REQUIRE(port != 0);
    session.Run(kSettleFrames);
    const ECS::Entity hostOwn = PlayerOf(host, NetSync::HostClientId);
    REQUIRE(hostOwn != ECS::NullEntity);

    // Up and down before anyone joins.
    host.world.scene.GetMut<App::PlayerInput>(hostOwn)->pressed = App::PlayerButtons::Of(App::PlayerAction::Jump);
    session.Run(kSettleFrames);
    REQUIRE(StateOf(host, hostOwn).jumps == 1u);

    REQUIRE(client.play.Join("127.0.0.1", port));
    session.Run(kJoinSteps);
    const ECS::Entity onClient =
        client.play.Session().Client()->EntityOf(host.play.Session().Server()->NetIdOf(hostOwn));
    REQUIRE(onClient != ECS::NullEntity);
    CHECK(StateOf(client, onClient).jumps == 0u);

    host.world.scene.GetMut<App::PlayerInput>(hostOwn)->pressed = App::PlayerButtons::Of(App::PlayerAction::Jump);
    session.Run(kSettleFrames);
    CHECK(StateOf(client, onClient).jumps == 1u);
}

TEST_CASE("A jump and landing between the host's updates each change another player's counts once on a client")
{
    // A client never steps another player's jump: the host's updates move it
    // into the air. A jump that came down before the next one was never in
    // the air there at all.
    const std::filesystem::path root = FreshRoot("assisi_players_other_state");
    WritePlayerBlueprint(root);
    Session session;
    Machine &host = session.Add();
    Machine &client = session.Add();
    const HostPlayer player = JoinWithHostPlayer(session, host, client);
    session.Run(kSettleFrames);

    const std::uint32_t hostJumps = StateOf(host, player.onHost).jumps;
    const std::uint32_t hostLandings = StateOf(host, player.onHost).landings;
    const Physics::CharacterState first = StateOf(client, player.onClient);
    CountChanges seen{.jumps = first.jumps, .landings = first.landings};
    client.afterStep = [&](std::uint64_t) { seen.Saw(StateOf(client, player.onClient)); };

    host.silentSteps = kSilentJumpSteps;
    host.world.scene.GetMut<App::PlayerInput>(player.onHost)->pressed =
        App::PlayerButtons::Of(App::PlayerAction::Jump);
    std::uint32_t landingsInSilence = hostLandings;
    while (host.silentSteps > 0)
    {
        session.Run(1);
        landingsInSilence = StateOf(host, player.onHost).landings;
    }
    session.Run(kCatchUpFrames);
    client.afterStep = nullptr;

    // Down again on the host before it said anything.
    REQUIRE(StateOf(host, player.onHost).jumps == hostJumps + 1u);
    REQUIRE(landingsInSilence == hostLandings + 1u);

    CHECK(seen.jumps - first.jumps == 1u);
    CHECK(seen.landings - first.landings == 1u);
    CHECK(seen.jumpChanges == 1);
    CHECK(seen.landingChanges == 1);
}
