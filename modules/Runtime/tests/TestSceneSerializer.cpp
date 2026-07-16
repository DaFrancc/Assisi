/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

// doctest forward-declares std::ostream; stringifying a std::string_view in a
// CHECK (an AssetPath View() comparison) needs the complete type.
#include <ostream>

#include <filesystem>
#include <fstream>
#include <string_view>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

using namespace Assisi;
using Assisi::Runtime::Camera;
using Assisi::Runtime::MeshRenderer;
using Assisi::Runtime::Parent;
using Assisi::Runtime::SceneSerializer;
using Assisi::Runtime::Transform;

TEST_CASE("SceneSerializer: transform values survive a round-trip")
{
    ECS::Scene scene;
    const ECS::Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{.position = {1.f, 2.f, 3.f},
                                            .rotation = glm::quat{1.f, 0.f, 0.f, 0.f},
                                            .scale    = {4.f, 5.f, 6.f}}) != nullptr);

    ECS::Scene loaded;
    SceneSerializer::Load(loaded, SceneSerializer::Save(scene));

    const auto *t = loaded.Get<Transform>(ECS::Entity{.index = 0, .generation = 0});
    REQUIRE(t != nullptr);
    CHECK(t->position.x == doctest::Approx(1.f));
    CHECK(t->position.y == doctest::Approx(2.f));
    CHECK(t->position.z == doctest::Approx(3.f));
    CHECK(t->scale.x == doctest::Approx(4.f));
    CHECK(t->scale.z == doctest::Approx(6.f));
}

// Regression for the forward-reference bug: a child whose serial index precedes
// its parent's must still resolve the parent on load. Creating the child before
// the parent makes the child sort first (lower key), so its Parent
// references an entity that a single-pass loader has not created yet.
TEST_CASE("SceneSerializer: forward parent reference survives a round-trip")
{
    ECS::Scene scene;
    const ECS::Entity child  = scene.Create(); // {0,0} — sorts first
    const ECS::Entity parent = scene.Create(); // {1,0}

    REQUIRE(scene.Add(parent, Transform{}) != nullptr);
    REQUIRE(scene.Add(child, Transform{}) != nullptr);
    REQUIRE(scene.Add(child, Parent{.parent = parent}) != nullptr);

    ECS::Scene loaded;
    SceneSerializer::Load(loaded, SceneSerializer::Save(scene));

    // Load allocates fresh sequential entities: serial 0 -> {0,0} (the child),
    // serial 1 -> {1,0} (the parent).
    const ECS::Entity loadedChild{.index = 0, .generation = 0};
    const ECS::Entity loadedParent{.index = 1, .generation = 0};

    const auto *pc = loaded.Get<Parent>(loadedChild);
    REQUIRE(pc != nullptr);
    CHECK(pc->parent != ECS::NullEntity); // the bug flattened this to null
    CHECK(pc->parent == loadedParent);
}

// Adversarial hand-authored fixture: entity[0] (the child) references parent
// serial index 1, which appears *after* it in the array. Exercises the loader
// directly, independent of save ordering.
TEST_CASE("SceneSerializer: child-before-parent fixture loads the hierarchy")
{
    const nlohmann::json fixture = {
        {"version", 1},
        {"entities",
         nlohmann::json::array(
             {{{"components", {{"Parent", {{"parent", 1}}}}}}, // [0] child -> parent 1
              {{"components", {{"Transform",
                               {{"position", {0.f, 0.f, 0.f}},
                                {"rotation", {1.f, 0.f, 0.f, 0.f}},
                                {"scale", {1.f, 1.f, 1.f}}}}}}}})}}; // [1] parent

    ECS::Scene loaded;
    SceneSerializer::Load(loaded, fixture);

    const auto *pc = loaded.Get<Parent>(ECS::Entity{.index = 0, .generation = 0});
    REQUIRE(pc != nullptr);
    CHECK(pc->parent == ECS::Entity{.index = 1, .generation = 0});
}

