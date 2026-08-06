/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBlueprintOverrides.cpp
/// @brief The merge lattice, and the invariant that instances only shrink.
///
/// Overrides are **recorded, not computed**: one exists because somebody edited
/// that field, and a field nobody touched re-reads from the source on every load.
/// That is what makes "fix it once, fixed everywhere" true, and it is the whole
/// point of the format (docs/blueprint-system-concept.md §5).
///
/// The cases that matter are the ones where two claims meet, because those are
/// the ones that get decided differently at two keyboards: outer beats inner per
/// *field*, a removal beats an outer field edit on the same component, and a
/// re-add starts from C++ defaults rather than from what was deleted.

#include <doctest/doctest.h>

#include <ostream>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/NameComponent.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

using namespace Assisi;
using Assisi::Runtime::BlueprintDefinition;
using Assisi::Runtime::Camera;
using Assisi::Runtime::InstanceTable;
using Assisi::Runtime::SceneSerializer;

namespace
{

std::filesystem::path FreshRoot(const std::string &name)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / ("assisi_bpo_" + name);
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

/// Camera stands in for "a component with more than one field", which is what the
/// merge rules are actually about. Runtime's test link has no physics.
nlohmann::json CameraFields(float fov, bool active)
{
    return {{"fovDegrees", fov}, {"isActive", active}};
}

/// The live entity for member @p name of instance @p id, or NullEntity if this
/// instance does not have it.
ECS::Entity MemberOf(ECS::Scene &scene, const InstanceTable &table, ECS::InstanceId id, std::string_view name)
{
    const Runtime::BlueprintInstance *row = table.Find(id);
    REQUIRE(row != nullptr);
    const BlueprintDefinition *definition = Runtime::GetBlueprintDefinition(row->source);
    REQUIRE(definition != nullptr);
    const std::optional<uint32_t> index = definition->IndexOf(name);
    if (!index.has_value())
        return ECS::NullEntity;

    for (auto [entity, tag] : scene.Query<ECS::BlueprintMember>())
    {
        if (tag.instanceId == id && tag.memberIndex == *index)
            return entity;
    }
    return ECS::NullEntity;
}

/// car.abp: a body carrying a Camera, and a wheel parented to it.
nlohmann::json CarFile()
{
    return {{"version", 2},
            {"entities", nlohmann::json::array({{{"name", "body"},
                                                 {"components", {{"Camera", CameraFields(60.f, true)}}}},
                                                {{"name", "wheel_fl"},
                                                 {"components", {{"Parent", {{"parent", "body"}}}}}}})}};
}

} // namespace

TEST_CASE("Overrides: a level's field claim wins, and the fields it did not name are untouched")
{
    const std::filesystem::path root = FreshRoot("field");
    Write(root, "car.abp", CarFile());
    Write(root, "main.alvl",
          {{"version", 2},
           {"entities", nlohmann::json::array()},
           {"instances", nlohmann::json::array({{{"name", "car_3"},
                                                 {"source", "car.abp"},
                                                 {"overrides", {{"body", {{"Camera", {{"fovDegrees", 90.f}}}}}}}}})}});

    ECS::Scene    scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {}, nullptr, &table));

    const Camera *camera = scene.Get<Camera>(MemberOf(scene, table, ECS::InstanceId{1},"body"));
    REQUIRE(camera != nullptr);
    CHECK(camera->fovDegrees == doctest::Approx(90.f)); // the claim
    CHECK(camera->isActive == true);                    // un-overridden means un-resolved
}

