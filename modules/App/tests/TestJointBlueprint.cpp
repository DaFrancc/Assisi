/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestJointBlueprint.cpp
/// @brief The ragdoll the book's Joints page prints spawns and holds together.
///
/// The blueprint is read out of the page itself, so the example a reader copies
/// is the one this checks: a listing that stopped loading, or a joint naming a
/// member by the wrong name, fails here rather than in a reader's game.

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>

#include <Assisi/App/BlueprintVerbs.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Blueprint.hpp>

using namespace Assisi;

namespace
{

/// The fixed step the ragdoll is simulated at.
constexpr float kStep = 1.f / 60.f;

/// Two seconds of falling.
constexpr int32_t kFallSteps = 120;

/// How far apart two joined limbs may drift from where they started (m). A
/// joint holds them to within millimetres; a limb pulled off its joint drifts
/// metres in two seconds of fall.
constexpr float kHoldTolerance = 0.05f;

/// The first ```json block of the book's Joints page.
std::string RagdollFromBook()
{
    const std::filesystem::path page =
        std::filesystem::path{ASSISI_SOURCE_ASSET_ROOT}.parent_path() / "book" / "src" / "physics-joints.md";
    std::ifstream in(page);
    REQUIRE(in.good());
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const std::string open = "```json\n";
    const std::size_t start = text.find(open);
    REQUIRE(start != std::string::npos);
    const std::size_t end = text.find("```", start + open.size());
    REQUIRE(end != std::string::npos);
    return text.substr(start + open.size(), end - start - open.size());
}

glm::vec3 PositionOf(const App::World &world, ECS::Entity entity)
{
    return world.scene.Get<ECS::Transform>(entity)->position;
}

} // namespace

TEST_CASE("The book's ragdoll blueprint spawns, falls, and keeps its limbs on their joints")
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "assisi_joint_ragdoll";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());
    Runtime::ClearBlueprintCache();
    {
        std::ofstream out(root / "ragdoll.abp", std::ios::binary);
        out << RagdollFromBook();
        REQUIRE(out.good());
    }

    App::World world{Physics::NoCollisionAssets()};
    const std::optional<ECS::InstanceId> id = App::SpawnBlueprint(world, "ragdoll.abp", ECS::Transform{});
    REQUIRE(id.has_value());

    const ECS::Entity pelvis = App::FindMember(world, *id, "Pelvis");
    const ECS::Entity torso = App::FindMember(world, *id, "Torso");
    const ECS::Entity head = App::FindMember(world, *id, "Head");
    const ECS::Entity upperArm = App::FindMember(world, *id, "UpperArmL");
    const ECS::Entity forearm = App::FindMember(world, *id, "ForearmL");
    REQUIRE(forearm != ECS::NullEntity);

    // Each joint names the member the listing says, by name.
    CHECK(world.scene.Get<Physics::SwingTwistJoint>(torso)->other == pelvis);
    CHECK(world.scene.Get<Physics::SwingTwistJoint>(head)->other == torso);
    CHECK(world.scene.Get<Physics::SwingTwistJoint>(upperArm)->other == torso);
    CHECK(world.scene.Get<Physics::HingeJoint>(forearm)->other == upperArm);

    const float headToTorso = glm::length(PositionOf(world, head) - PositionOf(world, torso));
    const float forearmToUpperArm = glm::length(PositionOf(world, forearm) - PositionOf(world, upperArm));
    const float startHeight = PositionOf(world, pelvis).y;

    for (int32_t i = 0; i < kFallSteps; ++i)
    {
        world.physics.Update(kStep);
    }

    CHECK(PositionOf(world, pelvis).y < startHeight - 1.f);
    CHECK(std::abs(glm::length(PositionOf(world, head) - PositionOf(world, torso)) - headToTorso) < kHoldTolerance);
    CHECK(std::abs(glm::length(PositionOf(world, forearm) - PositionOf(world, upperArm)) - forearmToUpperArm) <
          kHoldTolerance);
}