TEST_CASE("SceneSerializer: an empty scene round-trips to an empty scene")
{
    ECS::Scene scene;
    ECS::Scene loaded;
    loaded.Create(); // pre-existing entity that Load must clear away

    SceneSerializer::Load(loaded, SceneSerializer::Save(scene));
    CHECK(loaded.AliveCount() == 0);
}

TEST_CASE("SceneSerializer: MeshRenderer asset ids round-trip; GPU handles don't")
{
    // Reserved built-in id for prim://cube, plus an arbitrary material id. No hint
    // resolver is installed, so the GUIDs serialize without a path hint and the
    // ids round-trip on their own.
    const Core::AssetId cubeId     = Core::BuiltinAssetId::Cube;
    const Core::AssetId materialId = *Core::AssetId::Parse("8c08e9c0-e9fb-4f84-a9ba-7a90223526fd");

    ECS::Scene        scene;
    const ECS::Entity e = scene.Create();
    REQUIRE(scene.Add(e, MeshRenderer{
                             .mesh              = cubeId,
                             .materialOverrides = {materialId},
                         }) != nullptr);

    ECS::Scene loaded;
    SceneSerializer::Load(loaded, SceneSerializer::Save(scene));

    const MeshRenderer *mrc =
        loaded.Get<MeshRenderer>(ECS::Entity{.index = 0, .generation = 0});
    REQUIRE(mrc != nullptr); // presence survives
    CHECK(mrc->mesh == cubeId);
    REQUIRE(mrc->materialOverrides.size() == 1);
    CHECK(mrc->materialOverrides[0] == materialId);
    // The mesh/material handles are transient: never serialized, so they come
    // back empty and get re-resolved from the ids at load time.
    CHECK(mrc->meshBuffer == nullptr);
    CHECK(mrc->materials.empty());
}

TEST_CASE("SceneSerializer: multiple components on one entity all round-trip")
{
    ECS::Scene scene;
    const ECS::Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{.position = {1.f, 2.f, 3.f}}) != nullptr);
    REQUIRE(scene.Add(e, Camera{.fovDegrees = 42.f, .isActive = true}) != nullptr);

    ECS::Scene loaded;
    SceneSerializer::Load(loaded, SceneSerializer::Save(scene));

    const ECS::Entity le{.index = 0, .generation = 0};
    const auto *t = loaded.Get<Transform>(le);
    const auto *c = loaded.Get<Camera>(le);
    REQUIRE(t != nullptr);
    REQUIRE(c != nullptr);
    CHECK(t->position.x == doctest::Approx(1.f));
    CHECK(c->fovDegrees == doctest::Approx(42.f));
    CHECK(c->isActive == true);
}

TEST_CASE("SceneSerializer: an unsupported version leaves the scene untouched")
{
    ECS::Scene scene;
    const ECS::Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{}) != nullptr);

    const nlohmann::json future = {{"version", 999}, {"entities", nlohmann::json::array()}};
    SceneSerializer::Load(scene, future); // rejected before the scene is cleared

    CHECK(scene.AliveCount() == 1);
    CHECK(scene.Get<Transform>(e) != nullptr);
}

TEST_CASE("SceneSerializer: save/load through a file round-trips")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "assisi_serializer_test";
    fs::create_directories(root);
    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());

    ECS::Scene scene;
    const ECS::Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{.position = {8.f, 0.f, 0.f}}) != nullptr);

    REQUIRE(SceneSerializer::SaveToFile(scene, root / "rt.alvl"));

    ECS::Scene loaded;
    REQUIRE(SceneSerializer::LoadFromFile(loaded, "rt.alvl"));
    const auto *t = loaded.Get<Transform>(ECS::Entity{.index = 0, .generation = 0});
    REQUIRE(t != nullptr);
    CHECK(t->position.x == doctest::Approx(8.f));
}

TEST_CASE("SceneSerializer: malformed and missing files fail cleanly, not fatally")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "assisi_serializer_test";
    fs::create_directories(root);
    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());

    {
        std::ofstream bad(root / "bad.alvl");
        bad << "{ this is not valid json";
    }

    ECS::Scene scene;
    CHECK_FALSE(SceneSerializer::LoadFromFile(scene, "bad.alvl"));      // parse error caught
    CHECK_FALSE(SceneSerializer::LoadFromFile(scene, "nope.alvl"));     // missing file
}