TEST_CASE("Overrides: outermost wins per field, so two levels' claims both apply")
{
    const std::filesystem::path root = FreshRoot("perfield");
    Write(root, "car.abp", CarFile());
    // The lot claims one field...
    Write(root, "lot.abp",
          {{"version", 2},
           {"entities", nlohmann::json::array()},
           {"instances", nlohmann::json::array({{{"name", "car_1"},
                                                 {"source", "car.abp"},
                                                 {"overrides", {{"body", {{"Camera", {{"isActive", false}}}}}}}}})}});
    // ...and the level claims another, of the same component, on the same member.
    Write(root, "main.alvl",
          {{"version", 2},
           {"entities", nlohmann::json::array()},
           {"instances", nlohmann::json::array(
                             {{{"name", "lot_a"},
                               {"source", "lot.abp"},
                               {"overrides", {{"car_1/body", {{"Camera", {{"fovDegrees", 33.f}}}}}}}}})}});

    ECS::Scene    scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {}, nullptr, &table));

    // Both, not one replacing the other's whole object — the reading in which the
    // outer claim wipes the inner set is how someone loses edits.
    const Camera *camera = scene.Get<Camera>(MemberOf(scene, table, ECS::InstanceId{1},"car_1/body"));
    REQUIRE(camera != nullptr);
    CHECK(camera->fovDegrees == doctest::Approx(33.f));
    CHECK(camera->isActive == false);
}

TEST_CASE("Overrides: null removes a component, and the removal beats an outer field claim")
{
    const std::filesystem::path root = FreshRoot("removal");
    Write(root, "car.abp", CarFile());
    // The lot deletes the component...
    Write(root, "lot.abp", {{"version", 2},
                            {"entities", nlohmann::json::array()},
                            {"instances", nlohmann::json::array({{{"name", "car_1"},
                                                                  {"source", "car.abp"},
                                                                  {"overrides",
                                                                   {{"body", {{"Camera", nullptr}}}}}}})}});
    // ...and the level edits a field of it. Neither claim can be honoured.
    Write(root, "main.alvl",
          {{"version", 2},
           {"entities", nlohmann::json::array()},
           {"instances", nlohmann::json::array(
                             {{{"name", "lot_a"},
                               {"source", "lot.abp"},
                               {"overrides", {{"car_1/body", {{"Camera", {{"fovDegrees", 33.f}}}}}}}}})}});

    ECS::Scene    scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {}, nullptr, &table));

    // Removal wins. Resurrecting it from a field edit would silently bring back
    // every *other* field of a component somebody deliberately deleted.
    CHECK(scene.Get<Camera>(MemberOf(scene, table, ECS::InstanceId{1},"car_1/body")) == nullptr);
}

TEST_CASE("Overrides: an add starts from C++ defaults, not from what was deleted")
{
    const std::filesystem::path root = FreshRoot("readd");
    Write(root, "car.abp", CarFile());
    Write(root, "main.alvl",
          {{"version", 2},
           {"entities", nlohmann::json::array()},
           {"instances",
            nlohmann::json::array({{{"name", "car_3"},
                                    {"source", "car.abp"},
                                    // The wheel has no Camera at all; this adds one, naming one field.
                                    {"overrides", {{"wheel_fl", {{"Camera", {{"fovDegrees", 12.f}}}}}}}}})}});

    ECS::Scene    scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {}, nullptr, &table));

    const Camera *camera = scene.Get<Camera>(MemberOf(scene, table, ECS::InstanceId{1},"wheel_fl"));
    REQUIRE(camera != nullptr);
    CHECK(camera->fovDegrees == doctest::Approx(12.f));
    // The body's Camera says isActive=true; this one is a *new* component that
    // happens to share a name, so it says whatever C++ says.
    CHECK(camera->isActive == Camera{}.isActive);
}

TEST_CASE("Overrides: a removed member is not created, and the ones around it keep their index")
{
    const std::filesystem::path root = FreshRoot("shrink");
    Write(root, "car.abp", CarFile());
    Write(root, "main.alvl", {{"version", 2},
                              {"entities", nlohmann::json::array()},
                              {"instances", nlohmann::json::array({{{"name", "car_3"},
                                                                    {"source", "car.abp"},
                                                                    {"removed", {"wheel_fl"}}},
                                                                   {{"name", "car_4"},
                                                                    {"source", "car.abp"}}})}});

    ECS::Scene    scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {}, nullptr, &table));

    // One car lost a wheel; the other did not.
    CHECK(scene.AliveCount() == 3);
    CHECK(MemberOf(scene, table, ECS::InstanceId{1},"wheel_fl") == ECS::NullEntity);
    CHECK(MemberOf(scene, table, ECS::InstanceId{2},"wheel_fl") != ECS::NullEntity);

    // And the survivor kept index 0 rather than sliding down into the hole: the
    // index is the NetId offset, so two instances of one file that removed
    // different members must still agree about which index names which member.
    const ECS::BlueprintMember *body = scene.Get<ECS::BlueprintMember>(MemberOf(scene, table, ECS::InstanceId{1},"body"));
    REQUIRE(body != nullptr);
    CHECK(body->memberIndex == 0);
    const ECS::BlueprintMember *wheel = scene.Get<ECS::BlueprintMember>(MemberOf(scene, table, ECS::InstanceId{2},"wheel_fl"));
    REQUIRE(wheel != nullptr);
    CHECK(wheel->memberIndex == 1);
}

