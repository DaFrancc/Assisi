/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestJointAttachment.cpp
/// @brief An entity held at a joint of its parent's skinned mesh, with no
/// renderer: placed from this frame's pose before anything after the pose stage
/// reads it, its Transform left as the offset its author gave it, the joint
/// found again when the mesh changes, and a joint the mesh lacks reported once.

#include "LogCapture.hpp"
#include "TestRig.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/App/WorldFrame.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/InternedString.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/ParentFrame.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/ECS/WorldMatrix.hpp>
#include <Assisi/Geometry/Pose.hpp>
#include <Assisi/Math/Matrix.hpp>
#include <Assisi/Physics/CollisionSource.hpp>
#include <Assisi/Render/MeshBuffer.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

using namespace Assisi;

namespace
{

constexpr float kStep = 1.f / 60.f;

/// What the GPU would have given each mesh; any ids but the unbound one.
constexpr uint32_t kMeshId = 1;
constexpr uint32_t kOtherMeshId = 2;

/// The joint each frame turns, how far it turns it each frame (radians), and
/// the joint the item is held at, which the turn swings.
constexpr std::string_view kTurnedJoint = "limb_l_0";
constexpr float kTurnPerFrame = 0.1f;
constexpr std::string_view kHeldAt = "limb_l_end";

/// Where the character stands, the item's offset from the joint, and its own
/// child's offset from it (m).
constexpr glm::vec3 kCharacterAt{3.f, 0.f, 0.f};
constexpr glm::vec3 kOffset{0.f, 0.1f, 0.f};
constexpr glm::vec3 kChildOffset{0.2f, 0.f, 0.f};

/// How close a placed position must be (m).
constexpr float kNear = 1e-4f;

/// The two limbs in the other order, so each joint of the second sits at
/// another index than in the first.
Runtime::Testing::RigShape SwappedLimbs()
{
    Runtime::Testing::RigShape rig = Runtime::Testing::TwoLimbs();
    std::swap(rig.limbs[0], rig.limbs[1]);
    return rig;
}

/// A world with no renderer: a character skinned to a rig built in code, and an
/// item it holds at a joint.
struct Hold
{
    explicit Hold(std::string_view joint = kHeldAt)
    {
        mesh.KeepWithoutGpu(Runtime::Testing::BuildSkinnedBoxes(skeleton));
        mesh.SetId(kMeshId);
        otherMesh.KeepWithoutGpu(Runtime::Testing::BuildSkinnedBoxes(otherSkeleton));
        otherMesh.SetId(kOtherMeshId);

        ECS::Scene &scene = world.scene;
        character = scene.Create();
        REQUIRE(scene.Add(character, ECS::Transform{.position = glm::dvec3(kCharacterAt)}) != nullptr);
        Runtime::MeshRenderer renderer;
        renderer.meshBuffer = &mesh;
        REQUIRE(scene.Add(character, std::move(renderer)) != nullptr);
        REQUIRE(scene.Add(character, Runtime::SkinnedMesh{}) != nullptr);

        item = scene.Create();
        REQUIRE(scene.Add(item, ECS::Transform{.position = glm::dvec3(kOffset)}) != nullptr);
        REQUIRE(scene.Add(item, ECS::Parent{.parent = character}) != nullptr);
        REQUIRE(scene.Add(item, Runtime::JointAttachment{.joint = Core::InternedString{joint}}) != nullptr);
    }

    /// One frame after its fixed steps: Update, the pose stage, then Ui.
    void Frame()
    {
        const App::SystemContext ctx{world, kStep, 0, nullptr, nullptr, universal, true, nullptr};
        App::UpdateWorld(ctx);
        App::PoseWorld(world);
        App::PresentWorld(ctx);
        App::EndWorldFrame(world, universal, true);
        ++frame;
    }

    Runtime::SkinnedPose &Pose() { return *world.scene.Get<Runtime::SkinnedPose>(character); }

