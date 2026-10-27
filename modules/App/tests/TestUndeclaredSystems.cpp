/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestUndeclaredSystems.cpp
/// @brief Content needing a system this build does not declare: reported by the
///        drain, fatal in a game, and named for the editor to refuse saving it.
///
/// A blueprint spawned into a running world and a screen added to one ask for
/// their systems after the content has loaded, so the drain cannot refuse them
/// the way a level load is refused. What it can do is say so, and a game ends.

#include <doctest/doctest.h>

#include <Assisi/App/SystemCatalog.hpp>
#include <Assisi/App/TestStartContext.hpp>
#include <Assisi/App/TestSystems.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/Ui.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <cstdint>
#include <cstring>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <Assisi/Core/Abort.hpp>
#endif

using namespace Assisi::App;
using Assisi::App::Test::StartContext;

namespace
{

/// A name no build declares.
constexpr const char *kMissing = "Missing";

void Write(const std::filesystem::path &path, const nlohmann::json &doc)
{
    std::ofstream out(path, std::ios::binary);
    out << doc.dump(2);
    REQUIRE(out.good());
}

nlohmann::json Placement()
{
    return {{"position", {0.f, 0.f, 0.f}}, {"rotation", {1.f, 0.f, 0.f, 0.f}}, {"scale", {1.f, 1.f, 1.f}}};
}

/// A root holding `car.abp`, which needs Counter and a system no build declares,
/// and `levels/yard.alvl`, which places one car and names @p levelSystems itself.
std::filesystem::path WriteYard(const std::vector<std::string> &levelSystems)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "assisi-undeclared-systems";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "levels");
    REQUIRE(Assisi::Core::AssetSystem::SetRoot(root).has_value());
    Assisi::Runtime::ClearBlueprintCache();

    Write(root / "car.abp",
          {{"version", 2},
              {"systems", nlohmann::json::array({"Counter", kMissing})},
              {"entities",
               nlohmann::json::array({{{"name", "body"}, {"components", {{"Transform", Placement()}}}}})}});
    Write(root / "levels" / "yard.alvl",
          {{"version", 2},
              {"systems", levelSystems},
              {"entities", nlohmann::json::array()},
              {"instances",
               nlohmann::json::array({{{"name", "car_a"}, {"source", "car.abp"}, {"transform", Placement()}}})}});
    return root;
}

/// The yard's scene and instances read into @p world, as a level load reads them,
/// with the level's own system names in @p header.
void ReadYard(World &world, Assisi::Runtime::LevelHeader &header)
{
    REQUIRE(Assisi::Runtime::SceneSerializer::LoadFromFile(world.scene, "levels/yard.alvl",
                                                           {.header = &header, .instances = &world.instances}));
    REQUIRE(world.instances.Size() == 1);
}

} // namespace

TEST_CASE("A spawn asking for an undeclared system is reported with what asked, and its batch is not installed")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Spawned");
    REQUIRE(worlds.ApplySystems(world, {}, "levels/Empty.alvl"));

    QueueSystemInstall(world, std::vector<std::string>{"Counter", kMissing}, "blueprints/car.abp");
    const std::expected<void, UndeclaredSystems> installed = DrainSystemInstalls(StartContext(world));

    REQUIRE_FALSE(installed.has_value());
    CHECK(installed.error().context == "blueprints/car.abp");
    CHECK(installed.error().names == std::vector<std::string>{kMissing});

    // All or nothing, as a level's list is: half of a blueprint's behaviour runs
    // and looks nearly right.
    CHECK_FALSE(world.systems.Has("Counter"));
    CHECK(world.pendingSystems.names.empty());
}

TEST_CASE("A screen asking for an undeclared system is reported the same way, named by the screen")
{
    Assisi::Core::EventQueue universal{Assisi::Core::EventScope::Universal};
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Shown");
    REQUIRE(worlds.ApplySystems(world, {}, "levels/Empty.alvl"));

    Assisi::Mondrian::Ui ui{universal};
    const std::array<std::string, 1> needs{kMissing};
    (void)AddScreen(world,
                    std::make_unique<Assisi::Mondrian::Screen>(ui, world.events, Assisi::Mondrian::ScreenTraits{},
                                                               Assisi::Mondrian::kSortMenu, std::string{"pause"}),
                    needs);

    const std::expected<void, UndeclaredSystems> installed = DrainSystemInstalls(StartContext(world));
    REQUIRE_FALSE(installed.has_value());
    CHECK(installed.error().context == "pause");
    CHECK(installed.error().names == std::vector<std::string>{kMissing});
    CHECK(SourcesRequiring(world, kMissing) == std::vector<std::string>{"pause"});
}

#ifndef _WIN32

