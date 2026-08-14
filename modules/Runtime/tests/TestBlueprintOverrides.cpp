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

#include "LogCapture.hpp"

using namespace Assisi;
using Assisi::Runtime::BlueprintDefinition;
using Assisi::Runtime::BlueprintResult;
using Assisi::Runtime::Camera;
using Assisi::Runtime::InstanceTable;
using Assisi::Runtime::SceneSerializer;

namespace
{

std::filesystem::path FreshRoot(const std::string &name)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / ("assisi_bpo_" + name);
    std::error_code ec;
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
    const BlueprintResult definition = Runtime::GetBlueprintDefinition(row->source);
    REQUIRE(definition.has_value());
    const std::optional<uint32_t> index = (*definition)->IndexOf(name);
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

    ECS::Scene scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {.instances = &table}));

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

    ECS::Scene scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {.instances = &table}));

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

    ECS::Scene scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {.instances = &table}));

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

    ECS::Scene scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {.instances = &table}));

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

    ECS::Scene scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {.instances = &table}));

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

    ECS::Scene scene;
    InstanceTable table;
    // A removed member is a legitimate thing for a file to say, unlike a name the
    // file never declared — so the wheel arrives with a null parent, not a refusal.
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {.instances = &table}));

    const Runtime::Parent *parent = scene.Get<Runtime::Parent>(MemberOf(scene, table, ECS::InstanceId{1},"wheel_fl"));
    REQUIRE(parent != nullptr);
    CHECK(parent->parent == ECS::NullEntity);
}

TEST_CASE("Overrides: a reference orphaned by an in-file removal nulls, exactly as a per-instance one does")
{
    const std::filesystem::path root = FreshRoot("dangling_infile");
    Write(root, "car.abp", CarFile());
    // The same removal as the case above, authored in a file instead of on a
    // placement. It is baked into the member list rather than left as a hole — so
    // the reference car.abp wrote from wheel_fl to body now names nothing at all,
    // where the per-instance removal leaves the name claimed and mapped at nothing.
    Write(root, "bodyless_car.abp", {{"version", 2},
              {"entities", nlohmann::json::array()},
              {"instances", nlohmann::json::array({{{"name", "car"},
                                                      {"source", "car.abp"},
                                                      {"removed", {"body"}}}})}});
    Write(root, "main.alvl",
          {{"version", 2},
              {"entities", nlohmann::json::array()},
              {"instances", nlohmann::json::array({{{"name", "b"}, {"source", "bodyless_car.abp"}}})}});

    const Tests::LogCapture log;

    ECS::Scene scene;
    InstanceTable table;
    // Which path removed the member is not something a reference can see, so it
    // cannot be what decides between "null it" and "this file is unusable". Refusing
    // here takes down every instance of bodyless_car.abp and the level with them.
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {.instances = &table}));

    const Runtime::Parent *parent =
        scene.Get<Runtime::Parent>(MemberOf(scene, table, ECS::InstanceId{1}, "car/wheel_fl"));
    REQUIRE(parent != nullptr);
    CHECK(parent->parent == ECS::NullEntity);

    // Null *with a warning*: a silent null and a warned one leave identical worlds,
    // and the whole value of the decision is that whoever removed the member finds
    // out they broke a wire.
    CHECK(log.Mentions("'car/body', which was removed"));
}

