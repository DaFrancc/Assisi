/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestShippedLevels.cpp
/// @brief No level or blueprint in assets/ names a component or system that no
/// longer exists, and every level loads, builds a body for everything physical
/// in it, and simulates without coming apart.
///
/// Read from the source tree the build compiled in, so a component renamed in
/// code but not in the files fails here rather than in a player's hands.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <Assisi/App/LevelRuntime.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Blueprint.hpp>

using namespace Assisi;

namespace
{

/// Two seconds of simulation: long enough for anything dropped to land.
constexpr int32_t kSteps = 120;
constexpr float kStep = 1.f / 60.f;

bool Finite(glm::vec3 value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

/// Components and systems that no longer exist. A loader skips an unknown
/// component rather than refusing the file, so a level still naming one loads
/// with that component quietly missing; a level naming an unknown system fails
/// to load at all.
constexpr std::string_view kRetiredComponents[] = {"RigidBodyDescriptor", "CharacterDescriptor"};
constexpr std::string_view kRetiredSystems[] = {"CharacterMove", "CharacterState"};

/// The retired names @p file still uses, anywhere in it.
std::vector<std::string> RetiredNames(const std::filesystem::path &file)
{
    std::ifstream stream(file);
    const nlohmann::json document = nlohmann::json::parse(stream);
    std::vector<std::string> found;
    const std::string text = document.dump();
    for (const std::string_view name : kRetiredComponents)
    {
        if (text.find("\"" + std::string(name) + "\"") != std::string::npos)
        {
            found.emplace_back(name);
        }
    }
    for (const nlohmann::json &system : document.value("systems", nlohmann::json::array()))
    {
        for (const std::string_view name : kRetiredSystems)
        {
            if (system.is_string() && system.get<std::string>() == name)
            {
                found.emplace_back(name);
            }
        }
    }
    return found;
}

} // namespace

TEST_CASE("No shipped level or blueprint names a retired component or system")
{
    const std::filesystem::path root{ASSISI_SOURCE_ASSET_ROOT};
    int32_t files = 0;
    for (const std::filesystem::directory_entry &entry : std::filesystem::recursive_directory_iterator(root))
    {
        const std::filesystem::path extension = entry.path().extension();
        if (extension != ".alvl" && extension != ".abp")
        {
            continue;
        }
        CAPTURE(entry.path().string());
        ++files;
        CHECK(RetiredNames(entry.path()).empty());
    }
    CHECK(files > 0);
}

TEST_CASE("Every shipped level loads, builds its bodies and simulates")
{
    const std::filesystem::path root{ASSISI_SOURCE_ASSET_ROOT};
    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());
    Runtime::ClearBlueprintCache();

    int32_t levels = 0;
    for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(root / "levels"))
    {
        if (entry.path().extension() != ".alvl")
        {
            continue;
        }
        const std::string virtualPath = "levels/" + entry.path().filename().string();
        CAPTURE(virtualPath);
        ++levels;

        App::World world{Assisi::Physics::NoCollisionAssets()};
        REQUIRE(App::LoadLevelSim(world, virtualPath));

        // A piece or a follower answers through its owner's body; HasBody is
        // false for it by design.
        for (auto [entity, collider] : world.scene.Query<Physics::Collider>())
        {
            (void)collider;
            CHECK(world.physics.HasBody(world.physics.BodyOf(entity)));
        }
        for (auto [entity, character] : world.scene.Query<Physics::Character>())
        {
            (void)character;
            CHECK(world.physics.HasBody(entity));
        }

        for (int32_t i = 0; i < kSteps; ++i)
        {
            const ECS::FixedStepScope step(world.scene);
            world.physics.Update(kStep);
        }
        for (auto [entity, transform] : world.scene.Query<ECS::Transform>())
        {
            (void)entity;
            CHECK(Finite(transform.position));
        }
    }
    CHECK(levels > 0);
}
