/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestParentFrame.cpp
/// @brief A ParentFrame sits between a parent's world matrix and its child's
/// Transform: placing composes it, writing it places the child again, removing
/// it puts the child back on its parent's origin, and the questions about a
/// child's parent space answer with it.

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>

#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/ParentFrame.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/ECS/WorldMatrix.hpp>
#include <Assisi/Math/Matrix.hpp>

using namespace Assisi;
using Assisi::ECS::Parent;
using Assisi::ECS::ParentFrame;
using Assisi::ECS::PropagateTransforms;
using Assisi::ECS::Transform;
using Assisi::Math::TranslationOf;

namespace
{

/// The parent stands 10 m along x; the frame is 2 m up it, turned a quarter
/// about y; the child is 1 m along the frame's x, which the turn points down -z.
constexpr float kParentX = 10.f;
constexpr float kFrameY = 2.f;
constexpr float kChildX = 1.f;

const glm::quat kQuarterTurn = glm::angleAxis(glm::radians(90.f), glm::vec3(0.f, 1.f, 0.f));

struct Family
{
    Family()
    {
        parent = scene.Create();
        child = scene.Create();
        REQUIRE(scene.Add(parent, Transform{.position = {kParentX, 0.f, 0.f}}) != nullptr);
        REQUIRE(scene.Add(child, Transform{.position = {kChildX, 0.f, 0.f}}) != nullptr);
        REQUIRE(scene.Add(child, Parent{.parent = parent}) != nullptr);
    }

    glm::vec3 ChildAt() const { return TranslationOf(scene.Get<ECS::WorldMatrix>(child)->inCell); }

    ECS::Scene scene;
    ECS::Entity parent = ECS::NullEntity;
    ECS::Entity child = ECS::NullEntity;
};

} // namespace

TEST_CASE("ParentFrame: a child is placed at its parent, then the frame, then its own Transform")
{
    Family family;
    REQUIRE(family.scene.Add(family.child, ParentFrame{.rotation = kQuarterTurn, .position = {0.f, kFrameY, 0.f}}) !=
            nullptr);
    PropagateTransforms(family.scene, 0);

    const glm::vec3 at = family.ChildAt();
    CHECK(at.x == doctest::Approx(kParentX));
    CHECK(at.y == doctest::Approx(kFrameY));
    CHECK(at.z == doctest::Approx(-kChildX));
}

TEST_CASE("ParentFrame: writing it places the child again though nothing else moved")
{
    Family family;
    REQUIRE(family.scene.Add(family.child, ParentFrame{}) != nullptr);
    const uint64_t tick = PropagateTransforms(family.scene, 0);
    REQUIRE(family.ChildAt().y == doctest::Approx(0.f));

    family.scene.GetMut<ParentFrame>(family.child)->position.y = kFrameY;
    PropagateTransforms(family.scene, tick);
    CHECK(family.ChildAt().y == doctest::Approx(kFrameY));
}

TEST_CASE("ParentFrame: removing it hangs the child from its parent's origin again")
{
    Family family;
    REQUIRE(family.scene.Add(family.child, ParentFrame{.position = {0.f, kFrameY, 0.f}}) != nullptr);
    const uint64_t tick = PropagateTransforms(family.scene, 0);
    REQUIRE(family.ChildAt().y == doctest::Approx(kFrameY));

    REQUIRE(family.scene.Remove<ParentFrame>(family.child) == ECS::RemoveResult::Removed);
    PropagateTransforms(family.scene, tick);
    CHECK(family.ChildAt().y == doctest::Approx(0.f));
    CHECK(family.ChildAt().x == doctest::Approx(kParentX + kChildX));
}

TEST_CASE("ParentFrame: the child's parent space and world pose include it")
{
    Family family;
    REQUIRE(family.scene.Add(family.child, ParentFrame{.rotation = kQuarterTurn, .position = {0.f, kFrameY, 0.f}}) !=
            nullptr);
    PropagateTransforms(family.scene, 0);

    // What an editor turns a dragged world pose back into a Transform with.
    const std::optional<ECS::WorldMatrix> space = ECS::ParentWorldMatrix(family.scene, family.child);
    REQUIRE(space.has_value());
    const glm::vec3 origin = TranslationOf(space->inCell);
    CHECK(origin.x == doctest::Approx(kParentX));
    CHECK(origin.y == doctest::Approx(kFrameY));

    const Transform world = ECS::WorldTransformOf(family.scene, family.child);
    CHECK(world.position.x == doctest::Approx(kParentX));
    CHECK(world.position.y == doctest::Approx(kFrameY));
    CHECK(world.position.z == doctest::Approx(-kChildX));
}
