/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBlueprintPreparedForm.cpp
/// @brief The cached encoding a spawn decodes from, and the two properties that
/// make it safe to prefer over the JSON path.
///
/// A blueprint is parsed **once** and cached: spawning a hundred bullets must not
/// re-read and re-parse `bullet.abp` a hundred times
/// (docs/blueprint-system-concept.md §11).
///
/// What is cached is not the JSON but one BinaryCodec block per component, and it
/// has to be a decode rather than a byte copy: MeshRenderer holds a
/// `std::vector<AssetId>` whose bytes are a pointer, so copying them into a
/// hundred bullets would give a hundred components sharing one allocation and
/// ninety-nine dangling the moment the first is destroyed. The vector case below
/// is the one that would catch a memcpy shortcut.

#include <doctest/doctest.h>

#include <ostream>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

using namespace Assisi;
using Assisi::Runtime::BlueprintDefinition;
using Assisi::Runtime::BlueprintError;
using Assisi::Runtime::BlueprintResult;
using Assisi::Runtime::Camera;
using Assisi::Runtime::InstanceTable;
using Assisi::Runtime::MeshRenderer;
using Assisi::Runtime::SceneSerializer;

namespace
{

std::filesystem::path FreshRoot(const std::string &name)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / ("assisi_bpp_" + name);
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

ECS::Entity MemberOf(ECS::Scene &scene, const InstanceTable &table, ECS::InstanceId id, std::string_view name)
{
    return Runtime::FindMember(scene, table, id, name);
}

} // namespace

TEST_CASE("Prepared form: every component a member declares is encoded once")
{
    const std::filesystem::path root = FreshRoot("blocks");
    Write(root, "car.abp", {{"version", 2},
              {"entities", nlohmann::json::array({{{"name", "body"},
                                                     {"components",
                                                      {{"Camera", {{"fovDegrees", 55.f}}},
                                                          {"Transform",
                                                           {{"position", {1.f, 2.f, 3.f}},
                                                               {"rotation", {1.f, 0.f, 0.f, 0.f}},
                                                               {"scale", {1.f, 1.f, 1.f}}}}}}}})}});

    const BlueprintResult loaded = Runtime::GetBlueprintDefinition("car.abp");
    REQUIRE(loaded.has_value());
    const std::shared_ptr<const BlueprintDefinition> &definition = *loaded;
    REQUIRE(definition->members.size() == 1);

    const auto &prepared = definition->members[0].prepared;
    REQUIRE(prepared.size() == 2);
    for (const Runtime::PreparedComponent &component : prepared)
    {
        CHECK_FALSE(component.block.empty());
        CHECK(component.id != Core::Reflect::kInvalidComponentId);
    }

    // Cached by virtual path, so the second ask is the same object rather than a
    // second parse.
    const BlueprintResult again = Runtime::GetBlueprintDefinition("car.abp");
    REQUIRE(again.has_value());
    CHECK(*again == definition);
}

TEST_CASE("Prepared form: a spawn decodes to the same values a JSON load produces")
{
    const std::filesystem::path root = FreshRoot("equal");
    Write(root, "car.abp", {{"version", 2},
              {"entities", nlohmann::json::array({{{"name", "body"},
                                                     {"components",
                                                      {{"Camera",
                                                          {{"fovDegrees", 77.f}, {"isActive", true}}},
                                                          {"Transform",
                                                           {{"position", {1.f, 2.f, 3.f}},
                                                               {"rotation", {1.f, 0.f, 0.f, 0.f}},
                                                               {"scale", {4.f, 4.f, 4.f}}}}}}}})}});

    ECS::Scene scene;
    InstanceTable table;
    const auto id = SceneSerializer::ExpandInstance(scene, table, "car.abp", {});
    REQUIRE(id.has_value());

    const ECS::Entity body = MemberOf(scene, table, *id, "body");
    REQUIRE(body != ECS::NullEntity);

    const Camera *camera = scene.Get<Camera>(body);
    REQUIRE(camera != nullptr);
    CHECK(camera->fovDegrees == doctest::Approx(77.f));
    CHECK(camera->isActive == true);

    const ECS::Transform *transform = scene.Get<ECS::Transform>(body);
    REQUIRE(transform != nullptr);
    CHECK(transform->position.y == doctest::Approx(2.f));
    CHECK(transform->scale.x == doctest::Approx(4.f));
}