TEST_CASE("Overrides: a level's reference into a member an inner file removed nulls, not refuses")
{
    const std::filesystem::path root = FreshRoot("dangling_level");
    Write(root, "car.abp", CarFile());
    // wheel_fl is the member nothing inside car.abp references, so the definition
    // itself is intact and this is only about the name the *level* asks for.
    Write(root, "wheelless_car.abp", {{"version", 2},
              {"entities", nlohmann::json::array()},
              {"instances", nlohmann::json::array({{{"name", "car"},
                                                      {"source", "car.abp"},
                                                      {"removed", {"wheel_fl"}}}})}});
    Write(root, "main.alvl",
          {{"version", 2},
              {"entities", nlohmann::json::array({{{"name", "watcher"},
                                                     {"components", {{"Parent", {{"parent", "w/car/wheel_fl"}}}}}}})},
              {"instances", nlohmann::json::array({{{"name", "w"}, {"source", "wheelless_car.abp"}}})}});

    const Tests::LogCapture log;

    ECS::Scene scene;
    InstanceTable table;
    // The level names a member that a file it does not own decided to remove. That
    // is the level being out of date, not the level being corrupt.
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {.instances = &table}));

    ECS::Entity watcher = ECS::NullEntity;
    for (auto [entity, name] : scene.Query<Runtime::Name>())
    {
        if (name.value.View() == "watcher")
            watcher = entity;
    }
    REQUIRE(watcher != ECS::NullEntity);

    const Runtime::Parent *parent = scene.Get<Runtime::Parent>(watcher);
    REQUIRE(parent != nullptr);
    CHECK(parent->parent == ECS::NullEntity);
    CHECK(log.Mentions("'w/car/wheel_fl', which was removed"));
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

    ECS::Scene scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {.instances = &table}));

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
    ECS::Scene scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {.instances = &table}));
    CHECK(scene.AliveCount() == 2);

    // "Two entities loaded" is true whether the claim was dropped or applied to
    // some other member, so it cannot be the whole assertion. Dropped means the
    // fov nobody declared appears nowhere: the body keeps the blueprint's own
    // value, and the wheel — which declares no Camera at all — does not acquire
    // one.
    const Camera *body = scene.Get<Camera>(MemberOf(scene, table, ECS::InstanceId{1}, "body"));
    REQUIRE(body != nullptr);
    CHECK(body->fovDegrees == doctest::Approx(60.f));
    CHECK(scene.Get<Camera>(MemberOf(scene, table, ECS::InstanceId{1}, "wheel_fl")) == nullptr);

    for (auto [entity, camera] : scene.Query<Camera>())
    {
        (void)entity;
        CHECK_FALSE(camera.fovDegrees == doctest::Approx(1.f));
    }
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
    const BlueprintResult loaded = Runtime::GetBlueprintDefinition("lot.abp");
    REQUIRE(loaded.has_value());
    const std::shared_ptr<const BlueprintDefinition> &definition = *loaded;
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

    ECS::Scene scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {.instances = &table}));

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

    ECS::Scene scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {.instances = &table}));

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

    const BlueprintResult loaded = Runtime::GetBlueprintDefinition("slashy.abp");
    REQUIRE(loaded.has_value());
    const std::shared_ptr<const BlueprintDefinition> &definition = *loaded;
    const std::optional<uint32_t> wheel = definition->IndexOf("wheel_fl");
    REQUIRE(wheel.has_value());
    // Resolved at flatten: no slash survives into the definition.
    CHECK(definition->members[*wheel].components.at("Parent").at("parent").get<std::string>() == "body");

    ECS::Scene scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {.instances = &table}));

    const Runtime::Parent *parent =
        scene.Get<Runtime::Parent>(MemberOf(scene, table, ECS::InstanceId{1}, "wheel_fl"));
    REQUIRE(parent != nullptr);
    CHECK(parent->parent == MemberOf(scene, table, ECS::InstanceId{1}, "body"));
}

// ---------------------------------------------------------------------------
// `parented` and placement. Whether a member is parented decides whether the
// instance's placement composes onto its Transform: a parentless member ends up
// in world space, a parented one is already relative to a member that absorbed
// the placement. An override may add or remove Parent, so the answer cannot be
// decided before the overrides are in.
// ---------------------------------------------------------------------------

namespace
{
nlohmann::json Place(float x)
{
    return {{"position", {x, 0.f, 0.f}}, {"rotation", {1.f, 0.f, 0.f, 0.f}}, {"scale", {1.f, 1.f, 1.f}}};
}

/// hub and body both parentless at rest; wheel_fl hangs off body.
nlohmann::json RigFile()
{
    return {{"version", 2},
        {"entities", nlohmann::json::array({{{"name", "hub"}, {"components", {{"Transform", Place(0.f)}}}},
                                               {{"name", "body"}, {"components", {{"Transform", Place(1.f)}}}},
                                               {{"name", "wheel_fl"},
                                                   {"components",
                                                    {{"Transform", Place(2.f)},
                                                        {"Parent", {{"parent", "body"}}}}}}})}};
}
} // namespace

TEST_CASE("Placement: an override that adds Parent stops the placement composing onto it")
{
    const std::filesystem::path root = FreshRoot("parent_added");
    Write(root, "rig.abp", RigFile());
    Write(root, "main.alvl",
          {{"version", 2},
              {"entities", nlohmann::json::array()},
              {"instances", nlohmann::json::array({{{"name", "r"},
                                                      {"source", "rig.abp"},
                                                      {"transform", Place(100.f)},
                                                      // body becomes a child of hub.
                                                      {"overrides", {{"body", {{"Parent", {{"parent", "hub"}}}}}}}}})}});

    ECS::Scene scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {.instances = &table}));

    // Now relative to hub, which already absorbed the placement — so body keeps its
    // authored 1.0 and does not also take the instance's 100.
    const ECS::Transform *body =
        scene.Get<ECS::Transform>(MemberOf(scene, table, ECS::InstanceId{1}, "body"));
    REQUIRE(body != nullptr);
    CHECK(body->position.x == doctest::Approx(1.f));
}

