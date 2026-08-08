/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBlueprintReexpand.cpp
/// @brief Bringing a live instance up to date with an edited file, in place.
///
/// The property under test is identity: a member the edit left alone must come out
/// the other side as the **same entity** — same slot, same generation. That is not a
/// nicety. The editor's undo stack stores exact handles and Scene::ReviveAt is only
/// valid for a free slot, so a re-expansion that recreated its members would leave
/// every earlier transaction pointing at a slot something else now occupies
/// (docs/blueprint-implementation-plan.md, stage 5d).
///
/// The rest of these cases pin what falls out of that: what a deleted member reports,
/// what an added one gets, that the instance's own record survives an edit to the
/// file it names, and that a broken file changes nothing at all.

#include <doctest/doctest.h>

#include <ostream>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/NameComponent.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

using namespace Assisi;
using Assisi::Runtime::BlueprintDefinition;
using Assisi::Runtime::BlueprintResult;
using Assisi::Runtime::InstanceTable;
using Assisi::Runtime::LevelInstance;
using Assisi::Runtime::SceneSerializer;

namespace
{

std::filesystem::path FreshRoot(const std::string &name)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / ("assisi_rx_" + name);
    std::error_code             ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());
    Runtime::ClearBlueprintCache();
    return root;
}

void Write(const std::filesystem::path &root, const std::string &name, const nlohmann::json &doc)
{
    std::ofstream out(root / name, std::ios::binary);
    out << doc.dump(2);
    REQUIRE(out.good());
}

nlohmann::json At(float x, float y, float z)
{
    return {{"Transform",
             {{"position", {x, y, z}}, {"rotation", {1.f, 0.f, 0.f, 0.f}}, {"scale", {1.f, 1.f, 1.f}}}}};
}

nlohmann::json Entity(const std::string &name, nlohmann::json components)
{
    return {{"name", name}, {"components", std::move(components)}};
}

/// A crate with a lid on top.
nlohmann::json CrateFile()
{
    return {{"version", 2},
            {"entities", nlohmann::json::array({Entity("body", At(0.f, 0.f, 0.f)),
                                                Entity("lid", At(0.f, 1.f, 0.f))})}};
}

/// The member names a definition currently declares — what ReexpandInstance needs
/// captured before the cache is dropped, and the one thing the caller cannot
/// reconstruct afterwards.
std::vector<std::string> MemberNames(std::string_view source)
{
    const BlueprintResult loaded = Runtime::GetBlueprintDefinition(source);
    REQUIRE(loaded.has_value());
    const std::shared_ptr<const BlueprintDefinition> &definition = *loaded;
    std::vector<std::string> names;
    names.reserve(definition->members.size());
    for (const auto &member : definition->members)
        names.push_back(member.name);
    return names;
}

ECS::Entity MemberNamed(ECS::Scene &scene, const InstanceTable &table, ECS::InstanceId instanceId,
                        std::string_view memberName)
{
    return Runtime::FindMember(scene, table, instanceId, memberName);
}

LevelInstance CrateAt(std::string name, float x)
{
    LevelInstance entry;
    entry.name               = std::move(name);
    entry.source             = "crate.abp";
    entry.transform.position = {x, 0.f, 0.f};
    return entry;
}

} // namespace

TEST_CASE("Re-expand: a member the edit left alone is the same entity afterwards")
{
    const std::filesystem::path root = FreshRoot("identity");
    Write(root, "crate.abp", CrateFile());

    ECS::Scene    scene;
    InstanceTable table;
    const auto    placed = SceneSerializer::PlaceInstance(scene, table, CrateAt("crate_1", 5.f),
                                                          /*authored=*/true);
    REQUIRE(placed.has_value());

    const ECS::Entity bodyBefore = MemberNamed(scene, table, placed->instanceId, "body");
    const ECS::Entity lidBefore  = MemberNamed(scene, table, placed->instanceId, "lid");
    REQUIRE(bodyBefore != ECS::NullEntity);
    REQUIRE(lidBefore != ECS::NullEntity);

    const std::vector<std::string> before = MemberNames("crate.abp");

    // The edit: the lid moves. Nothing is added or removed.
    Write(root, "crate.abp",
          {{"version", 2},
           {"entities", nlohmann::json::array({Entity("body", At(0.f, 0.f, 0.f)),
                                               Entity("lid", At(0.f, 3.f, 0.f))})}});
    Runtime::InvalidateBlueprint("crate.abp");

    const auto result = SceneSerializer::ReexpandInstance(scene, table, placed->instanceId, before);
    REQUIRE(result.has_value());
    CHECK(result->destroyed.empty());

    // Same handles, generation included — this is the whole point.
    const ECS::Entity bodyAfter = MemberNamed(scene, table, placed->instanceId, "body");
    const ECS::Entity lidAfter  = MemberNamed(scene, table, placed->instanceId, "lid");
    CHECK(bodyAfter == bodyBefore);
    CHECK(lidAfter == lidBefore);
    CHECK(scene.IsAlive(bodyBefore));

    // ...carrying the new value, composed onto the placement as ever.
    const ECS::Transform *lid = scene.Get<ECS::Transform>(lidAfter);
    REQUIRE(lid != nullptr);
    CHECK(lid->position.x == doctest::Approx(5.f));
    CHECK(lid->position.y == doctest::Approx(3.f));
}