TEST_CASE("SceneSerializer: unknown component names are skipped, not fatal")
{
    const nlohmann::json fixture = {
        {"version", 1},
        {"entities", nlohmann::json::array({{{"components",
                                              {{"NopeComponent", {{"x", 1}}},
                                               {"Transform",
                                                {{"position", {7.f, 0.f, 0.f}},
                                                 {"rotation", {1.f, 0.f, 0.f, 0.f}},
                                                 {"scale", {1.f, 1.f, 1.f}}}}}}}})}};

    ECS::Scene loaded;
    SceneSerializer::Load(loaded, fixture);

    const auto *t = loaded.Get<Transform>(ECS::Entity{.index = 0, .generation = 0});
    REQUIRE(t != nullptr);
    CHECK(t->position.x == doctest::Approx(7.f));
}

// ---------------------------------------------------------------------------
// ScopedRawEntityContext — the identity mapping the editor undo/redo system uses
// to serialize/restore a single component's EntityRef fields outside Save/Load.
// ---------------------------------------------------------------------------

TEST_CASE("SceneSerializer: raw-entity context round-trips a Parent handle outside Save/Load")
{
    ECS::Scene        scene;
    const ECS::Entity a = scene.Create(); // the parent
    const ECS::Entity b = scene.Create(); // the child, references a
    REQUIRE(scene.Add(a, Transform{}) != nullptr);
    REQUIRE(scene.Add(b, Parent{.parent = a}) != nullptr);

    const auto *meta = Core::Reflect::ComponentRegistry::Instance().Find("Parent");
    REQUIRE(meta != nullptr);

    // Capture: serialize b's Parent to JSON under the raw context. The handle is
    // encoded as a's raw slot index — not collapsed, not remapped.
    nlohmann::json captured;
    {
        SceneSerializer::ScopedRawEntityContext raw(scene);
        captured = meta->serialize(scene.Get<Parent>(b));
    }
    REQUIRE(captured.at("parent").is_number());
    CHECK(captured.at("parent").get<uint32_t>() == a.index);

    // Restore: remove then re-add from the captured JSON under the raw context.
    // (addToScene bottoms out in Scene::Add, which no-ops if the component still
    // exists, so the remove-first is mandatory — mirrors the apply engine.)
    scene.RemoveById(b, meta->id);
    REQUIRE_FALSE(scene.Has<Parent>(b));
    {
        SceneSerializer::ScopedRawEntityContext raw(scene);
        meta->addToScene(&scene, b.index, b.generation, captured);
    }
    const Parent *restored = scene.Get<Parent>(b);
    REQUIRE(restored != nullptr);
    CHECK(restored->parent == a); // the whole point: NOT silently flattened to NullEntity
}

TEST_CASE("SceneSerializer: WITHOUT a context an EntityRef collapses (the bug the scope fixes)")
{
    ECS::Scene        scene;
    const ECS::Entity a = scene.Create();
    const ECS::Entity b = scene.Create();
    REQUIRE(scene.Add(b, Parent{.parent = a}) != nullptr);

    const auto *meta = Core::Reflect::ComponentRegistry::Instance().Find("Parent");
    REQUIRE(meta != nullptr);

    // No context engaged: EntityToIndex returns nullopt, so a live handle encodes
    // as the ~0u sentinel and deserializes back to NullEntity — the silent
    // hierarchy flattening that ScopedRawEntityContext exists to prevent.
    const nlohmann::json flat = meta->serialize(scene.Get<Parent>(b));
    REQUIRE(flat.at("parent").is_number());
    CHECK(flat.at("parent").get<uint32_t>() == ~0u);

    scene.RemoveById(b, meta->id);
    meta->addToScene(&scene, b.index, b.generation, flat);
    const Parent *restored = scene.Get<Parent>(b);
    REQUIRE(restored != nullptr);
    CHECK(restored->parent == ECS::NullEntity);
}
