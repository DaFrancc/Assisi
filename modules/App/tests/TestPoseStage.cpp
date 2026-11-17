/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPoseStage.cpp
/// @brief The pose stage, after a world's Update systems and with no renderer:
/// every entity placed, each final pose built from the animated one, joint
/// matrices worked out from it, so what runs after reads this frame's joints.

#include "TestRig.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/App/WorldFrame.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/ECS/WorldMatrix.hpp>
#include <Assisi/Geometry/Pose.hpp>
#include <Assisi/Physics/CollisionSource.hpp>
#include <Assisi/Render/MeshBuffer.hpp>
#include <Assisi/Runtime/Components.hpp>

using namespace Assisi;

namespace
{

constexpr float kStep = 1.f / 60.f;

/// What the GPU would have given the mesh; any id but the unbound one.
constexpr uint32_t kMeshId = 1;

/// The joint each frame turns, and how far it turns it each frame (radians).
constexpr int32_t kTurnedJoint = Runtime::Testing::kBodyJoint;
constexpr float kTurnPerFrame = 0.1f;

/// How close two matrices' entries must be.
constexpr float kNear = 1e-5f;

/// How far an entity moved in Update goes (m).
constexpr float kMove = 2.f;

/// A world with no renderer, and a character skinned to a rig built in code.
struct Stage
{
    Stage()
    {
        mesh.KeepWithoutGpu(Runtime::Testing::BuildSkinnedBoxes(skeleton));
        mesh.SetId(kMeshId);
        character = world.scene.Create();
        REQUIRE(world.scene.Add(character, ECS::Transform{}) != nullptr);
        Runtime::MeshRenderer renderer;
        renderer.meshBuffer = &mesh;
        REQUIRE(world.scene.Add(character, std::move(renderer)) != nullptr);
        REQUIRE(world.scene.Add(character, Runtime::SkinnedMesh{}) != nullptr);
    }

    App::SystemContext Context()
    {
        return App::SystemContext{world, kStep, 0, nullptr, nullptr, universal, true, nullptr};
    }

    /// One frame after its fixed steps: Update, the pose stage, then Ui.
    void Frame()
    {
        const App::SystemContext ctx = Context();
        App::UpdateWorld(ctx);
        App::PoseWorld(world);
        App::PresentWorld(ctx);
        App::EndWorldFrame(world, universal, true);
        ++frame;
    }

    Runtime::SkinnedPose &Pose() { return *world.scene.Get<Runtime::SkinnedPose>(character); }

    const Geometry::Skeleton skeleton = Runtime::Testing::BuildSkeleton(Runtime::Testing::TwoLimbs());
    Render::MeshBuffer mesh;
    App::World world{Physics::NoCollisionAssets()};
    Core::EventQueue universal{Core::EventScope::Universal};
    ECS::Entity character = ECS::NullEntity;
    int32_t frame = 0;
};

/// Turns the joint further each frame, as code that writes the pose does.
void Turn(Stage &stage)
{
    std::vector<Geometry::JointTransform> &pose = stage.Pose().pose;
    if (pose.empty())
    {
        return;
    }
    pose[static_cast<std::size_t>(kTurnedJoint)].Rotation =
        glm::angleAxis(kTurnPerFrame * static_cast<float>(stage.frame + 1), glm::vec3(0.f, 1.f, 0.f));
}

/// Where the pose stage should have put each joint this frame.
std::vector<glm::mat4> Expected(const Stage &stage, const std::vector<Geometry::JointTransform> &pose)
{
    std::vector<glm::mat4> model(stage.skeleton.JointCount(), glm::mat4(1.f));
    Geometry::JointModelTransforms(stage.skeleton, pose, model);
    return model;
}

bool Near(const glm::mat4 &a, const glm::mat4 &b)
{
    for (glm::length_t column = 0; column < 4; ++column)
    {
        for (glm::length_t row = 0; row < 4; ++row)
        {
            if (std::abs(a[column][row] - b[column][row]) > kNear)
            {
                return false;
            }
        }
    }
    return true;
}

} // namespace