TEST_CASE("Overrides: a reference to a removed member nulls, rather than refusing the file")
{
    const std::filesystem::path root = FreshRoot("dangling");
    Write(root, "car.abp", CarFile());
    Write(root, "main.alvl", {{"version", 2},
                              {"entities", nlohmann::json::array()},
                              {"instances", nlohmann::json::array({{{"name", "car_3"},
                                                                    {"source", "car.abp"},
                                                                    // The wheel's Parent
                                                                    {"removed", {"body"}}}})}});

    ECS::Scene    scene;
    InstanceTable table;
    // A removed member is a legitimate thing for a file to say, unlike a name the
    // file never declared — so the wheel arrives with a null parent, not a refusal.
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {}, nullptr, &table));

    const Runtime::Parent *parent = scene.Get<Runtime::Parent>(MemberOf(scene, table, ECS::InstanceId{1},"wheel_fl"));
    REQUIRE(parent != nullptr);
    CHECK(parent->parent == ECS::NullEntity);
}

TEST_CASE("Overrides: removing a nested instance's name removes everything under it")
{
    const std::filesystem::path root = FreshRoot("subtree");
    Write(root, "car.abp", CarFile());
    Write(root, "lot.abp", {{"version", 2},
                            {"entities", nlohmann::json::array()},
                            {"instances", nlohmann::json::array({{{"name", "car_1"}, {"source", "car.abp"}},
                                                                 {{"name", "car_2"}, {"source", "car.abp"}}})}});
    Write(root, "main.alvl", {{"version", 2},
                              {"entities", nlohmann::json::array()},
                              {"instances", nlohmann::json::array({{{"name", "lot_a"},
                                                                    {"source", "lot.abp"},
                                                                    {"removed", {"car_2"}}}})}});

    ECS::Scene    scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {}, nullptr, &table));

    // A path removes the member it names and everything beneath it, so nothing has
    // to know whether the path named a member or a nested instance.
    CHECK(scene.AliveCount() == 2);
    CHECK(MemberOf(scene, table, ECS::InstanceId{1},"car_1/body") != ECS::NullEntity);
    CHECK(MemberOf(scene, table, ECS::InstanceId{1},"car_2/body") == ECS::NullEntity);
}

TEST_CASE("Overrides: a claim on a member the blueprint no longer declares is dropped, not fatal")
{
    const std::filesystem::path root = FreshRoot("orphan");
    Write(root, "car.abp", CarFile());
    Write(root, "main.alvl",
          {{"version", 2},
           {"entities", nlohmann::json::array()},
           {"instances", nlohmann::json::array({{{"name", "car_3"},
                                                 {"source", "car.abp"},
                                                 {"overrides", {{"spoiler", {{"Camera", {{"fovDegrees", 1.f}}}}}}}}})}});

    // Banning renames is what makes this clean: a missing member can only mean
    // deliberate deletion, so dropping the claim cannot be discarding a real edit.
    ECS::Scene    scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {}, nullptr, &table));
    CHECK(scene.AliveCount() == 2);
}