namespace
{

/// Where the child's SIGABRT handler writes the reason Abort kept, as a crash
/// handler would read it.
std::int32_t gReasonFile = -1;

void WriteReason(std::int32_t)
{
    const char *reason = Assisi::Core::AbortReason();
    (void)write(gReasonFile, reason, std::strlen(reason));
    close(gReasonFile);
    std::signal(SIGABRT, SIG_DFL);
    std::raise(SIGABRT);
}

} // namespace

TEST_CASE("A game ends on an undeclared system, its crash report naming the system and what asked for it")
{
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Game");
    REQUIRE(worlds.ApplySystems(world, {}, "levels/Empty.alvl"));
    QueueSystemInstall(world, std::vector<std::string>{"Counter", kMissing}, "blueprints/car.abp");

    const std::filesystem::path reasonPath = std::filesystem::temp_directory_path() / "assisi-undeclared-reason.txt";
    std::filesystem::remove(reasonPath);

    // In a child, since the point is that the process does not survive it.
    const pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0)
    {
        gReasonFile = open(reasonPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        std::signal(SIGABRT, WriteReason);
        DrainSystemInstallsOrAbort(StartContext(world));
        _exit(EXIT_SUCCESS);
    }

    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child);
    REQUIRE(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGABRT);

    std::ifstream file(reasonPath);
    const std::string reason{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    CHECK(reason == "'blueprints/car.abp' needs systems this build does not declare: 'Missing'.");
    std::filesystem::remove(reasonPath);
}

#endif

TEST_CASE("Opened for editing, a level installs all but what its placed blueprints need and this build lacks")
{
    const std::filesystem::path root = WriteYard({"Follower"});
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};

    // A game refuses the level whole, so it never runs with the car's behaviour
    // missing.
    World &game = worlds.Create("Game");
    Assisi::Runtime::LevelHeader gameHeader;
    ReadYard(game, gameHeader);
    CHECK_FALSE(worlds.ApplySystems(game, gameHeader.systems, "levels/yard.alvl"));
    CHECK_FALSE(game.systems.Has("Follower"));

    // The editor opens it, so the author can see what is missing.
    World &edited = worlds.Create("Edited");
    Assisi::Runtime::LevelHeader editedHeader;
    ReadYard(edited, editedHeader);
    REQUIRE(worlds.ApplySystems(edited, editedHeader.systems, "levels/yard.alvl", IfUndeclared::Skip));
    CHECK(edited.systems.Has("Follower"));
    CHECK(edited.systems.Has("Counter"));
    CHECK(SourcesRequiring(edited, kMissing) == std::vector<std::string>{"car.abp"});

    std::filesystem::remove_all(root);
}

TEST_CASE("Opened for editing, a level naming an undeclared system itself installs the rest and keeps the name")
{
    const std::filesystem::path root = WriteYard({"Follower", "AlsoMissing"});
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};

    // A game refuses it.
    World &game = worlds.Create("Game");
    Assisi::Runtime::LevelHeader gameHeader;
    ReadYard(game, gameHeader);
    CHECK_FALSE(worlds.ApplySystems(game, gameHeader.systems, "levels/yard.alvl"));

    World &edited = worlds.Create("Edited");
    Assisi::Runtime::LevelHeader header;
    ReadYard(edited, header);
    REQUIRE(worlds.ApplySystems(edited, header.systems, "levels/yard.alvl", IfUndeclared::Skip));
    CHECK(edited.systems.Has("Follower"));
    CHECK(edited.systems.Has("Counter"));

    // Still the level's, so the Systems panel shows it and the save refuses.
    CHECK(edited.systemNames == std::vector<std::string>{"Follower", "AlsoMissing"});
    CHECK(UndeclaredRequiredSystems(edited) == std::vector<std::string>{"AlsoMissing", kMissing});

    std::filesystem::remove_all(root);
}

TEST_CASE("What a file needs and this build lacks: its own names and its placed blueprints', once each, not its screens")
{
    const std::filesystem::path root = WriteYard({"Follower", "AlsoMissing", kMissing});
    Assisi::Core::EventQueue universal{Assisi::Core::EventScope::Universal};
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};

    World &edited = worlds.Create("Edited");
    Assisi::Runtime::LevelHeader header;
    ReadYard(edited, header);
    edited.systemNames = header.systems;

    // A screen is shown at run time and is no part of the file.
    Assisi::Mondrian::Ui ui{universal};
    const std::array<std::string, 1> needs{"ScreenMissing"};
    (void)AddScreen(edited,
                    std::make_unique<Assisi::Mondrian::Screen>(ui, edited.events, Assisi::Mondrian::ScreenTraits{},
                                                               Assisi::Mondrian::kSortMenu, std::string{"pause"}),
                    needs);

    // `Missing` is named by the level and by the car, and listed once.
    CHECK(UndeclaredRequiredSystems(edited) == std::vector<std::string>{"AlsoMissing", kMissing});

    edited.systemNames = {"Follower"};
    CHECK(UndeclaredRequiredSystems(edited) == std::vector<std::string>{kMissing});

    std::filesystem::remove_all(root);
}
