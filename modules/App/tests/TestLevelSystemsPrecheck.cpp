/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestLevelSystemsPrecheck.cpp
/// @brief The two checks standing between a level and its systems must reach the
/// same verdict.
///
/// `LoadLevelFromPath` asks twice whether a level's systems can be installed.
/// `LevelSystemsAreDeclared` asks *before* anything is touched, reading the names
/// straight out of the file. `WorldManager::ApplySystems` asks again on the far
/// side of the scene replacement, against the names `Load` parsed into the level
/// header. An earlier fix made that late refusal survivable; it did not establish
/// that it cannot fire, and only the early check can refuse cheaply — with the level
/// on screen still the one that was there.
///
/// The hazard is that these are two readers of one array, in two files. Let them
/// disagree about which names a file contains and the disagreement lands exactly
/// in the window between them: a load that clears the gate and then fails past
/// the point of no return. So what is pinned here is the agreement itself, over
/// the inputs where two independently written parsers drift apart first —
/// entries that are not strings, a `systems` key that is not an array, repeats.
///
/// These assertions are the reason `ParseSystemNames` is one function. They fail
/// if it is ever inlined back into its two callers and one of them is edited.

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <Assisi/App/SystemCatalog.hpp>
#include <Assisi/App/TestSystems.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

using Assisi::App::LevelSystemsAreDeclared;
using Assisi::App::World;
using Assisi::App::WorldManager;
using Assisi::Runtime::LevelHeader;
using Assisi::Runtime::SceneSerializer;

namespace
{

std::filesystem::path MountTestRoot()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "assisi-level-systems-precheck";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "levels");
    REQUIRE(Assisi::Core::AssetSystem::SetRoot(root).has_value());
    return root;
}

/// A level whose only interesting content is its `systems` array.
///
/// Written as text rather than through SaveToFile, which can only emit an array
/// of strings — and the entries that are *not* strings are most of the point. A
/// null @p systems writes no key at all, which is its own case.
void WriteLevel(const std::filesystem::path &root, std::string_view name, const nlohmann::json &systems)
{
    nlohmann::json doc;
    doc["version"]  = 2;
    doc["entities"] = nlohmann::json::array();
    if (!systems.is_null())
        doc["systems"] = systems;

    std::ofstream out(root / "levels" / std::string{name});
    REQUIRE(out.good());
    out << doc.dump(2);
}

/// What each of the two routes made of the same file.
struct Verdicts
{
    /// The pre-check, and the list it reached it from.
    bool precheck = false;
    std::vector<std::string> precheckNames;

    /// ApplySystems over the header Load filled, and the list *it* saw.
    bool applied = false;
    std::vector<std::string> headerNames;
};

/// Run the file past both gates the way LoadLevelFromPath does: the pre-check
/// first, then a real load, then ApplySystems over the header that load produced.
Verdicts RunBothChecks(std::string_view virtualPath)
{
    Verdicts verdicts;

    const auto wanted = SceneSerializer::ReadLevelSystems(virtualPath);
    REQUIRE(wanted.has_value());
    verdicts.precheckNames = *wanted;
    verdicts.precheck      = LevelSystemsAreDeclared(virtualPath);

    LevelHeader header;
    Assisi::ECS::Scene scene;
    REQUIRE(SceneSerializer::LoadFromFile(scene, virtualPath, {.header = &header}).has_value());
    verdicts.headerNames = header.systems;

    WorldManager worlds;
    World &world = worlds.Create("Precheck");
    verdicts.applied   = worlds.ApplySystems(world, header.systems, virtualPath);

    return verdicts;
}

/// The invariant, in one place: same names, same answer.
void CheckRoutesAgree(const Verdicts &verdicts)
{
    CHECK(verdicts.precheckNames == verdicts.headerNames);
    CHECK(verdicts.precheck == verdicts.applied);
}

} // namespace