    glm::vec3 PlacedAt(ECS::Entity entity) const
    {
        return glm::vec3(ECS::WorldPositionOf(*world.scene.Get<ECS::WorldMatrix>(entity)));
    }

    /// Where @p offset from joint @p name of @p bones, in this frame's pose, is in the world.
    glm::vec3 AtJoint(const Geometry::Skeleton &bones, std::string_view name, glm::vec3 offset)
    {
        const int32_t joint = Geometry::FindJoint(bones, name);
        REQUIRE(joint != Geometry::kNoJoint);
        const glm::mat4 &model = Pose().jointModel[static_cast<std::size_t>(joint)];
        return kCharacterAt + glm::vec3(model * glm::vec4(offset, 1.f));
    }

    const Geometry::Skeleton skeleton = Runtime::Testing::BuildSkeleton(Runtime::Testing::TwoLimbs());
    const Geometry::Skeleton otherSkeleton = Runtime::Testing::BuildSkeleton(SwappedLimbs());
    Render::MeshBuffer mesh;
    Render::MeshBuffer otherMesh;
    App::World world{Physics::NoCollisionAssets()};
    Core::EventQueue universal{Core::EventScope::Universal};
    ECS::Entity character = ECS::NullEntity;
    ECS::Entity item = ECS::NullEntity;
    int32_t frame = 0;
};

/// Swings the limb further each frame, as an animation would.
void Swing(Hold &hold)
{
    std::vector<Geometry::JointTransform> &pose = hold.Pose().pose;
    const int32_t joint = Geometry::FindJoint(hold.skeleton, kTurnedJoint);
    if (pose.size() != hold.skeleton.JointCount() || joint == Geometry::kNoJoint)
    {
        return;
    }
    pose[static_cast<std::size_t>(joint)].Rotation =
        glm::angleAxis(kTurnPerFrame * static_cast<float>(hold.frame + 1), glm::vec3(1.f, 0.f, 0.f));
}

bool Near(glm::vec3 a, glm::vec3 b)
{
    return glm::length(a - b) < kNear;
}

} // namespace

TEST_CASE("Joint attachment: a held item is where this frame's joint is before anything after the pose stage reads it")
{
    Hold hold;
    std::optional<glm::vec3> seen;
    hold.world.systems.Register(App::SystemPhase::Update, "Swing", [&hold](App::SystemContext &) { Swing(hold); });
    hold.world.systems.Register(App::SystemPhase::Ui, "Read",
                                [&hold, &seen](App::SystemContext &) { seen = hold.PlacedAt(hold.item); });

    // The first frame binds the pose to the skeleton; the limb swings from then on.
    hold.Frame();
    for (int32_t frame = 0; frame < 3; ++frame)
    {
        hold.Frame();
        CAPTURE(frame);
        REQUIRE(seen.has_value());
        const glm::vec3 expected = hold.AtJoint(hold.skeleton, kHeldAt, kOffset);
        CHECK(Near(*seen, expected));
    }
    // The item's own children hang from it.
    const ECS::Entity child = hold.world.scene.Create();
    REQUIRE(hold.world.scene.Add(child, ECS::Transform{.position = glm::dvec3(kChildOffset)}) != nullptr);
    REQUIRE(hold.world.scene.Add(child, ECS::Parent{.parent = hold.item}) != nullptr);
    hold.Frame();
    CHECK(Near(hold.PlacedAt(child), hold.AtJoint(hold.skeleton, kHeldAt, kOffset + kChildOffset)));
}

