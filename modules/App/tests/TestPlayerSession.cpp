/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPlayerSession.cpp
/// @brief A host and two clients, each a real session over loopback sockets,
///        each playing its own player.
///
/// Real sessions rather than a hand-driven server and client: what is checked is
/// that a client's controls, stamped with its own clock's tick, are the ones the
/// host's step for that tick reads, and that only the player whose controls they
/// are moves on the host.

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include <Assisi/App/FixedStepClock.hpp>
#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/NetworkedPlay.hpp>
#include <Assisi/App/PlayerSession.hpp>
#include <Assisi/App/StepWorld.hpp>
#include <Assisi/App/SystemCatalog.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/RenderOffset.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/NetSync/NetSession.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/Components.hpp>

using namespace Assisi;

namespace
{

constexpr float kStep = 1.f / 60.f;

/// Steps for a client to connect, say hello and receive everything.
constexpr std::int32_t kJoinSteps = 150;

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

std::filesystem::path FreshRoot()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "assisi_players";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());
    Runtime::ClearBlueprintCache();
    return root;
}

nlohmann::json Pose(float y)
{
    return {{"position", {0.f, y, 0.f}}, {"rotation", {1.f, 0.f, 0.f, 0.f}}, {"scale", {1.f, 1.f, 1.f}}};
}

/// A player: a character with a camera, both sent to every client, the camera
/// without its Transform, which is each machine's own.
void WritePlayerBlueprint(const std::filesystem::path &root)
{
    const nlohmann::json body = {{"Transform", Pose(0.f)},
        {"Character", nlohmann::json::object()},
        {"Player", nlohmann::json::object()},
        {"Replicated", nlohmann::json::object()}};
    const nlohmann::json eye = {{"Transform", Pose(1.5f)},
        {"Camera", {{"isActive", true}}},
        {"Parent", {{"parent", "Player"}}},
        {"Replicated", {{"excluded", {"Transform"}}}}};
    const nlohmann::json file = {{"version", 2},
        {"entities", nlohmann::json::array({{{"name", "Player"}, {"components", body}},
                                               {{"name", "Eye"}, {"components", eye}}})}};
    std::ofstream out(root / "player.abp", std::ios::binary);
    out << file.dump(2);
    REQUIRE(out.good());
}

/// The most fixed steps one machine runs in a frame, as the hosts cap them.
constexpr std::uint32_t kMaxStepsPerFrame = 4;

/// One machine: its world, its session, its players, and its own fixed-step
/// clock and tick, since a client runs a little faster or slower than the host.
struct Machine
{
    /// Runs the systems the editor's play runs a player with.
    Machine()
    {
        REQUIRE(App::WorldManager::ApplySystems(world, std::vector<std::string>{"LocalPlayer", "CharacterCommand"},
                                                "players"));
    }

    App::World world{Physics::NoCollisionAssets()};
    App::NetworkedPlay play{world};
    Core::EventQueue events;
    App::FixedStepClock clock{kStep, kMaxStepsPerFrame};
    /// The tick each step was, in order: the session's, not the local count.
    std::vector<std::uint64_t> stepTicks;
    /// Called after each step with its tick, to watch what the step did.
    std::function<void(std::uint64_t)> afterStep;
    std::uint64_t tick = 0;
    /// While above zero, a step sends nothing and counts this down: the
    /// commands for those ticks never reach the host.
    std::int32_t silentSteps = 0;
};

/// One fixed step on @p machine, as every program runs one.
void Step(Machine &machine, std::uint64_t localTick)
{
    const std::uint64_t tick = machine.play.BeginStep(localTick, /*stepping=*/ true);

    // A client here has its world already: it is the same empty one the host
    // started from, plus whatever the test put in it.
    if (machine.play.Session().IsAwaitingLevel())
    {
        machine.play.ConfirmLevelReady();
    }

    App::StepWorld(App::SystemContext{machine.world, kStep, tick, nullptr, nullptr, machine.events, true, nullptr});
    machine.world.scene.FlushDestroyed();
    if (machine.silentSteps > 0)
    {
        --machine.silentSteps;
    }
    else
    {
        machine.play.EndStep();
    }
    machine.stepTicks.push_back(tick);
    if (machine.afterStep)
    {
        machine.afterStep(tick);
    }
}

template <typename T> std::int32_t Count(ECS::Scene &scene)
{
    std::int32_t count = 0;
    for (auto [entity, component] : scene.Query<T>())
    {
        (void)entity;
        (void)component;
        ++count;
    }
    return count;
}

/// This machine's player, or NullEntity when it has not exactly one.
ECS::Entity LocalPlayerOf(ECS::Scene &scene)
{
    ECS::Entity found = ECS::NullEntity;
    for (auto [entity, local] : scene.Query<App::LocalPlayer>())
    {
        (void)local;
        if (found != ECS::NullEntity)
        {
            return ECS::NullEntity;
        }
        found = entity;
    }
    return found;
}