TEST_CASE("The systems pre-check and ApplySystems agree on every shape of the array")
{
    const std::filesystem::path root = MountTestRoot();

    SUBCASE("names this build declares")
    {
        // The ordinary case, and the one that proves the rest are not passing
        // because both routes found nothing.
        WriteLevel(root, "Good.alvl", nlohmann::json::array({"Counter", "Follower"}));
        const Verdicts verdicts = RunBothChecks("levels/Good.alvl");

        CHECK(verdicts.precheckNames == std::vector<std::string>{"Counter", "Follower"});
        CHECK(verdicts.precheck);
        CheckRoutesAgree(verdicts);
    }

    SUBCASE("a name this build does not declare is refused by both")
    {
        // Refused early is the whole design: the level on screen survives. What
        // matters here is that the late check would have refused it too, so the
        // early refusal is not hiding a disagreement.
        WriteLevel(root, "Typo.alvl", nlohmann::json::array({"Counter", "NoSuchSystemAnywhere"}));
        const Verdicts verdicts = RunBothChecks("levels/Typo.alvl");

        CHECK_FALSE(verdicts.precheck);
        CheckRoutesAgree(verdicts);
    }

    SUBCASE("entries that are not strings are skipped identically by both")
    {
        // The sharpest case. Two hand-written parsers diverge here first: one
        // skips the junk, the other coerces it, or refuses the file outright.
        // Either way the pre-check would pass a file ApplySystems then rejects,
        // entered without a single bad *name* in the file.
        WriteLevel(root, "Junk.alvl",
                   nlohmann::json::array({"Counter", 42, nullptr, nlohmann::json::object({{"a", 1}}),
                                          nlohmann::json::array({"Follower"})}));
        const Verdicts verdicts = RunBothChecks("levels/Junk.alvl");

        CHECK(verdicts.precheckNames == std::vector<std::string>{"Counter"});
        CHECK(verdicts.precheck);
        CheckRoutesAgree(verdicts);
    }

    SUBCASE("a systems key that is not an array is ignored by both")
    {
        WriteLevel(root, "NotAnArray.alvl", nlohmann::json("Counter"));
        const Verdicts verdicts = RunBothChecks("levels/NotAnArray.alvl");

        CHECK(verdicts.precheckNames.empty());
        CHECK(verdicts.precheck);
        CheckRoutesAgree(verdicts);
    }

    SUBCASE("no systems key at all")
    {
        WriteLevel(root, "None.alvl", nlohmann::json{});
        const Verdicts verdicts = RunBothChecks("levels/None.alvl");

        CHECK(verdicts.precheckNames.empty());
        CHECK(verdicts.precheck);
        CheckRoutesAgree(verdicts);
    }

    SUBCASE("a repeated name survives to both routes intact")
    {
        // The list is a union at *install* time, not at parse time. A reader that
        // deduplicated on the way out would still install the same systems, so
        // nothing downstream would notice — until the two readers disagreed about
        // the list's length and one of them was the one being compared.
        WriteLevel(root, "Twice.alvl", nlohmann::json::array({"Counter", "Counter"}));
        const Verdicts verdicts = RunBothChecks("levels/Twice.alvl");

        CHECK(verdicts.precheckNames == std::vector<std::string>{"Counter", "Counter"});
        CHECK(verdicts.precheck);
        CheckRoutesAgree(verdicts);
    }
}

TEST_CASE("A level the pre-check cleared installs exactly what it named")
{
    // The pre-check passing has to mean the systems actually arrive. Otherwise
    // "the two routes agree" could be satisfied by both of them reading nothing.
    const std::filesystem::path root = MountTestRoot();
    WriteLevel(root, "Runs.alvl", nlohmann::json::array({"Counter", "Follower"}));

    REQUIRE(LevelSystemsAreDeclared("levels/Runs.alvl"));

    LevelHeader header;
    Assisi::ECS::Scene scene;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "levels/Runs.alvl", {.header = &header}).has_value());

    WorldManager worlds;
    World &world = worlds.Create("Runs");
    REQUIRE(worlds.ApplySystems(world, header.systems, "levels/Runs.alvl"));

    CHECK(world.systems.Has("Counter"));
    CHECK(world.systems.Has("Follower"));
}
