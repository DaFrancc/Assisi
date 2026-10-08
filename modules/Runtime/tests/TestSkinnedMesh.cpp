/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestSkinnedMesh.cpp
/// @brief A SkinnedMesh starts at its skeleton's rest pose, keeps what code
/// writes into it until the mesh changes, and is saved as presence alone.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <string>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>
#include <Assisi/Runtime/SkinnedMeshPose.hpp>

using namespace Assisi;
using Assisi::Geometry::JointTransform;
using Assisi::Geometry::Skeleton;
using Assisi::Runtime::BindPose;
using Assisi::Runtime::kUnboundMesh;
using Assisi::Runtime::SkinnedMesh;

namespace
{

constexpr uint32_t kFirstMesh = 1;
constexpr uint32_t kSecondMesh = 2;

/// A chain of @p jointCount joints, each one unit above its parent.
Skeleton Chain(uint32_t jointCount)
{
    Skeleton skeleton;
    for (uint32_t joint = 0; joint < jointCount; ++joint)
    {
        skeleton.Names.push_back("joint" + std::to_string(joint));
        skeleton.Parents.push_back(static_cast<int32_t>(joint) - 1);
        JointTransform rest;
        rest.Translation = {0.f, joint == 0 ? 0.f : 1.f, 0.f};
        skeleton.RestLocal.push_back(rest);
        skeleton.InverseBind.push_back(glm::translate(glm::mat4(1.f), glm::vec3(0.f, -static_cast<float>(joint), 0.f)));
    }
    return skeleton;
}

} // namespace

TEST_CASE("SkinnedMesh: binding starts at the rest pose, sized for the skeleton")
{
    const Skeleton skeleton = Chain(3);
    SkinnedMesh skinned;

    CHECK(BindPose(skinned, skeleton, kFirstMesh) == &skeleton);
    CHECK(skinned.boundMeshId == kFirstMesh);
    REQUIRE(skinned.pose.size() == 3);
    CHECK(skinned.pose[2].Translation.y == doctest::Approx(1.f));
    CHECK(skinned.jointModel.size() == 3);
    CHECK(skinned.palette.size() == 3);
}

TEST_CASE("SkinnedMesh: a pose written from code survives binding to the same mesh again")
{
    // Binding runs every frame before evaluation; resetting here would undo every
    // joint code or a clip set since the last frame.
    const Skeleton skeleton = Chain(2);
    SkinnedMesh skinned;
    (void)BindPose(skinned, skeleton, kFirstMesh);

    skinned.pose[1].Rotation = glm::angleAxis(1.f, glm::vec3(0.f, 0.f, 1.f));
    (void)BindPose(skinned, skeleton, kFirstMesh);

    CHECK(skinned.pose[1].Rotation.w == doctest::Approx(std::cos(0.5f)));
}

TEST_CASE("SkinnedMesh: a different mesh resets and resizes the pose")
{
    const Skeleton small = Chain(2);
    const Skeleton large = Chain(4);
    SkinnedMesh skinned;
    (void)BindPose(skinned, small, kFirstMesh);
    skinned.pose[1].Rotation = glm::angleAxis(1.f, glm::vec3(0.f, 0.f, 1.f));

    (void)BindPose(skinned, large, kSecondMesh);

    CHECK(skinned.boundMeshId == kSecondMesh);
    REQUIRE(skinned.pose.size() == 4);
    CHECK(skinned.jointModel.size() == 4);
    CHECK(skinned.palette.size() == 4);
    CHECK(skinned.pose[1].Rotation.w == doctest::Approx(1.f));
}

TEST_CASE("SkinnedMesh: a mesh with no skeleton leaves nothing bound")
{
    const Skeleton skeleton = Chain(2);
    SkinnedMesh skinned;
    (void)BindPose(skinned, skeleton, kFirstMesh);

    CHECK(BindPose(skinned, Skeleton{}, kSecondMesh) == nullptr);
    CHECK(skinned.boundMeshId == kUnboundMesh);
    CHECK(skinned.pose.empty());
    CHECK(skinned.jointModel.empty());
    CHECK(skinned.palette.empty());
}

TEST_CASE("SkinnedMesh: unbinding empties it, so the next bind starts from rest")
{
    const Skeleton skeleton = Chain(2);
    SkinnedMesh skinned;
    (void)BindPose(skinned, skeleton, kFirstMesh);
    skinned.pose[1].Rotation = glm::angleAxis(1.f, glm::vec3(0.f, 0.f, 1.f));

    Runtime::UnbindSkinnedMesh(skinned);
    CHECK(skinned.boundMeshId == kUnboundMesh);
    CHECK(skinned.pose.empty());

    (void)BindPose(skinned, skeleton, kFirstMesh);
    CHECK(skinned.pose[1].Rotation.w == doctest::Approx(1.f));
}

TEST_CASE("SkinnedMesh: saved and loaded by presence, bringing its MeshRenderer")
{
    ECS::Scene scene;
    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add(entity, SkinnedMesh{}) != nullptr);
    REQUIRE(scene.Get<Runtime::MeshRenderer>(entity) != nullptr);

    ECS::Scene loaded;
    REQUIRE(Runtime::SceneSerializer::Load(loaded, Runtime::SceneSerializer::Save(scene)).has_value());

    const ECS::Entity first{.index = 0, .generation = 0};
    const SkinnedMesh *skinned = loaded.Get<SkinnedMesh>(first);
    REQUIRE(skinned != nullptr);
    CHECK(skinned->boundMeshId == kUnboundMesh);
    CHECK(skinned->pose.empty());
    CHECK(loaded.Get<Runtime::MeshRenderer>(first) != nullptr);
}
