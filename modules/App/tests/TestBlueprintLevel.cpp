/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBlueprintLevel.cpp
/// @brief A level that places a blueprint, loaded the way a game loads one.
///
/// The Runtime suite tests the flattening and the tag; this is the composition
/// App owns — deserialize, propagate, build bodies — over a scene whose members
/// are parented and placed. That combination is what needs pinning: a member is
/// created in world space from a placement it only reaches through its parent's
/// matrix.

#include <doctest/doctest.h>

#include <ostream>

#include <cstdint>
#include <filesystem>
#include <fstream>

#include <Assisi/App/LevelRuntime.hpp>
#include <Assisi/App/SystemCatalog.hpp>
#include <Assisi/App/TestSystems.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

using namespace Assisi;

namespace
{

void Write(const std::filesystem::path &path, const nlohmann::json &doc)
{
    std::ofstream out(path, std::ios::binary);
    out << doc.dump(2);
    REQUIRE(out.good());
}

} // namespace

TEST_CASE("App: a level's blueprint instances load, place, and get physics bodies")
{
    namespace fs        = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "assisi-app-blueprint-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "levels");
    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());
    Runtime::ClearBlueprintCache();

    // A crate whose lid is parented to it, so the placement reaches the lid only
    // through the parent's world matrix.
    Write(root / "crate.abp",
          {{"version", 2},
              {"entities",
               nlohmann::json::array(
                   {{{"name", "box"},
                       {"components",
                        {{"Transform",
                            {{"position", {0.f, 0.f, 0.f}}, {"rotation", {1.f, 0.f, 0.f, 0.f}}, {"scale", {1.f, 1.f, 1.f}}}},
                            {"RigidBodyDescriptor", {{"isStatic", true}}}}}},
                       {{"name", "lid"},
                           {"components",
                            {{"Transform",
                                {{"position", {0.f, 1.f, 0.f}}, {"rotation", {1.f, 0.f, 0.f, 0.f}}, {"scale", {1.f, 1.f, 1.f}}}},
                                {"Parent", {{"parent", "box"}}},
                                {"RigidBodyDescriptor", {{"isStatic", true}}}}}}})}});

    Write(root / "levels" / "yard.alvl",
          {{"version", 2},
              {"entities", nlohmann::json::array()},
              {"instances", nlohmann::json::array({{{"name", "crate_a"},
                                                      {"source", "crate.abp"},
                                                      {"transform",
                                                       {{"position", {30.f, 0.f, 0.f}},
                                                           {"rotation", {1.f, 0.f, 0.f, 0.f}},
                                                           {"scale", {1.f, 1.f, 1.f}}}}}})}});

    App::World world;
    REQUIRE(App::LoadLevelSim(world, "levels/yard.alvl"));

    CHECK(world.scene.AliveCount() == 2);
    REQUIRE(world.instances.Size() == 1);

    // Both members got a Jolt body, and the parented one is at its *world* pose —
    // not at the local (0,1,0) a physics layer that could not see Parent would
    // have used.
    int32_t bodies = 0;
    for (auto [entity, body, tag] : world.scene.Query<Physics::RigidBody, ECS::BlueprintMember>())
    {
        ++bodies;
        const auto [position, rotation] = world.physics.GetBodyTransform(body);
        CHECK(position.x == doctest::Approx(30.f));
        CHECK(position.y == doctest::Approx(tag.memberIndex == 0 ? 0.f : 1.f));
    }
    CHECK(bodies == 2);

    fs::remove_all(root, ec);
}

