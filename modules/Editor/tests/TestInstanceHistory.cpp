/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestInstanceHistory.cpp
/// @brief Editing a blueprint member records an override, and undo takes both
/// back.
///
/// The failure this exists to prevent has a name: a **fake override**. If the
/// component reverts and the instance's record does not, the level keeps a note
/// saying "this instance changed that field" for a field it no longer changes —
/// and from then on that member stops following its blueprint. That is exactly
/// the disease recorded overrides were introduced to cure, reintroduced through
/// undo.
///
/// So the assertion that matters is not "the override is written" but "the
/// override and the value move together, in one Ctrl-Z".

#include <doctest/doctest.h>

#include <ostream>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Editor/EditHistory.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

using namespace Assisi;
using Assisi::Editor::EditHistory;
using Assisi::Runtime::Camera;
using Assisi::Runtime::InstanceTable;
using Assisi::Runtime::SceneSerializer;

namespace
{

std::filesystem::path FreshRoot(const std::string &name)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / ("assisi_edh_" + name);
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

/// car.abp: a body with a Camera, and a wheel parented to it.
nlohmann::json CarFile()
{
    return {{"version", 2},
        {"entities",
         nlohmann::json::array({{{"name", "body"},
                                   {"components", {{"Camera", {{"fovDegrees", 60.f}, {"isActive", true}}}}}},
                                   {{"name", "wheel_fl"},
                                       {"components", {{"Parent", {{"parent", "body"}}}}}}})}};
}

ECS::Entity MemberOf(ECS::Scene &scene, const InstanceTable &table, ECS::InstanceId id, std::string_view name)
{
    return Runtime::FindMember(scene, table, id, name);
}

} // namespace

TEST_CASE("EditHistory: editing a member records an override, and undo takes both back")
{
    const std::filesystem::path root = FreshRoot("override");
    Write(root, "car.abp", CarFile());

    ECS::Scene scene;
    InstanceTable table;
    const auto id = SceneSerializer::ExpandInstance(scene, table, "car.abp", {});
    REQUIRE(id.has_value());

    const ECS::Entity body = MemberOf(scene, table, *id, "body");
    REQUIRE(body != ECS::NullEntity);

    EditHistory history(scene, {}, &table);

    const auto cameraId = Core::Reflect::ComponentIdOf<Camera>();
    history.RecordBefore(body, cameraId, "Edit Camera", body);
    scene.GetMut<Camera>(body)->fovDegrees = 90.f;
    history.CommitGesture(body, cameraId);

    // Recorded, and only the field the gesture moved: writing the whole component
    // would pin isActive at today's blueprint value, which is how "fix it once,
    // fixed everywhere" quietly stops being true for that member.
    const Runtime::BlueprintInstance *row = table.Find(*id);
    REQUIRE(row != nullptr);
    REQUIRE(row->overrides.contains("body"));
    const nlohmann::json &claim = row->overrides.at("body").at("Camera");
    CHECK(claim.at("fovDegrees").get<float>() == doctest::Approx(90.f));
    CHECK_FALSE(claim.contains("isActive"));

    REQUIRE(history.CanUndo());
    (void)history.Undo();

    // Both, in one Ctrl-Z. A revert that left the note behind would be a fake
    // override: the value says 60 and the file says this instance changed it.
    CHECK(scene.Get<Camera>(body)->fovDegrees == doctest::Approx(60.f));
    const Runtime::BlueprintInstance *reverted = table.Find(*id);
    REQUIRE(reverted != nullptr);
    const bool stillClaimed =
        reverted->overrides.contains("body") && reverted->overrides.at("body").contains("Camera");
    CHECK_FALSE(stillClaimed);

    // And redo puts both back.
    (void)history.Redo();
    CHECK(scene.Get<Camera>(body)->fovDegrees == doctest::Approx(90.f));
    CHECK(table.Find(*id)->overrides.at("body").at("Camera").at("fovDegrees").get<float>() ==
          doctest::Approx(90.f));
}

