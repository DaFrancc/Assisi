/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "PlayerSessionTesting.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <fstream>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/PlayerSession.hpp>
#include <Assisi/App/StepWorld.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/Components.hpp>

namespace Assisi::App::Testing
{

namespace
{

/// The first of the ports a host here tries, and one past the last.
constexpr std::uint16_t kFirstPort = 27240;
constexpr std::uint16_t kPastLastPort = 27280;

/// How far apart two starts are put (m): well clear of two capsules.
constexpr float kStartsApart = 4.f;

nlohmann::json Pose(float y)
{
    return {{"position", {0.f, y, 0.f}}, {"rotation", {1.f, 0.f, 0.f, 0.f}}, {"scale", {1.f, 1.f, 1.f}}};
}

} // namespace

std::filesystem::path FreshRoot(std::string_view name)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());
    Runtime::ClearBlueprintCache();
    return root;
}

void WritePlayerBlueprint(const std::filesystem::path &root, std::string_view movement)
{
    const nlohmann::json body = {{"Transform", Pose(0.f)},
        {"Character", nlohmann::json::object()},
        {"CharacterLocomotion", nlohmann::json::object()},
        {std::string(movement), nlohmann::json::object()},
        {"Player", nlohmann::json::object()},
        {"Replicated", nlohmann::json::object()}};
    const nlohmann::json eye = {{"Transform", Pose(1.5f)},
        {"Camera", {{"isActive", true}}},
        {"Parent", {{"parent", "Player"}}},
        {"Replicated", nlohmann::json::object()}};
    const nlohmann::json file = {{"version", 2},
        {"entities", nlohmann::json::array({{{"name", "Player"}, {"components", body}},
                                               {{"name", "Eye"}, {"components", eye}}})}};
    std::ofstream out(root / "player.abp", std::ios::binary);
    out << file.dump(2);
    REQUIRE(out.good());
}

Machine::Machine()
{
    REQUIRE(App::WorldManager::ApplySystems(world, std::vector<std::string>{"LocalPlayer", "CharacterCommand"},
                                            "players"));
}

void Step(Machine &machine, std::uint64_t localTick)
{
    const std::uint64_t tick = machine.play.BeginStep(localTick, /*stepping=*/ true);

    // A client here has its world already: it is the same empty one the host
    // started from, plus whatever the test put in it.
    if (machine.play.Session().IsAwaitingLevel())
    {
        machine.play.ConfirmLevelReady();
    }

    machine.play.StepWorld(
        App::SystemContext{machine.world, kStep, tick, nullptr, nullptr, machine.universal, true, nullptr});
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

Machine &Session::Add()
{
    machines.push_back(std::make_unique<Machine>());
    return *machines.back();
}

void Session::Run(std::int32_t frames)
{
    Run(frames, [] {});
}

std::uint16_t HostOnFreePort(App::NetworkedPlay &play, App::HostRole role)
{
    for (std::uint16_t port = kFirstPort; port < kPastLastPort; ++port)
    {
        if (play.Host(port, NetSync::LevelIdentity{}, role))
        {
            return port;
        }
    }
    return 0;
}

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

} // namespace Assisi::App::Testing
