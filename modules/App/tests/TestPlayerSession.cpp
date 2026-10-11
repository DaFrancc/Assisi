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

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include <Assisi/App/BlueprintReplication.hpp>
#include <Assisi/App/ContentSet.hpp>
#include <Assisi/App/FixedStepClock.hpp>
#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/PlayerSession.hpp>
#include <Assisi/App/PlayerSystems.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/NetSync/NetSession.hpp>
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
    App::World world{Physics::NoCollisionAssets()};
    NetSync::NetSession session{world.scene, &world.physics};
    App::PlayerSession players;
    Core::EventQueue events;
    App::FixedStepClock clock{kStep, kMaxStepsPerFrame};
    std::uint64_t tick = 0;
};

/// One fixed step on @p machine, in the order the editor runs it.
void Step(Machine &machine, std::uint64_t tick)
{
    machine.session.Poll();
    if (machine.session.IsActive())
    {
        machine.players.BeginStep(machine.session, machine.world, tick);
    }

    App::SystemContext ctx{machine.world, kStep, tick, nullptr, nullptr, machine.events, true, nullptr};
    {
        const ECS::FixedStepScope step(machine.world.scene);
        App::LocalPlayerSystem(ctx);
        App::CharacterCommandSystem(ctx);
        machine.world.physics.Update(kStep);
    }
    machine.world.scene.FlushDestroyed();
    machine.session.AfterPhysicsStep();
    machine.session.Tick(tick, machine.players.LocalCommand());
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
    for (const ECS::Entity entity : host.session.Server()->ControlledEntities(client))
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
                    machine->clock.Advance(static_cast<double>(kStep) * machine->session.TimeScale());
                for (std::uint32_t step = 0; step < steps; ++step)
                {
                    Step(*machine, ++machine->tick);
                }
                // Once a frame, after the steps, as the editor draws.
                machine->session.SmoothView(kStep);
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

std::uint16_t HostOnFreePort(NetSync::NetSession &session)
{
    for (std::uint16_t port = 27240; port < 27280; ++port)
    {
        if (session.Host(port))
        {
            return port;
        }
    }
    return 0;
}

} // namespace

TEST_CASE("Each player of a session plays its own character, and takes it with them when they leave")
{
    const std::filesystem::path root = FreshRoot();
    WritePlayerBlueprint(root);
    const App::ContentSet content = App::BuildContentSet();

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

    for (const std::unique_ptr<Machine> &machine : session.machines)
    {
        machine->players.Attach(machine->session, machine->world);
    }
    const std::uint16_t port = HostOnFreePort(host.session);
    REQUIRE_MESSAGE(port != 0, "could not bind any port in 27240-27279: ", host.session.LastError());
    App::ApplyContentSet(host.session, host.world, content);
    for (Machine *client : {&first, &second})
    {
        REQUIRE_MESSAGE(client->session.Join("127.0.0.1", port), client->session.LastError());
        App::ApplyContentSet(client->session, client->world, content);
    }

    session.Run(kJoinSteps);

    // A character each, made at the start and handed over.
    const NetSync::ClientId firstId = first.session.LocalClientId();
    const NetSync::ClientId secondId = second.session.LocalClientId();
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
    const NetSync::SessionStats settled = first.session.Stats();
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
    const ECS::Entity firstsOnSecond = second.session.Client()->EntityOf(host.session.Server()->NetIdOf(firstsOnHost));
    REQUIRE(firstsOnSecond != ECS::NullEntity);
    CHECK(std::abs(glm::dot(second.world.scene.Get<ECS::Transform>(firstsOnSecond)->rotation, facing)) ==
          doctest::Approx(1.f).epsilon(kFacingTolerance));

    // The look up is the first client's alone: its own camera keeps it, and the
    // other client's copy of that camera is not sent it.
    CHECK(PitchOfEye(first.world.scene, firstsOwn) == doctest::Approx(lookUp).epsilon(kFacingTolerance));
    CHECK(PitchOfEye(second.world.scene, firstsOnSecond) == doctest::Approx(0.f).epsilon(kFacingTolerance));

    // The first leaves: its character goes, camera and all, here and for the
    // one still playing.
    first.session.Disconnect();
    session.Run(kLeaveSteps);

    CHECK(PlayerOf(host, firstId) == ECS::NullEntity);
    CHECK(host.world.instances.Size() == 2);
    CHECK(Count<App::Player>(host.world.scene) == 2);
    CHECK(Count<Runtime::Camera>(host.world.scene) == 2);
    CHECK(Count<App::Player>(second.world.scene) == 2);
    CHECK(Count<Runtime::Camera>(second.world.scene) == 2);
}
