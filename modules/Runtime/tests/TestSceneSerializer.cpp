/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

using namespace Assisi;
using Assisi::Runtime::CameraComponent;
using Assisi::Runtime::MeshRendererComponent;
using Assisi::Runtime::ParentComponent;
using Assisi::Runtime::SceneSerializer;
using Assisi::Runtime::TransformComponent;

TEST_CASE("SceneSerializer: transform values survive a round-trip")
{
    ECS::Scene scene;
    const ECS::Entity e = scene.Create();
    REQUIRE(scene.Add(e, TransformComponent{.position = {1.f, 2.f, 3.f},
                                            .rotation = glm::quat{1.f, 0.f, 0.f, 0.f},
                                            .scale    = {4.f, 5.f, 6.f}}) != nullptr);

    ECS::Scene loaded;
    SceneSerializer::Load(loaded, SceneSerializer::Save(scene));

    const auto *t = loaded.Get<TransformComponent>(ECS::Entity{.index = 0, .generation = 0});
    REQUIRE(t != nullptr);
    CHECK(t->position.x == doctest::Approx(1.f));
    CHECK(t->position.y == doctest::Approx(2.f));
    CHECK(t->position.z == doctest::Approx(3.f));
    CHECK(t->scale.x == doctest::Approx(4.f));
    CHECK(t->scale.z == doctest::Approx(6.f));
}

// Regression for the forward-reference bug: a child whose serial index precedes
// its parent's must still resolve the parent on load. Creating the child before
// the parent makes the child sort first (lower key), so its ParentComponent
// references an entity that a single-pass loader has not created yet.
TEST_CASE("SceneSerializer: forward parent reference survives a round-trip")
{
    ECS::Scene scene;
    const ECS::Entity child  = scene.Create(); // {0,0} — sorts first
    const ECS::Entity parent = scene.Create(); // {1,0}

    REQUIRE(scene.Add(parent, TransformComponent{}) != nullptr);
    REQUIRE(scene.Add(child, TransformComponent{}) != nullptr);
    REQUIRE(scene.Add(child, ParentComponent{.parent = parent}) != nullptr);

    ECS::Scene loaded;
    SceneSerializer::Load(loaded, SceneSerializer::Save(scene));

    // Load allocates fresh sequential entities: serial 0 -> {0,0} (the child),
    // serial 1 -> {1,0} (the parent).
    const ECS::Entity loadedChild{.index = 0, .generation = 0};
    const ECS::Entity loadedParent{.index = 1, .generation = 0};

    const auto *pc = loaded.Get<ParentComponent>(loadedChild);
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
             {{{"components", {{"ParentComponent", {{"parent", 1}}}}}}, // [0] child -> parent 1
              {{"components", {{"TransformComponent",
                               {{"position", {0.f, 0.f, 0.f}},
                                {"rotation", {1.f, 0.f, 0.f, 0.f}},
                                {"scale", {1.f, 1.f, 1.f}}}}}}}})}}; // [1] parent

    ECS::Scene loaded;
    SceneSerializer::Load(loaded, fixture);

    const auto *pc = loaded.Get<ParentComponent>(ECS::Entity{.index = 0, .generation = 0});
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

TEST_CASE("SceneSerializer: an all-transient component keeps its presence but no data")
{
    ECS::Scene scene;
    const ECS::Entity e = scene.Create();
    REQUIRE(scene.Add(e, MeshRendererComponent{}) != nullptr);

    ECS::Scene loaded;
    SceneSerializer::Load(loaded, SceneSerializer::Save(scene));

    // MeshRendererComponent is entirely transient — it serializes as {} but its
    // presence on the entity must survive.
    CHECK(loaded.Get<MeshRendererComponent>(ECS::Entity{.index = 0, .generation = 0}) != nullptr);
}

TEST_CASE("SceneSerializer: multiple components on one entity all round-trip")
{
    ECS::Scene scene;
    const ECS::Entity e = scene.Create();
    REQUIRE(scene.Add(e, TransformComponent{.position = {1.f, 2.f, 3.f}}) != nullptr);
    REQUIRE(scene.Add(e, CameraComponent{.fovDegrees = 42.f, .isActive = true}) != nullptr);

    ECS::Scene loaded;
    SceneSerializer::Load(loaded, SceneSerializer::Save(scene));

    const ECS::Entity le{.index = 0, .generation = 0};
    const auto *t = loaded.Get<TransformComponent>(le);
    const auto *c = loaded.Get<CameraComponent>(le);
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
    REQUIRE(scene.Add(e, TransformComponent{}) != nullptr);

    const nlohmann::json future = {{"version", 999}, {"entities", nlohmann::json::array()}};
    SceneSerializer::Load(scene, future); // rejected before the scene is cleared

    CHECK(scene.AliveCount() == 1);
    CHECK(scene.Get<TransformComponent>(e) != nullptr);
}

TEST_CASE("SceneSerializer: save/load through a file round-trips")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "assisi_serializer_test";
    fs::create_directories(root);
    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());

    ECS::Scene scene;
    const ECS::Entity e = scene.Create();
    REQUIRE(scene.Add(e, TransformComponent{.position = {8.f, 0.f, 0.f}}) != nullptr);

    REQUIRE(SceneSerializer::SaveToFile(scene, root / "rt.alvl"));

    ECS::Scene loaded;
    REQUIRE(SceneSerializer::LoadFromFile(loaded, "rt.alvl"));
    const auto *t = loaded.Get<TransformComponent>(ECS::Entity{.index = 0, .generation = 0});
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
                                               {"TransformComponent",
                                                {{"position", {7.f, 0.f, 0.f}},
                                                 {"rotation", {1.f, 0.f, 0.f, 0.f}},
                                                 {"scale", {1.f, 1.f, 1.f}}}}}}}})}};

    ECS::Scene loaded;
    SceneSerializer::Load(loaded, fixture);

    const auto *t = loaded.Get<TransformComponent>(ECS::Entity{.index = 0, .generation = 0});
    REQUIRE(t != nullptr);
    CHECK(t->position.x == doctest::Approx(7.f));
}