/// How far up the camera parented to @p character looks, in radians; 0 when
/// it has none.
float PitchOfEye(ECS::Scene &scene, ECS::Entity character)
{
    for (auto [entity, camera, parent] : scene.Query<Runtime::Camera, ECS::Parent>())
    {
        (void)camera;
        if (parent.parent == character)
        {
            const glm::vec3 forward = scene.Get<ECS::Transform>(entity)->rotation * glm::vec3(0.f, 0.f, -1.f);
            return std::asin(glm::clamp(forward.y, -1.f, 1.f));
        }
    }
    return 0.f;
}

/// The host's player for @p client.
ECS::Entity PlayerOf(Machine &host, NetSync::ClientId client)
{
    for (const ECS::Entity entity : host.play.Session().Server()->ControlledEntities(client))
    {
        if (host.world.scene.Has<App::Player>(entity))
        {
            return entity;
        }
    }
    return ECS::NullEntity;
}

/// Machines and the loop that runs them: a frame every step's worth of real
/// time, in which each machine runs as many fixed steps as its session's time
/// scale gives it, as the hosts run them.
struct Session
{
    std::vector<std::unique_ptr<Machine>> machines;

    Machine &Add()
    {
        machines.push_back(std::make_unique<Machine>());
        return *machines.back();
    }

    /// Runs @p frames frames. Real time has to match: these are real sockets,
    /// and a client's clock measures the trip in milliseconds and leads by it
    /// in ticks, so ticks run faster than real time read as commands arriving
    /// late.
    template <typename Before> void Run(std::int32_t frames, Before before)
    {
        using Clock = std::chrono::steady_clock;
        const Clock::duration frame =
            std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(kStep));
        Clock::time_point next = Clock::now();
        for (std::int32_t i = 0; i < frames; ++i)
        {
            before();
            for (const std::unique_ptr<Machine> &machine : machines)
            {
                const std::uint32_t steps =
                    machine->clock.Advance(static_cast<double>(kStep) * machine->play.Session().TimeScale());
                for (std::uint32_t step = 0; step < steps; ++step)
                {
                    Step(*machine, ++machine->tick);
                }
                // Once a frame, after the steps, as the editor draws.
                machine->play.Session().SmoothView(kStep);
            }
            next += frame;
            std::this_thread::sleep_until(next);
        }
    }

    void Run(std::int32_t frames)
    {
        Run(frames, [] {});
    }
};

std::uint16_t HostOnFreePort(App::NetworkedPlay &play)
{
    for (std::uint16_t port = 27240; port < 27280; ++port)
    {
        if (play.Host(port, NetSync::LevelIdentity{}, App::HostRole::Player))
        {
            return port;
        }
    }
    return 0;
}

/// Frames a client walks for while its character is watched.
constexpr std::int32_t kLongWalkFrames = 120;

/// The most frames a client's own character may take to start moving once
/// asked: the step that samples the controls, and one with nothing to step.
constexpr std::size_t kMostFramesToStart = 2;

/// The most a character walking straight on may move back in one frame (m):
/// rounding, not a pull towards where the host had it.
constexpr float kMostBackPerFrame = 0.001f;

/// How far counts as having started to move (m).
constexpr float kStartedMoving = 0.01f;

/// How far apart two starts are put (m): well clear of two capsules.
constexpr float kStartsApart = 4.f;

/// Further than a character walking for kLongWalkFrames falls short of (m).
constexpr float kLongWalkDistance = 1.f;

/// Steps a host runs alone before anyone joins, so its count of steps and a
/// client's own are well apart.
constexpr std::int32_t kHostHeadStart = 100;

/// Frames that see the host step past every tick a client has stepped, and a
/// snapshot about it arrive: the client's lead and the trip back, with room.
constexpr std::int32_t kCatchUpFrames = 30;

/// Steps in a row a client sends nothing: past the host's hold, so the host
/// holds some and lets go of the rest.
constexpr std::int32_t kSilentSteps = static_cast<std::int32_t>(App::kInputHoldTicks) + 2;

/// Frames walked before a gap in commands, so the host holds a walk.
constexpr std::int32_t kWalkBeforeGap = 30;

