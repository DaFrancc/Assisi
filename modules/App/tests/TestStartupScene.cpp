/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestStartupScene.cpp
/// @brief What the shipped config names has to become a level the game can
///        actually open, and every way it fails to has to be nameable.
///
/// The game takes no level argument, so this string is the only thing standing
/// between a player and a black window. Each refusal below is a distinct repair:
/// an unnamed scene is a config that was never filled in, an unknown GUID is a
/// scene that was deleted out from under its id, and a missing path is a scene
/// that was never staged. A single "it did not work" would leave all three
/// looking alike.

#include <doctest/doctest.h>

#include <Assisi/App/StartupScene.hpp>
#include <Assisi/Core/AssetDatabase.hpp>
#include <Assisi/Core/AssetSystem.hpp>

#include <expected>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using Assisi::App::ResolveStartupScene;
using Assisi::App::StartupSceneError;

namespace
{

/// A root holding only what each case writes into it, so a resolution that
/// succeeds succeeded on this content and not on the repository's.
std::filesystem::path MountTestRoot()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "assisi-startup-scene";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "levels");
    REQUIRE(Assisi::Core::AssetSystem::SetRoot(root).has_value());
    return root;
}

/// An empty level, enough to exist and be indexed.
void WriteLevel(const std::filesystem::path &root, std::string_view name)
{
    std::ofstream out(root / "levels" / std::string{name});
    REQUIRE(out.good());
    out << R"({ "version": 2, "entities": [], "systems": [] })";
}

/// The sidecar that gives a file its identity. Written by hand rather than
/// minted, because the game indexes ReadOnly and never creates one.
void WriteSidecar(const std::filesystem::path &root, std::string_view name, std::string_view guid)
{
    std::ofstream out(root / "levels" / (std::string{name} + ".aast"));
    REQUIRE(out.good());
    out << R"({ "guid": ")" << guid << R"(", "type": "AssetSidecar", "version": 1 })";
}

/// The editor's answers: ids through the database, paths through the asset root.
std::expected<std::string, StartupSceneError> Resolve(std::string_view named,
                                                      const Assisi::Core::AssetDatabase &database)
{
    return ResolveStartupScene(
        named, [&database](const Assisi::Core::AssetId &id) { return database.PathFor(id); },
        [](std::string_view vpath) { return Assisi::Core::AssetSystem::Exists(vpath); });
}

} // namespace

TEST_CASE("A config that names no startup scene is refused by name")
{
    const std::filesystem::path root = MountTestRoot();
    const Assisi::Core::AssetDatabase database;

    const auto resolved = Resolve("", database);
    REQUIRE_FALSE(resolved.has_value());
    CHECK(resolved.error() == StartupSceneError::Unnamed);

    std::filesystem::remove_all(root);
}

TEST_CASE("A virtual path with no file behind it is Missing, not Unnamed")
{
    const std::filesystem::path root = MountTestRoot();
    const Assisi::Core::AssetDatabase database;

    const auto resolved = Resolve("levels/Absent.alvl", database);
    REQUIRE_FALSE(resolved.has_value());
    CHECK(resolved.error() == StartupSceneError::Missing);

    std::filesystem::remove_all(root);
}

TEST_CASE("A virtual path that exists comes back unchanged")
{
    const std::filesystem::path root = MountTestRoot();
    WriteLevel(root, "Present.alvl");
    const Assisi::Core::AssetDatabase database;

    const auto resolved = Resolve("levels/Present.alvl", database);
    REQUIRE(resolved.has_value());
    CHECK(*resolved == "levels/Present.alvl");

    std::filesystem::remove_all(root);
}

TEST_CASE("A GUID the index does not hold is refused as a GUID")
{
    // Well-formed and unknown, which is the pair that matters: it must not fall
    // through to the path branch and be reported as a missing file, because the
    // repair is different — the scene is somewhere, under another name.
    const std::filesystem::path root = MountTestRoot();
    const Assisi::Core::AssetDatabase database;

    const auto resolved = Resolve("3ad52602-8a8f-4b69-8d56-41cd795cb819", database);
    REQUIRE_FALSE(resolved.has_value());
    CHECK(resolved.error() == StartupSceneError::UnknownGuid);

    std::filesystem::remove_all(root);
}

TEST_CASE("A GUID the index holds resolves to the file carrying it")
{
    const std::filesystem::path root = MountTestRoot();
    WriteLevel(root, "Identified.alvl");
    WriteSidecar(root, "Identified.alvl", "11111111-2222-4333-8444-555555555555");

    Assisi::Core::AssetDatabase database;
    REQUIRE(database.Rebuild(Assisi::Core::RebuildMode::ReadOnly).has_value());

    const auto resolved = Resolve("11111111-2222-4333-8444-555555555555", database);
    REQUIRE(resolved.has_value());
    CHECK(*resolved == "levels/Identified.alvl");

    std::filesystem::remove_all(root);
}