TEST_CASE("Pose stage: a system after it reads the joints the pose was given this frame, with no renderer")
{
    Stage stage;
    std::vector<glm::mat4> seen;
    stage.world.systems.Register(App::SystemPhase::Update, "Turn", [&stage](App::SystemContext &) { Turn(stage); });
    stage.world.systems.Register(App::SystemPhase::Ui, "Read",
                                 [&stage, &seen](App::SystemContext &) { seen = stage.Pose().jointModel; });

    // The first frame binds the pose to the skeleton; the code writes it from then on.
    stage.Frame();
    for (int32_t frame = 0; frame < 3; ++frame)
    {
        stage.Frame();
        CAPTURE(frame);
        REQUIRE(stage.Pose().pose.size() == stage.skeleton.JointCount());
        const std::vector<glm::mat4> expected = Expected(stage, stage.Pose().pose);
        REQUIRE(seen.size() == expected.size());
        CHECK(Near(seen[static_cast<std::size_t>(kTurnedJoint)], expected[static_cast<std::size_t>(kTurnedJoint)]));
    }
}

TEST_CASE("Pose stage: a change to the final pose lasts one frame and never reaches the animated pose")
{
    Stage stage;
    stage.Frame();
    Runtime::SkinnedPose &skinned = stage.Pose();
    REQUIRE(skinned.finalPose.size() == stage.skeleton.JointCount());
    const std::vector<Geometry::JointTransform> animated = skinned.pose;

    // As a step that adjusts the pose after the animation would.
    const glm::quat turned = glm::angleAxis(kTurnPerFrame, glm::vec3(1.f, 0.f, 0.f));
    skinned.finalPose[static_cast<std::size_t>(kTurnedJoint)].Rotation = turned;
    stage.Frame();
    CHECK(skinned.pose[static_cast<std::size_t>(kTurnedJoint)].Rotation ==
          animated[static_cast<std::size_t>(kTurnedJoint)].Rotation);
    CHECK(skinned.finalPose[static_cast<std::size_t>(kTurnedJoint)].Rotation ==
          animated[static_cast<std::size_t>(kTurnedJoint)].Rotation);
}

TEST_CASE("Pose stage: what Update moved is placed, children too, before anything after it reads it")
{
    Stage stage;
    const ECS::Entity child = stage.world.scene.Create();
    REQUIRE(stage.world.scene.Add(child, ECS::Transform{}) != nullptr);
    REQUIRE(stage.world.scene.Add(child, ECS::Parent{stage.character}) != nullptr);
    stage.Frame();
    stage.world.systems.Register(App::SystemPhase::Update, "Move", [&stage](App::SystemContext &)
                                 { stage.world.scene.GetMut<ECS::Transform>(stage.character)->position.x = kMove; });

    stage.Frame();
    const ECS::WorldMatrix *placed = stage.world.scene.Get<ECS::WorldMatrix>(child);
    REQUIRE(placed != nullptr);
    CHECK(ECS::WorldPositionOf(*placed).x == doctest::Approx(kMove));
}

TEST_CASE("Pose stage: the frame's drawing poses a world only when nothing has this frame")
{
    Stage stage;
    stage.Frame();
    Runtime::SkinnedPose &skinned = stage.Pose();
    REQUIRE(skinned.jointModel.size() == stage.skeleton.JointCount());
    const glm::mat4 before = skinned.jointModel[static_cast<std::size_t>(kTurnedJoint)];
    const glm::quat rest = skinned.pose[static_cast<std::size_t>(kTurnedJoint)].Rotation;
    skinned.pose[static_cast<std::size_t>(kTurnedJoint)].Rotation =
        glm::angleAxis(kTurnPerFrame, glm::vec3(0.f, 1.f, 0.f));

    // Posed already this frame, as a world that ran its Update systems was.
    App::PoseWorld(stage.world);
    const glm::mat4 posed = skinned.jointModel[static_cast<std::size_t>(kTurnedJoint)];
    CHECK_FALSE(Near(posed, before));
    skinned.pose[static_cast<std::size_t>(kTurnedJoint)].Rotation = rest;
    App::EnsurePosed(stage.world, nullptr);
    CHECK(Near(skinned.jointModel[static_cast<std::size_t>(kTurnedJoint)], posed));

    // A new frame that ran no Update, as in the editor's edit mode or paused.
    App::EndWorldFrame(stage.world, stage.universal, false);
    App::EnsurePosed(stage.world, nullptr);
    CHECK(Near(skinned.jointModel[static_cast<std::size_t>(kTurnedJoint)], before));
}