/// A host and @p client on a floor, a start each for the host to make their
/// characters at, @p client joined and settled.
void JoinOnFloor(Session &session, Machine &host, Machine &client)
{
    // A floor on both machines, as a level puts one there.
    for (const std::unique_ptr<Machine> &machine : session.machines)
    {
        const ECS::Entity floor = machine->world.scene.Create();
        (void)machine->world.scene.Add<ECS::Transform>(floor, ECS::Transform{.position = {0.f, -1.f, 0.f}});
        Physics::Collider collider;
        collider.shape = Physics::ColliderShape::Box;
        collider.halfExtents = {50.f, 1.f, 50.f};
        (void)machine->world.scene.Add<Physics::Collider>(floor, collider);
    }

    // A start each, apart: two characters made on one start push each other
    // apart, and the host, who sees the other where it is, rightly wins that.
    App::PlayerStart startAt;
    startAt.blueprint = Core::AssetPath{std::string_view{"player.abp"}};
    for (const float x : {0.f, kStartsApart})
    {
        const ECS::Entity start = host.world.scene.Create();
        (void)host.world.scene.Add<ECS::Transform>(start, ECS::Transform{.position = {x, 0.f, 0.f}});
        (void)host.world.scene.Add<App::PlayerStart>(start, startAt);
    }

    const std::uint16_t port = HostOnFreePort(host.play);
    REQUIRE_MESSAGE(port != 0, "could not bind any port in 27240-27279: ", host.play.Session().LastError());
    REQUIRE_MESSAGE(client.play.Join("127.0.0.1", port), client.play.Session().LastError());
    session.Run(kJoinSteps);
}

} // namespace

TEST_CASE("A client's own character moves the moment it is asked, and the host's answers never pull it back")
{
    // The host's answer for a tick arrives a round trip after the client
    // stepped it, from before the host had the client's latest commands. Taken
    // as it comes, it puts the character back where it stood, and it walks
    // only once the host has seen it asked to.
    const std::filesystem::path root = FreshRoot();
    WritePlayerBlueprint(root);

    Session session;
    Machine &host = session.Add();
    Machine &client = session.Add();

    JoinOnFloor(session, host, client);

    const ECS::Entity own = LocalPlayerOf(client.world.scene);
    REQUIRE(own != ECS::NullEntity);
    const std::uint64_t correctionsBefore = client.play.Session().Client()->Corrections().applied;

    // Where the client simulates it, a frame at a time: its Transform, without
    // the offset that only hides a correction from view.
    std::vector<glm::vec3> simulated;
    session.Run(kLongWalkFrames,
                [&]
    {
        simulated.push_back(client.world.scene.Get<ECS::Transform>(own)->position);
        if (App::PlayerInput *controls = client.world.scene.GetMut<App::PlayerInput>(own))
        {
            controls->move = {0.f, 1.f};
        }
    });
    simulated.push_back(client.world.scene.Get<ECS::Transform>(own)->position);

    // The host answered all along, and agreed every time.
    const NetSync::ReplicationClient::CorrectionStats &corrections = client.play.Session().Client()->Corrections();
    REQUIRE(corrections.applied > correctionsBefore);
    CHECK(corrections.predictionsKept > 0);
    CHECK(corrections.predictionsReplayed == 0);
    const glm::vec3 travelled = simulated.back() - simulated.front();
    REQUIRE(glm::length(travelled) > kLongWalkDistance);
    const glm::vec3 ahead = glm::normalize(travelled);

    std::size_t started = simulated.size();
    for (std::size_t i = 1; i < simulated.size(); ++i)
    {
        CAPTURE(i);
        CHECK(glm::dot(simulated[i] - simulated[i - 1], ahead) > -kMostBackPerFrame);
        if (started == simulated.size() && glm::dot(simulated[i] - simulated.front(), ahead) > kStartedMoving)
        {
            started = i;
        }
    }
    CHECK(started <= kMostFramesToStart);
}