TEST_CASE("Joint attachment: the item's Transform stays its offset, and a saved level keeps the joint, not the pose")
{
    Hold hold;
    hold.world.systems.Register(App::SystemPhase::Update, "Swing", [&hold](App::SystemContext &) { Swing(hold); });
    for (int32_t frame = 0; frame < 3; ++frame)
    {
        hold.Frame();
    }
    REQUIRE_FALSE(Near(hold.PlacedAt(hold.item), kCharacterAt + kOffset));
    CHECK(glm::vec3(hold.world.scene.Get<ECS::Transform>(hold.item)->position) == kOffset);

    ECS::Scene loaded;
    REQUIRE(Runtime::SceneSerializer::Load(loaded, Runtime::SceneSerializer::Save(hold.world.scene)).has_value());
    int32_t held = 0;
    for (auto [entity, attachment, transform] : loaded.Query<Runtime::JointAttachment, ECS::Transform>())
    {
        ++held;
        CHECK(attachment.joint.View() == kHeldAt);
        CHECK(glm::vec3(transform.position) == kOffset);
        // Worked out again from the pose once the level runs; nothing of it was saved.
        const ECS::ParentFrame *frame = loaded.Get<ECS::ParentFrame>(entity);
        CHECK((frame == nullptr || frame->position == glm::vec3(0.f)));
    }
    CHECK(held == 1);
}

TEST_CASE("Joint attachment: after the mesh changes to one with its joints in another order, it holds the same joint")
{
    Hold hold;
    hold.Frame();
    REQUIRE(Geometry::FindJoint(hold.skeleton, kHeldAt) != Geometry::FindJoint(hold.otherSkeleton, kHeldAt));
    REQUIRE(Near(hold.PlacedAt(hold.item), hold.AtJoint(hold.skeleton, kHeldAt, kOffset)));

    hold.world.scene.Get<Runtime::MeshRenderer>(hold.character)->meshBuffer = &hold.otherMesh;
    hold.Frame();
    CHECK(Near(hold.PlacedAt(hold.item), hold.AtJoint(hold.otherSkeleton, kHeldAt, kOffset)));
}

TEST_CASE("Joint attachment: a joint the mesh lacks is reported once, and the item hangs from its parent's origin")
{
    const Assisi::Tests::LogCapture log;
    Hold hold("no_such_joint");
    for (int32_t frame = 0; frame < 3; ++frame)
    {
        hold.Frame();
    }
    CHECK(Near(hold.PlacedAt(hold.item), kCharacterAt + kOffset));
    CHECK(log.Count("no_such_joint") == 1);
}

TEST_CASE("Joint attachment: handed to a parent with no skeleton, the item hangs from that parent's origin")
{
    Hold hold;
    hold.Frame();
    REQUIRE_FALSE(Near(hold.PlacedAt(hold.item), kCharacterAt + kOffset));

    // A rack with no pose to read: the hand's pose must not come with the item.
    constexpr glm::vec3 kRackAt{-2.f, 1.f, 0.f};
    const ECS::Entity rack = hold.world.scene.Create();
    REQUIRE(hold.world.scene.Add(rack, ECS::Transform{.position = glm::dvec3(kRackAt)}) != nullptr);
    hold.world.scene.GetMut<ECS::Parent>(hold.item)->parent = rack;
    hold.Frame();
    CHECK(Near(hold.PlacedAt(hold.item), kRackAt + kOffset));

    // Handed back, it is at the hand again.
    hold.world.scene.GetMut<ECS::Parent>(hold.item)->parent = hold.character;
    hold.Frame();
    CHECK(Near(hold.PlacedAt(hold.item), hold.AtJoint(hold.skeleton, kHeldAt, kOffset)));
}

TEST_CASE("Joint attachment: taking the attachment off hangs the item from its parent's origin again")
{
    Hold hold;
    hold.Frame();
    REQUIRE_FALSE(Near(hold.PlacedAt(hold.item), kCharacterAt + kOffset));

    REQUIRE(hold.world.scene.Remove<Runtime::JointAttachment>(hold.item) == ECS::RemoveResult::Removed);
    hold.Frame();
    CHECK(Near(hold.PlacedAt(hold.item), kCharacterAt + kOffset));
}