TEST_CASE("Prepared form: two spawns hold their own vector storage")
{
    const std::filesystem::path root = FreshRoot("vectors");
    // The case a byte copy would fail: materialOverrides is a std::vector, so its
    // bytes are a pointer.
    Write(root, "car.abp",
          {{"version", 2},
              {"entities", nlohmann::json::array({{{"name", "body"},
                                                     {"components",
                                                      {{"MeshRenderer",
                                                          {{"mesh",
                                                              {{"guid", "00000000-0000-0000-0000-000000000001"}}},
                                                              {"materialOverrides",
                                                               nlohmann::json::array(
                                                                   {{{"guid", "8c08e9c0-e9fb-4f84-a9ba-7a90223526fd"}}})}}}}}}})}});

    ECS::Scene scene;
    InstanceTable table;
    const auto first  = SceneSerializer::ExpandInstance(scene, table, "car.abp", {});
    const auto second = SceneSerializer::ExpandInstance(scene, table, "car.abp", {});
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    MeshRenderer *a = scene.GetMut<MeshRenderer>(MemberOf(scene, table, *first, "body"));
    MeshRenderer *b = scene.GetMut<MeshRenderer>(MemberOf(scene, table, *second, "body"));
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    REQUIRE(a->materialOverrides.size() == 1);
    REQUIRE(b->materialOverrides.size() == 1);
    CHECK(a->materialOverrides[0] == b->materialOverrides[0]);

    // Separate allocations, not one shared between them: growing the first must
    // leave the second alone, which is exactly what a memcpy would not do.
    CHECK(a->materialOverrides.data() != b->materialOverrides.data());
    a->materialOverrides.clear();
    CHECK(b->materialOverrides.size() == 1);
}

TEST_CASE("Prepared form: a reference decodes to this instance's member, not the other's")
{
    const std::filesystem::path root = FreshRoot("refs");
    Write(root, "car.abp", {{"version", 2},
              {"entities", nlohmann::json::array({{{"name", "body"}, {"components", {}}},
                                                     {{"name", "wheel"},
                                                         {"components",
                                                          {{"Parent", {{"parent", "body"}}}}}}})}});

    ECS::Scene scene;
    InstanceTable table;
    const auto first  = SceneSerializer::ExpandInstance(scene, table, "car.abp", {});
    const auto second = SceneSerializer::ExpandInstance(scene, table, "car.abp", {});
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    // The block stores the *member index*; the codec's reference hook turns it into
    // a handle at decode time, which is why nothing has to walk the decoded
    // component afterwards looking for references to patch.
    const Runtime::Parent *parent = scene.Get<Runtime::Parent>(MemberOf(scene, table, *second, "wheel"));
    REQUIRE(parent != nullptr);
    CHECK(parent->parent == MemberOf(scene, table, *second, "body"));
    CHECK(parent->parent != MemberOf(scene, table, *first, "body"));
}

TEST_CASE("Prepared form: an overridden component takes the JSON path and the override survives")
{
    const std::filesystem::path root = FreshRoot("override");
    Write(root, "car.abp", {{"version", 2},
              {"entities", nlohmann::json::array({{{"name", "body"},
                                                     {"components",
                                                      {{"Camera",
                                                          {{"fovDegrees", 55.f},
                                                              {"isActive", true}}}}}}})}});
    Write(root, "main.alvl",
          {{"version", 2},
              {"entities", nlohmann::json::array()},
              {"instances", nlohmann::json::array({{{"name", "car_3"},
                                                      {"source", "car.abp"},
                                                      {"overrides", {{"body", {{"Camera", {{"fovDegrees", 91.f}}}}}}}}})}});

    ECS::Scene scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {.instances = &table}));

    // A prepared block is full state, so decoding one over a component the override
    // just set would undo it. The claim wins; the field the claim did not name
    // still falls back to the blueprint's value.
    const Camera *camera = scene.Get<Camera>(MemberOf(scene, table, ECS::InstanceId{1}, "body"));
    REQUIRE(camera != nullptr);
    CHECK(camera->fovDegrees == doctest::Approx(91.f));
    CHECK(camera->isActive == true);
}

TEST_CASE("Prepared form: a blueprint naming a reference it does not declare is unusable")
{
    const std::filesystem::path root = FreshRoot("badref");
    Write(root, "car.abp", {{"version", 2},
              {"entities", nlohmann::json::array({{{"name", "wheel"},
                                                     {"components",
                                                      {{"Parent", {{"parent", "chassis"}}}}}}})}});

    // Caught when the definition is built rather than at every spawn: a blueprint
    // whose wiring names nothing is broken about itself, not about where it is used.
    const BlueprintResult loaded = Runtime::GetBlueprintDefinition("car.abp");
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error() == BlueprintError::ComponentRejected);
}