TEST_CASE("Overrides: a nested file's own claims are baked into its member list")
{
    const std::filesystem::path root = FreshRoot("baked");
    Write(root, "car.abp", CarFile());
    Write(root, "lot.abp",
          {{"version", 2},
           {"entities", nlohmann::json::array()},
           {"instances", nlohmann::json::array({{{"name", "car_1"},
                                                 {"source", "car.abp"},
                                                 {"overrides", {{"body", {{"Camera", {{"fovDegrees", 21.f}}}}}}},
                                                 {"removed", {"wheel_fl"}}}})}});

    // The claims are authored *in* lot.abp, so every instance of the lot has the
    // same member list — and the removal really removes rather than leaving a
    // hole, because there is no per-instance variation for the index to preserve.
    const BlueprintDefinition *definition = Runtime::GetBlueprintDefinition("lot.abp");
    REQUIRE(definition != nullptr);
    REQUIRE(definition->members.size() == 1);
    CHECK(definition->members[0].name == "car_1/body");
    CHECK(definition->members[0].components.at("Camera").at("fovDegrees").get<float>() == doctest::Approx(21.f));
}

// ---------------------------------------------------------------------------
// Reference scope (§6): one rule, two namespaces.
//
//   no leading slash -> the namespace of the thing being addressed
//   leading slash    -> the namespace of the file that wrote the text
//
// For a component authored in its own file those are the same namespace, which
// is why ordinary files never need the slash. They diverge for an override,
// where the writer and the target are different files.
// ---------------------------------------------------------------------------

TEST_CASE("References: a level's override reaches its own entities through a leading slash")
{
    const std::filesystem::path root = FreshRoot("refs_level");
    Write(root, "car.abp", CarFile());
    Write(root, "main.alvl",
          {{"version", 2},
           {"entities", nlohmann::json::array({{{"name", "spawn_marker"}}})},
           {"instances", nlohmann::json::array({{{"name", "car_3"},
                                                 {"source", "car.abp"},
                                                 // Written by the level, so '/' is the level's scope.
                                                 {"overrides",
                                                  {{"wheel_fl", {{"Parent", {{"parent", "/spawn_marker"}}}}}}}}})}});

    ECS::Scene    scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {}, nullptr, &table));

    ECS::Entity marker = ECS::NullEntity;
    for (auto [entity, name] : scene.Query<Runtime::Name>())
    {
        if (name.value.View() == "spawn_marker")
            marker = entity;
    }
    REQUIRE(marker != ECS::NullEntity);

    // Not car_3/spawn_marker, which does not exist and would refuse the file.
    const Runtime::Parent *parent =
        scene.Get<Runtime::Parent>(MemberOf(scene, table, ECS::InstanceId{1}, "wheel_fl"));
    REQUIRE(parent != nullptr);
    CHECK(parent->parent == marker);
}

TEST_CASE("References: a nested file's override resolves in that file, not the instance")
{
    const std::filesystem::path root = FreshRoot("refs_nested");
    Write(root, "car.abp", CarFile());
    // The antenna belongs to this file, and this file writes the claim — so '/'
    // means here, one level below whatever ends up instancing it.
    Write(root, "car_with_antenna.abp",
          {{"version", 2},
           {"entities", nlohmann::json::array({{{"name", "antenna"}}})},
           {"instances", nlohmann::json::array({{{"name", "car"},
                                                 {"source", "car.abp"},
                                                 {"overrides",
                                                  {{"wheel_fl", {{"Parent", {{"parent", "/antenna"}}}}}}}}})}});
    // Wrapped one level deeper on purpose. With car_with_antenna.abp as the root of
    // its own definition its scope and the definition root coincide, and an
    // unqualified '/antenna' resolves correctly by accident — the degenerate input
    // this bug hides behind. Nesting it under a garage makes the writing file's
    // scope `rig/`, which is the thing that has to be remembered.
    Write(root, "garage.abp",
          {{"version", 2},
           {"entities", nlohmann::json::array()},
           {"instances", nlohmann::json::array({{{"name", "rig"}, {"source", "car_with_antenna.abp"}}})}});
    Write(root, "main.alvl",
          {{"version", 2},
           {"entities", nlohmann::json::array()},
           {"instances", nlohmann::json::array({{{"name", "g"}, {"source", "garage.abp"}}})}});

    ECS::Scene    scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {}, nullptr, &table));

    const ECS::Entity antenna = MemberOf(scene, table, ECS::InstanceId{1}, "rig/antenna");
    REQUIRE(antenna != ECS::NullEntity);

    const Runtime::Parent *parent =
        scene.Get<Runtime::Parent>(MemberOf(scene, table, ECS::InstanceId{1}, "rig/car/wheel_fl"));
    REQUIRE(parent != nullptr);
    CHECK(parent->parent == antenna);
}