TEST_CASE("Placement: an override that removes Parent lets the placement compose onto it")
{
    const std::filesystem::path root = FreshRoot("parent_removed");
    Write(root, "rig.abp", RigFile());
    Write(root, "main.alvl",
          {{"version", 2},
              {"entities", nlohmann::json::array()},
              {"instances", nlohmann::json::array({{{"name", "r"},
                                                      {"source", "rig.abp"},
                                                      {"transform", Place(100.f)},
                                                      // wheel_fl is cut loose from body.
                                                      {"overrides", {{"wheel_fl", {{"Parent", nullptr}}}}}}})}});

    ECS::Scene scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {.instances = &table}));

    const ECS::Entity wheel = MemberOf(scene, table, ECS::InstanceId{1}, "wheel_fl");
    REQUIRE(scene.Get<Runtime::Parent>(wheel) == nullptr);

    // Parentless now, so it is in world space and must carry the instance's 100.
    const ECS::Transform *transform = scene.Get<ECS::Transform>(wheel);
    REQUIRE(transform != nullptr);
    CHECK(transform->position.x == doctest::Approx(102.f));
}

TEST_CASE("Placement: adding Parent to a nested member also undoes the placement already baked in")
{
    const std::filesystem::path root = FreshRoot("parent_nested");
    Write(root, "rig.abp", RigFile());
    // rig is nested, so its parentless members had the *lot's* placement composed
    // into their Transform at flatten — before any override could say otherwise.
    Write(root, "lot.abp",
          {{"version", 2},
              {"entities", nlohmann::json::array()},
              {"instances",
               nlohmann::json::array({{{"name", "r1"}, {"source", "rig.abp"}, {"transform", Place(10.f)}}})}});
    Write(root, "main.alvl",
          {{"version", 2},
              {"entities", nlohmann::json::array()},
              {"instances", nlohmann::json::array({{{"name", "L"},
                                                      {"source", "lot.abp"},
                                                      {"transform", Place(100.f)},
                                                      {"overrides",
                                                       {{"r1/body", {{"Parent", {{"parent", "r1/hub"}}}}}}}}})}});

    ECS::Scene scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {.instances = &table}));

    // body is now relative to hub, which sits at the rig's origin. Its authored
    // local was 1, and neither the lot's 10 nor the level's 100 belongs to it any
    // more — the 10 was composed in at flatten and has to come back out.
    const ECS::Transform *body =
        scene.Get<ECS::Transform>(MemberOf(scene, table, ECS::InstanceId{1}, "r1/body"));
    REQUIRE(body != nullptr);
    CHECK(body->position.x == doctest::Approx(1.f));
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

    ECS::Scene scene;
    InstanceTable table;
    Runtime::LevelHeader header;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {.header = &header, .instances = &table}));

    const nlohmann::json saved = SceneSerializer::Save(scene, header, &table);
    REQUIRE(saved.at("instances").size() == 1);
    const nlohmann::json &entry = saved.at("instances")[0];
    CHECK(entry.at("overrides").at("body").at("Camera").at("fovDegrees").get<float>() == doctest::Approx(44.f));
    CHECK(entry.at("removed") == nlohmann::json::array({"wheel_fl"}));

    // And the whole file round-trips: a save that lost the claims would leave a
    // level that reads as a plain car the next time it opens.
    ECS::Scene reloaded;
    InstanceTable reloadedTable;
    REQUIRE(SceneSerializer::Load(reloaded, saved, {.instances = &reloadedTable}).has_value());
    CHECK(reloaded.AliveCount() == 1);
    const Camera *camera = reloaded.Get<Camera>(MemberOf(reloaded, reloadedTable, ECS::InstanceId{1}, "body"));
    REQUIRE(camera != nullptr);
    CHECK(camera->fovDegrees == doctest::Approx(44.f));
}

TEST_CASE("Overrides: an override may not rename the member it applies to")
{
    // The other half of round-7 S23. A file declaring a Name renames the member
    // outright; an override claiming one arrives through the JSON path instead,
    // where Scene::Add refuses an occupied slot — so today it is not a rename but
    // a silent no-op, which is its own defect. The author wrote something that
    // will never happen and is told nothing. Both halves are the same rule: the
    // member path is the address the override was itself routed by, so the claim
    // is refused, and said out loud.
    //
    // The name assertion holds before the fix and is a pin, not the proof: it is
    // what catches a future add-or-replace path quietly turning the no-op into
    // the rename. The warning is what this case actually moves.
    const std::filesystem::path root = FreshRoot("rename");
    Write(root, "car.abp", CarFile());
    Write(root, "main.alvl",
          {{"version", 2},
              {"entities", nlohmann::json::array()},
              {"instances", nlohmann::json::array({{{"name", "car_3"},
                                                      {"source", "car.abp"},
                                                      {"overrides", {{"body", {{"Name", {{"value", "chassis"}}}}}}}}})}});

    const Tests::LogCapture log;

    ECS::Scene scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {.instances = &table}));

    const Runtime::Name *name =
        scene.Get<Runtime::Name>(MemberOf(scene, table, ECS::InstanceId{1}, "body"));
    REQUIRE(name != nullptr);
    CHECK(name->value.View() == "body");

    CHECK(log.Mentions("Name component"));
}