TEST_CASE("EditHistory: removing a component from a member records it as a removal")
{
    const std::filesystem::path root = FreshRoot("removal");
    Write(root, "car.abp", CarFile());

    ECS::Scene scene;
    InstanceTable table;
    const auto id = SceneSerializer::ExpandInstance(scene, table, "car.abp", {});
    REQUIRE(id.has_value());
    const ECS::Entity body = MemberOf(scene, table, *id, "body");

    EditHistory history(scene, {}, &table);

    const auto cameraId = Core::Reflect::ComponentIdOf<Camera>();
    history.RecordBefore(body, cameraId, "Remove Camera", body);
    scene.RemoveById(body, cameraId);
    history.CommitGesture(body, cameraId);

    // `null` reads unambiguously as "this instance does not have it", since a real
    // component is always an object.
    const Runtime::BlueprintInstance *row = table.Find(*id);
    REQUIRE(row != nullptr);
    REQUIRE(row->overrides.contains("body"));
    CHECK(row->overrides.at("body").at("Camera").is_null());

    (void)history.Undo();
    CHECK(scene.Get<Camera>(body) != nullptr);
    const Runtime::BlueprintInstance *reverted = table.Find(*id);
    REQUIRE(reverted != nullptr);
    const bool stillClaimed =
        reverted->overrides.contains("body") && reverted->overrides.at("body").contains("Camera");
    CHECK_FALSE(stillClaimed);
}

TEST_CASE("EditHistory: a reference override is recorded by name, not by handle")
{
    const std::filesystem::path root = FreshRoot("refs");
    Write(root, "car.abp", CarFile());

    ECS::Scene scene;
    InstanceTable table;
    const auto id = SceneSerializer::ExpandInstance(scene, table, "car.abp", {});
    REQUIRE(id.has_value());

    const ECS::Entity body  = MemberOf(scene, table, *id, "body");
    const ECS::Entity wheel = MemberOf(scene, table, *id, "wheel_fl");
    REQUIRE(body != ECS::NullEntity);
    REQUIRE(wheel != ECS::NullEntity);

    EditHistory history(scene, {}, &table);

    // Unparent the wheel, then point it back at the body — the round trip a user
    // makes with the entity picker.
    const auto parentId = Core::Reflect::ComponentIdOf<Runtime::Parent>();
    history.RecordBefore(wheel, parentId, "Edit Parent", wheel);
    scene.GetMut<Runtime::Parent>(wheel)->parent = ECS::NullEntity;
    history.CommitGesture(wheel, parentId);

    const Runtime::BlueprintInstance *row = table.Find(*id);
    REQUIRE(row != nullptr);
    CHECK(row->overrides.at("wheel_fl").at("Parent").at("parent").is_null());

    history.RecordBefore(wheel, parentId, "Edit Parent", wheel);
    scene.GetMut<Runtime::Parent>(wheel)->parent = body;
    history.CommitGesture(wheel, parentId);

    // A name, not a raw handle: a capture serializes references as packed
    // (slot, generation), which is right for replaying an undo and would name
    // nothing at all the next time the file loads.
    const nlohmann::json &ref = table.Find(*id)->overrides.at("wheel_fl").at("Parent").at("parent");
    REQUIRE(ref.is_string());
    CHECK(ref.get<std::string>() == "body");
}

TEST_CASE("EditHistory: editing a loose entity records no instance claim")
{
    const std::filesystem::path root = FreshRoot("loose");
    Write(root, "car.abp", CarFile());

    ECS::Scene scene;
    InstanceTable table;
    const auto id = SceneSerializer::ExpandInstance(scene, table, "car.abp", {});
    REQUIRE(id.has_value());

    // A hand-built entity carries no BlueprintMember tag, so editing one must not
    // touch anybody's record — however much it looks like a member in the panels.
    const ECS::Entity loose = scene.Create();
    REQUIRE(scene.Add(loose, Camera{}) != nullptr);

    EditHistory history(scene, {}, &table);
    const auto cameraId = Core::Reflect::ComponentIdOf<Camera>();
    history.RecordBefore(loose, cameraId, "Edit Camera", loose);
    scene.GetMut<Camera>(loose)->fovDegrees = 30.f;
    history.CommitGesture(loose, cameraId);

    CHECK(table.Find(*id)->overrides.empty());
    CHECK(history.CanUndo());
}