// ---------------------------------------------------------------------------
// A malformed member value is an error value, not an exception
// ---------------------------------------------------------------------------
//
// Preparing a member deserializes it, and a generated deserializer reads a float
// as `j.at("fovDegrees").get<float>()` — which throws if the file says a string.
// That is the *last* step of building a definition, so it is the step most likely
// to be left outside the error path, and the contract callers wrote themselves
// against says nullptr (Blueprint.hpp). The three below are the callers that
// documented the promise; the editor's Save is the one a user meets.

namespace
{

/// A blueprint whose one member declares a Camera with a string where the float
/// goes — malformed in a way only the prepare step can see.
nlohmann::json MistypedCar()
{
    return {{"version", 2},
        {"entities", nlohmann::json::array({{{"name", "body"},
                                               {"components",
                                                {{"Camera", {{"fovDegrees", "wide"}}}}}}})}};
}

} // namespace

TEST_CASE("Prepared form: a member value of the wrong type is unusable, not a throw")
{
    const std::filesystem::path root = FreshRoot("mistyped");
    Write(root, "car.abp", MistypedCar());

    // The nlohmann throw out of the generated deserializer is caught at the
    // boundary and handed back as a value; nothing escapes to the caller.
    BlueprintResult definition;
    CHECK_NOTHROW(definition = Runtime::GetBlueprintDefinition("car.abp"));
    REQUIRE_FALSE(definition.has_value());
    CHECK(definition.error() == BlueprintError::ComponentRejected);

    // Nothing was cached, so fixing the file is enough — the failure does not
    // outlive itself and wait for a level unload to clear.
    CHECK_NOTHROW((void)Runtime::GetBlueprintDefinition("car.abp"));
    CHECK_FALSE(Runtime::GetBlueprintDefinition("car.abp").has_value());

    Write(root, "car.abp", {{"version", 2},
              {"entities", nlohmann::json::array({{{"name", "body"},
                                                     {"components",
                                                      {{"Camera", {{"fovDegrees", 55.f}}}}}}})}});
    CHECK(Runtime::GetBlueprintDefinition("car.abp").has_value());
}

TEST_CASE("Prepared form: FindMember on an instance whose file went bad answers, not throws")
{
    const std::filesystem::path root = FreshRoot("mistyped_find");
    Write(root, "car.abp", {{"version", 2},
              {"entities", nlohmann::json::array({{{"name", "body"},
                                                     {"components",
                                                      {{"Camera", {{"fovDegrees", 55.f}}}}}}})}});
    Write(root, "main.alvl", {{"version", 2},
              {"entities", nlohmann::json::array()},
              {"instances", nlohmann::json::array({{{"name", "car_3"},
                                                      {"source", "car.abp"}}})}});

    ECS::Scene scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {.instances = &table}));
    REQUIRE(MemberOf(scene, table, ECS::InstanceId{1}, "body") != ECS::NullEntity);

    // The editor's blueprint save, in the order the editor does it: the file on
    // disk changes, the cached definition is dropped, and the *live* instance is
    // still standing with tags that need the file to be readable to mean anything.
    Write(root, "car.abp", MistypedCar());
    Runtime::InvalidateBlueprint("car.abp");

    ECS::Entity found = ECS::NullEntity;
    CHECK_NOTHROW(found = Runtime::FindMember(scene, table, ECS::InstanceId{1}, "body"));
    CHECK(found == ECS::NullEntity);
}

TEST_CASE("Prepared form: saving a level whose blueprint went bad does not throw")
{
    const std::filesystem::path root = FreshRoot("mistyped_save");
    Write(root, "car.abp", {{"version", 2},
              {"entities", nlohmann::json::array({{{"name", "body"},
                                                     {"components",
                                                      {{"Camera", {{"fovDegrees", 55.f}}}}}}})}});
    Write(root, "main.alvl", {{"version", 2},
              {"entities", nlohmann::json::array()},
              {"instances", nlohmann::json::array({{{"name", "car_3"},
                                                      {"source", "car.abp"}}})}});

    ECS::Scene scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {.instances = &table}));

    Write(root, "car.abp", MistypedCar());
    Runtime::InvalidateBlueprint("car.abp");

    // Save walks every member to name it, which is where it asks for the
    // definition. Throwing here loses the user's level: the one action they take
    // to keep their work is the one that fails.
    nlohmann::json saved;
    CHECK_NOTHROW(saved = SceneSerializer::Save(scene, Runtime::LevelHeader{}, &table));
    CHECK(saved.contains("instances"));
}
