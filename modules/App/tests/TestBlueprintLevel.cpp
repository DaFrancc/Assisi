/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBlueprintLevel.cpp
/// @brief A level that places a blueprint, loaded the way a game loads one.
///
/// The Runtime suite tests the flattening and the tag; this is the composition
/// App owns — deserialize, propagate, build bodies — over a scene whose members
/// are parented and placed. It exists because that combination is what the
/// physics/`Parent` fix (step zero) was for: a member is created in world space
/// from a placement it only reaches through its parent's matrix.

#include <doctest/doctest.h>

#include <ostream>

#include <cstdint>
#include <filesystem>
#include <fstream>

#include <Assisi/App/LevelRuntime.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
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