TEST_CASE("Each player of a session plays its own character, and takes it with them when they leave")
{
    const std::filesystem::path root = FreshRoot();
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

    // The look up is the first client's alone: its own camera keeps it, and the
    // other client's copy of that camera is not sent it.
    CHECK(PitchOfEye(first.world.scene, firstsOwn) == doctest::Approx(lookUp).epsilon(kFacingTolerance));
    CHECK(PitchOfEye(second.world.scene, firstsOnSecond) == doctest::Approx(0.f).epsilon(kFacingTolerance));

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

TEST_CASE("A remote player's controls are held through a short gap in commands and released after it")
{
    App::PlayerInput controls;
    NetSync::InputCommand command;
    command.moveY = 1.f;
    command.actionBits = 0b1u;
    App::RelayHeldCommand(controls, &command, 0);
    REQUIRE(controls.move.y == 1.f);
    REQUIRE(controls.pressed.bits == 0b1u);

    App::RelayHeldCommand(controls, nullptr, App::kInputHoldTicks);
    CHECK(controls.move.y == 1.f);
    CHECK(controls.held.bits == 0b1u);

    App::RelayHeldCommand(controls, nullptr, App::kInputHoldTicks + 1);
    CHECK(controls.move.y == 0.f);
    CHECK(controls.held.bits == 0u);
    // A press the step has not used yet still counts: it happened.
    CHECK(controls.pressed.bits == 0b1u);
}

TEST_CASE("A client's systems step at the host's tick, so a system of the tick alone agrees with the host's")
{
    const std::filesystem::path root = FreshRoot();
    WritePlayerBlueprint(root);

    Session session;
    Machine &host = session.Add();
    Machine &client = session.Add();

    // An oscillator each, never sent: each machine moves its own from the tick.
    std::map<std::uint64_t, float> hostHeights;
    std::map<std::uint64_t, float> clientHeights;
    const std::array<std::map<std::uint64_t, float> *, 2> heights{&hostHeights, &clientHeights};
    for (std::size_t i = 0; i < session.machines.size(); ++i)
    {
        Machine &machine = *session.machines[i];
        REQUIRE(App::SystemCatalog::Instance().Install(machine.world, std::vector<std::string>{"Oscillate"}, "test"));
        const ECS::Entity bob = machine.world.scene.Create();
        (void)machine.world.scene.Add<ECS::Transform>(bob);
        (void)machine.world.scene.Add<Runtime::Oscillator>(bob);
        std::map<std::uint64_t, float> &recorded = *heights[i];
        ECS::Scene &scene = machine.world.scene;
        machine.afterStep = [&recorded, &scene, bob](std::uint64_t tick)
                            { recorded[tick] = scene.Get<ECS::Transform>(bob)->position.y; };
    }

    for (std::int32_t i = 0; i < kHostHeadStart; ++i)
    {
        Step(host, ++host.tick);
    }
    JoinOnFloor(session, host, client);
    session.Run(kCatchUpFrames);

    // Ahead of the host, by the lead its commands need, rather than behind it
    // by the steps the host ran before it joined.
    const std::uint64_t clientTick = client.stepTicks.back();
    CHECK(clientTick > host.stepTicks.back());
    CHECK(client.tick < host.tick);

    session.Run(kCatchUpFrames);
    REQUIRE(hostHeights.contains(clientTick));
    CHECK(clientHeights.at(clientTick) == hostHeights.at(clientTick));
}

TEST_CASE("The host steps a gap in a client's commands on held then released controls, and tells the client which")
{
    const std::filesystem::path root = FreshRoot();
    WritePlayerBlueprint(root);

    Session session;
    Machine &host = session.Add();
    Machine &client = session.Add();
    JoinOnFloor(session, host, client);

    const ECS::Entity own = LocalPlayerOf(client.world.scene);
    const ECS::Entity onHost = PlayerOf(host, client.play.Session().LocalClientId());
    REQUIRE(own != ECS::NullEntity);
    REQUIRE(onHost != ECS::NullEntity);

    std::map<std::uint64_t, float> hostWalk;
    host.afterStep = [&host, &hostWalk, onHost](std::uint64_t tick)
                     {
                         if (const App::PlayerInput *controls = host.world.scene.Get<App::PlayerInput>(onHost))
                         {
                             hostWalk[tick] = controls->move.y;
                         }
                     };
    const std::function<void()> walk = [&client, own]
                                       {
                                           if (App::PlayerInput *controls = client.world.scene.GetMut<App::PlayerInput>(own))
                                           {
                                               controls->move = {0.f, 1.f};
                                           }
                                       };

    session.Run(kWalkBeforeGap, walk);
    const std::size_t gapFrom = client.stepTicks.size();
    client.silentSteps = kSilentSteps;
    session.Run(kSilentSteps + kCatchUpFrames, walk);
    REQUIRE(client.stepTicks.size() > gapFrom + static_cast<std::size_t>(kSilentSteps));

    const NetSync::CommandEcho &echo = client.play.Session().Client()->Echo();
    CHECK(echo.Applied(client.stepTicks[gapFrom - 1]));
    CHECK(echo.Applied(client.stepTicks[gapFrom + static_cast<std::size_t>(kSilentSteps)]));
    for (std::int32_t k = 0; k < kSilentSteps; ++k)
    {
        const std::uint64_t tick = client.stepTicks[gapFrom + static_cast<std::size_t>(k)];
        CAPTURE(k);
        REQUIRE(hostWalk.contains(tick));
        // Held while the gap is short, let go once it is not.
        const bool held = k < static_cast<std::int32_t>(App::kInputHoldTicks);
        CHECK(hostWalk.at(tick) == (held ? 1.f : 0.f));
        CHECK_FALSE(echo.Applied(tick));
    }
}

TEST_CASE("Every player reaches a client as a whole blueprint, the host's own included")
{
    // The host makes its own player on its first step. Sent before the host
    // could name blueprints on the wire, its members arrived as loose entities,
    // and the camera, whose Transform the blueprint supplies, with none at all.
    const std::filesystem::path root = FreshRoot();
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