TEST_CASE("References: inside a file, a leading slash and a plain name mean the same thing")
{
    const std::filesystem::path root = FreshRoot("refs_selfscope");
    // Authored in car.abp's own entity, so the writing file and the addressed file
    // are one and the same. This is the case that makes the '/' marker unambiguous
    // downstream: it has to be resolved away at flatten, or a level-scoped
    // reference and a file's own reference arrive at expansion looking identical.
    Write(root, "slashy.abp",
          {{"version", 2},
           {"entities", nlohmann::json::array({{{"name", "body"}},
                                               {{"name", "wheel_fl"},
                                                {"components", {{"Parent", {{"parent", "/body"}}}}}}})}});
    Write(root, "main.alvl",
          {{"version", 2},
           {"entities", nlohmann::json::array()},
           {"instances", nlohmann::json::array({{{"name", "s"}, {"source", "slashy.abp"}}})}});

    const BlueprintDefinition *definition = Runtime::GetBlueprintDefinition("slashy.abp");
    REQUIRE(definition != nullptr);
    const std::optional<uint32_t> wheel = definition->IndexOf("wheel_fl");
    REQUIRE(wheel.has_value());
    // Resolved at flatten: no slash survives into the definition.
    CHECK(definition->members[*wheel].components.at("Parent").at("parent").get<std::string>() == "body");

    ECS::Scene    scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {}, nullptr, &table));

    const Runtime::Parent *parent =
        scene.Get<Runtime::Parent>(MemberOf(scene, table, ECS::InstanceId{1}, "wheel_fl"));
    REQUIRE(parent != nullptr);
    CHECK(parent->parent == MemberOf(scene, table, ECS::InstanceId{1}, "body"));
}

TEST_CASE("Overrides: a save writes back what was read, and reloading gives the same world")
{
    const std::filesystem::path root = FreshRoot("roundtrip");
    Write(root, "car.abp", CarFile());
    Write(root, "main.alvl",
          {{"version", 2},
           {"entities", nlohmann::json::array()},
           {"instances", nlohmann::json::array({{{"name", "car_3"},
                                                 {"source", "car.abp"},
                                                 {"transform",
                                                  {{"position", {7.f, 0.f, 0.f}},
                                                   {"rotation", {1.f, 0.f, 0.f, 0.f}},
                                                   {"scale", {1.f, 1.f, 1.f}}}},
                                                 {"overrides", {{"body", {{"Camera", {{"fovDegrees", 44.f}}}}}}},
                                                 {"removed", {"wheel_fl"}}}})}});

    ECS::Scene           scene;
    InstanceTable        table;
    Runtime::LevelHeader header;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {}, &header, &table));

    const nlohmann::json saved = SceneSerializer::Save(scene, header, &table);
    REQUIRE(saved.at("instances").size() == 1);
    const nlohmann::json &entry = saved.at("instances")[0];
    CHECK(entry.at("overrides").at("body").at("Camera").at("fovDegrees").get<float>() == doctest::Approx(44.f));
    CHECK(entry.at("removed") == nlohmann::json::array({"wheel_fl"}));

    // And the whole file round-trips: a save that lost the claims would leave a
    // level that reads as a plain car the next time it opens.
    ECS::Scene    reloaded;
    InstanceTable reloadedTable;
    SceneSerializer::Load(reloaded, saved, {}, nullptr, &reloadedTable);
    CHECK(reloaded.AliveCount() == 1);
    const Camera *camera = reloaded.Get<Camera>(MemberOf(reloaded, reloadedTable, ECS::InstanceId{1}, "body"));
    REQUIRE(camera != nullptr);
    CHECK(camera->fovDegrees == doctest::Approx(44.f));
}