TEST_CASE("Re-expand: a deleted member is reported and destroyed, an added one appears")
{
    const std::filesystem::path root = FreshRoot("addremove");
    Write(root, "crate.abp", CrateFile());

    ECS::Scene    scene;
    InstanceTable table;
    const auto    placed = SceneSerializer::PlaceInstance(scene, table, CrateAt("crate_1", 0.f),
                                                          /*authored=*/true);
    REQUIRE(placed.has_value());

    const ECS::Entity bodyBefore = MemberNamed(scene, table, placed->instanceId, "body");
    const ECS::Entity lidBefore  = MemberNamed(scene, table, placed->instanceId, "lid");
    const std::vector<std::string> before = MemberNames("crate.abp");

    // The lid goes, a handle arrives.
    Write(root, "crate.abp",
          {{"version", 2},
           {"entities", nlohmann::json::array({Entity("body", At(0.f, 0.f, 0.f)),
                                               Entity("handle", At(1.f, 0.f, 0.f))})}});
    Runtime::InvalidateBlueprint("crate.abp");

    const auto result = SceneSerializer::ReexpandInstance(scene, table, placed->instanceId, before);
    REQUIRE(result.has_value());

    // Exactly the deleted one, named as an entity rather than as a count — the
    // editor truncates its undo history against this list.
    REQUIRE(result->destroyed.size() == 1);
    CHECK(result->destroyed[0] == lidBefore);

    scene.FlushDestroyed();
    CHECK_FALSE(scene.IsAlive(lidBefore));

    // The survivor kept its handle; the newcomer got one of its own.
    CHECK(MemberNamed(scene, table, placed->instanceId, "body") == bodyBefore);
    const ECS::Entity handle = MemberNamed(scene, table, placed->instanceId, "handle");
    REQUIRE(handle != ECS::NullEntity);
    CHECK(handle != bodyBefore);
    CHECK(handle != lidBefore);

    // ...and the member list is the new one, in the new file's order.
    REQUIRE(result->members.size() == 2);
    CHECK(result->members[0] == bodyBefore);
    CHECK(result->members[1] == handle);
}

TEST_CASE("Re-expand: the instance's own record survives an edit to the file it names")
{
    const std::filesystem::path root = FreshRoot("record");
    Write(root, "crate.abp", CrateFile());

    ECS::Scene    scene;
    InstanceTable table;

    // An instance that moved its own lid — the recorded override the whole format
    // exists to keep separate from the file's value.
    LevelInstance entry     = CrateAt("crate_1", 0.f);
    entry.overrides["lid"]  = {{"Transform",
                                {{"position", {0.f, 9.f, 0.f}},
                                 {"rotation", {1.f, 0.f, 0.f, 0.f}},
                                 {"scale", {1.f, 1.f, 1.f}}}}};
    const auto placed = SceneSerializer::PlaceInstance(scene, table, entry, /*authored=*/true);
    REQUIRE(placed.has_value());

    const std::vector<std::string> before = MemberNames("crate.abp");

    // The file's own lid moves. The instance overrode that member, so the override
    // must still win afterwards — this is "recorded, not computed" surviving a
    // re-expansion.
    Write(root, "crate.abp",
          {{"version", 2},
           {"entities", nlohmann::json::array({Entity("body", At(0.f, 0.f, 0.f)),
                                               Entity("lid", At(0.f, 2.f, 0.f))})}});
    Runtime::InvalidateBlueprint("crate.abp");

    const auto result = SceneSerializer::ReexpandInstance(scene, table, placed->instanceId, before);
    REQUIRE(result.has_value());

    const Runtime::BlueprintInstance *row = table.Find(placed->instanceId);
    REQUIRE(row != nullptr);
    CHECK(row->name == "crate_1");
    CHECK(row->authored);
    CHECK(row->overrides.contains("lid"));

    const ECS::Transform *lid =
        scene.Get<ECS::Transform>(MemberNamed(scene, table, placed->instanceId, "lid"));
    REQUIRE(lid != nullptr);
    CHECK(lid->position.y == doctest::Approx(9.f)); // the override, not the file's 2
}

TEST_CASE("Re-expand: a file that no longer loads changes nothing")
{
    const std::filesystem::path root = FreshRoot("broken");
    Write(root, "crate.abp", CrateFile());

    ECS::Scene    scene;
    InstanceTable table;
    const auto    placed = SceneSerializer::PlaceInstance(scene, table, CrateAt("crate_1", 0.f),
                                                          /*authored=*/true);
    REQUIRE(placed.has_value());

    const ECS::Entity bodyBefore = MemberNamed(scene, table, placed->instanceId, "body");
    const ECS::Entity lidBefore  = MemberNamed(scene, table, placed->instanceId, "lid");
    const std::vector<std::string> before = MemberNames("crate.abp");

    // Two entities of one name: refused by the loader, so there is no new definition
    // to re-expand to.
    Write(root, "crate.abp",
          {{"version", 2},
           {"entities", nlohmann::json::array({Entity("body", At(0.f, 0.f, 0.f)),
                                               Entity("body", At(1.f, 0.f, 0.f))})}});
    Runtime::InvalidateBlueprint("crate.abp");

    CHECK_FALSE(SceneSerializer::ReexpandInstance(scene, table, placed->instanceId, before).has_value());

    // Untouched, not half-rebuilt: the refusal is checked before a member is stripped.
    CHECK(scene.IsAlive(bodyBefore));
    CHECK(scene.IsAlive(lidBefore));
    CHECK(scene.Get<ECS::Transform>(lidBefore) != nullptr);
    CHECK(scene.Get<ECS::BlueprintMember>(bodyBefore) != nullptr);
}

TEST_CASE("Re-expand: an unknown instance is refused")
{
    const std::filesystem::path root = FreshRoot("unknown");
    Write(root, "crate.abp", CrateFile());

    ECS::Scene    scene;
    InstanceTable table;
    CHECK_FALSE(SceneSerializer::ReexpandInstance(scene, table, ECS::InstanceId{7}, {}).has_value());
}