TEST_CASE("App: a child of a walking character follows it")
{
    // The player-blueprint shape: a character with a camera parented to it. The
    // character is moved by the physics writeback rather than by an author, and
    // the child's world matrix has to follow — otherwise the view stays where the
    // level was composed while the character walks away from it.
    namespace fs        = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "assisi-app-character-child";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "levels");
    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());
    Runtime::ClearBlueprintCache();

    Write(root / "walker.abp",
          {{"version", 2},
              {"entities",
               nlohmann::json::array(
                   {{{"name", "body"},
                       {"components",
                        {{"Transform",
                            {{"position", {0.f, 0.f, 0.f}}, {"rotation", {1.f, 0.f, 0.f, 0.f}}, {"scale", {1.f, 1.f, 1.f}}}},
                            {"CharacterDescriptor", {{"walkSpeed", 5.f}, {"groundAcceleration", 1000.f}}}}}},
                       {{"name", "eye"},
                           {"components",
                            {{"Transform",
                                {{"position", {0.f, 1.5f, 0.f}}, {"rotation", {1.f, 0.f, 0.f, 0.f}}, {"scale", {1.f, 1.f, 1.f}}}},
                                {"Parent", {{"parent", "body"}}}}}}})}});

    // A floor to stand on, and the instance placed off the origin so "followed"
    // cannot be confused with "happens to be at the spawn point".
    Write(root / "levels" / "walk.alvl",
          {{"version", 2},
              {"entities",
               nlohmann::json::array({{{"name", "floor"},
                   {"components",
                    {{"Transform",
                        {{"position", {0.f, -0.5f, 0.f}}, {"rotation", {1.f, 0.f, 0.f, 0.f}}, {"scale", {1.f, 1.f, 1.f}}}},
                        {"RigidBodyDescriptor",
                         {{"isStatic", true}, {"halfExtents", {50.f, 0.5f, 50.f}}}}}}}})},
              {"instances", nlohmann::json::array({{{"name", "walker_a"},
                                                      {"source", "walker.abp"},
                                                      {"transform",
                                                       {{"position", {5.f, 0.f, 0.f}},
                                                           {"rotation", {1.f, 0.f, 0.f, 0.f}},
                                                           {"scale", {1.f, 1.f, 1.f}}}}}})}});

    App::World world;
    REQUIRE(App::LoadLevelSim(world, "levels/walk.alvl"));

    // The two members, told apart by which one physics drives.
    ECS::Entity body = ECS::NullEntity;
    for (auto [entity, character] : world.scene.Query<Physics::Character>())
    {
        (void)character;
        body = entity;
    }
    REQUIRE(body != ECS::NullEntity);

    ECS::Entity eye = ECS::NullEntity;
    for (auto [entity, parent] : world.scene.Query<Runtime::Parent>())
    {
        if (parent.parent == body)
            eye = entity;
    }
    REQUIRE(eye != ECS::NullEntity); // the child's parent link survived expansion

    const Physics::Character *character = world.scene.Get<Physics::Character>(body);
    REQUIRE(character != nullptr);

    constexpr float kStep = 1.f / 60.f;
    uint64_t        tick  = 0;
    for (int32_t i = 0; i < 120; ++i)
    {
        world.physics.MoveCharacter(*character, {5.f, 0.f, 0.f}, /*jump=*/ false);
        world.physics.Update(kStep);
        world.physics.CaptureState();
        world.physics.InterpolateTransforms(world.scene, 1.f, App::ParentWorldResolver(world.scene));
        tick = Runtime::PropagateTransforms(world.scene, tick);
    }

    const Runtime::Transform *bodyTransform = world.scene.Get<Runtime::Transform>(body);
    const Runtime::Transform *eyeTransform  = world.scene.Get<Runtime::Transform>(eye);
    REQUIRE(bodyTransform != nullptr);
    REQUIRE(eyeTransform != nullptr);

    // It actually walked, well past where the instance was placed.
    CHECK(bodyTransform->position.x > 8.f);

    // And the child came with it: its world matrix is the character's pose plus
    // its own local offset, not the pose the level was composed at.
    const glm::vec3 eyeWorld(eyeTransform->worldMatrix[3]);
    CHECK(eyeWorld.x == doctest::Approx(bodyTransform->position.x).epsilon(0.01));
    CHECK(eyeWorld.y == doctest::Approx(bodyTransform->position.y + 1.5f).epsilon(0.05));

    fs::remove_all(root, ec);
}

TEST_CASE("App: a level runs the systems its blueprints require, not just its own")
{
    // A blueprint declares the systems its content needs so that behaviour
    // travels with it. Only SpawnBlueprint ever installed them, so a level that
    // *loaded* with an instance already in it — the ordinary case, and what the
    // editor writes when you place one and save — held the components and ran
    // none of the code. The editor's Systems panel made it worse by reading the
    // instance table and reporting the system as required while nothing ran it.
    namespace fs        = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "assisi-app-blueprint-systems";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "levels");
    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());
    Runtime::ClearBlueprintCache();

    Write(root / "ticker.abp",
          {{"version", 2},
              {"systems", nlohmann::json::array({"Counter"})},
              {"entities",
               nlohmann::json::array(
                   {{{"name", "box"},
                       {"components",
                        {{"Transform",
                            {{"position", {0.f, 0.f, 0.f}}, {"rotation", {1.f, 0.f, 0.f, 0.f}}, {"scale", {1.f, 1.f, 1.f}}}}}}}})}});

    // The level names nothing itself, so anything installed came from the
    // instance — which is the whole point of the case.
    Write(root / "levels" / "yard.alvl",
          {{"version", 2},
              {"systems", nlohmann::json::array()},
              {"entities", nlohmann::json::array()},
              {"instances", nlohmann::json::array({{{"name", "ticker_a"},
                                                      {"source", "ticker.abp"},
                                                      {"transform",
                                                       {{"position", {0.f, 0.f, 0.f}},
                                                           {"rotation", {1.f, 0.f, 0.f, 0.f}},
                                                           {"scale", {1.f, 1.f, 1.f}}}}}})}});

    App::WorldManager worlds;
    App::World *const world = worlds.LoadLevel("levels/yard.alvl");
    REQUIRE(world != nullptr);
    REQUIRE(world->instances.Size() == 1);

    CHECK(world->systems.Has("Counter"));

    // Installed, but not *claimed*: the level asked for nothing, and writing the
    // blueprint's names into its list would have the file keep claiming them
    // after the instance was deleted.
    CHECK(world->systemNames.empty());

    fs::remove_all(root, ec);
}
