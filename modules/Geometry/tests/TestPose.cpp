/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPose.cpp
/// @brief A pose becomes model-space joints and skinning matrices: a child
/// follows its parent, the rest pose moves nothing, and transforms compose in
/// the order glTF defines.

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <vector>

#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Geometry/Pose.hpp>
#include <Assisi/Math/GLM.hpp>

using Assisi::Geometry::FindJoint;
using Assisi::Geometry::JointModelTransforms;
using Assisi::Geometry::JointTransform;
using Assisi::Geometry::kNoJoint;
using Assisi::Geometry::kNoParent;
using Assisi::Geometry::Skeleton;
using Assisi::Geometry::SkinningPalette;

namespace
{

constexpr float kQuarterTurn = glm::half_pi<float>();

/// A root at the origin and a child one unit above it, bound at that rest pose:
/// each inverse bind is the inverse of the joint's rest model transform.
Skeleton TwoJointSkeleton(const glm::mat4 &rootTransform = glm::mat4(1.f))
{
    Skeleton skeleton;
    skeleton.RootTransform = rootTransform;
    skeleton.Names = {"root", "child"};
    skeleton.Parents = {kNoParent, 0};
    JointTransform child;
    child.Translation = {0.f, 1.f, 0.f};
    skeleton.RestLocal = {JointTransform{}, child};
    const glm::mat4 rootModel = rootTransform;
    const glm::mat4 childModel = rootModel * glm::translate(glm::mat4(1.f), glm::vec3(0.f, 1.f, 0.f));
    skeleton.InverseBind = {glm::inverse(rootModel), glm::inverse(childModel)};
    return skeleton;
}

void CheckMatrix(const glm::mat4 &actual, const glm::mat4 &expected)
{
    for (int32_t column = 0; column < 4; ++column)
    {
        for (int32_t row = 0; row < 4; ++row)
        {
            CAPTURE(column);
            CAPTURE(row);
            CHECK(actual[column][row] == doctest::Approx(expected[column][row]));
        }
    }
}

struct Evaluated
{
    std::array<glm::mat4, 2> model{};
    std::array<glm::mat4, 2> palette{};
};

Evaluated Evaluate(const Skeleton &skeleton, const std::vector<JointTransform> &pose)
{
    Evaluated evaluated;
    JointModelTransforms(skeleton, pose, evaluated.model);
    SkinningPalette(skeleton, evaluated.model, evaluated.palette);
    return evaluated;
}

} // namespace

TEST_CASE("Pose: the rest pose moves nothing")
{
    const Skeleton skeleton = TwoJointSkeleton();
    const Evaluated evaluated = Evaluate(skeleton, skeleton.RestLocal);

    CheckMatrix(evaluated.palette[0], glm::mat4(1.f));
    CheckMatrix(evaluated.palette[1], glm::mat4(1.f));
    CHECK(evaluated.model[1][3].y == doctest::Approx(1.f));
}

TEST_CASE("Pose: a child joint follows its parent's rotation")
{
    const Skeleton skeleton = TwoJointSkeleton();
    std::vector<JointTransform> pose = skeleton.RestLocal;
    pose[0].Rotation = glm::angleAxis(kQuarterTurn, glm::vec3(0.f, 0.f, 1.f));

    const Evaluated evaluated = Evaluate(skeleton, pose);

    // A quarter turn about Z swings the child, one unit up, to one unit along -X.
    CHECK(evaluated.model[1][3].x == doctest::Approx(-1.f));
    CHECK(evaluated.model[1][3].y == doctest::Approx(0.f).epsilon(1e-5));

    // A vertex bound to the child at (0, 1, 0) moves with it.
    const glm::vec4 moved = evaluated.palette[1] * glm::vec4(0.f, 1.f, 0.f, 1.f);
    CHECK(moved.x == doctest::Approx(-1.f));
    CHECK(moved.y == doctest::Approx(0.f).epsilon(1e-5));
}

TEST_CASE("Pose: the skeleton's root transform places the root joints")
{
    const glm::mat4 lifted = glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.f, 5.f));
    const Skeleton skeleton = TwoJointSkeleton(lifted);
    const Evaluated evaluated = Evaluate(skeleton, skeleton.RestLocal);

    CHECK(evaluated.model[0][3].z == doctest::Approx(5.f));
    CHECK(evaluated.model[1][3].z == doctest::Approx(5.f));
    // Bound at that same rest, so skinning still moves nothing.
    CheckMatrix(evaluated.palette[0], glm::mat4(1.f));
    CheckMatrix(evaluated.palette[1], glm::mat4(1.f));
}

TEST_CASE("Pose: a joint scales and rotates about itself, then moves")
{
    JointTransform local;
    local.Translation = {1.f, 0.f, 0.f};
    local.Rotation = glm::angleAxis(kQuarterTurn, glm::vec3(0.f, 0.f, 1.f));
    local.Scale = {2.f, 2.f, 2.f};
    const glm::mat4 matrix = Assisi::Geometry::JointMatrix(local);

    // Translation applied last, so neither scale nor rotation moves the joint.
    CHECK(matrix[3].x == doctest::Approx(1.f));
    CHECK(matrix[3].y == doctest::Approx(0.f).epsilon(1e-5));
    // A point one unit along X is doubled, turned to +Y, then moved.
    const glm::vec4 point = matrix * glm::vec4(1.f, 0.f, 0.f, 1.f);
    CHECK(point.x == doctest::Approx(1.f));
    CHECK(point.y == doctest::Approx(2.f));
}

TEST_CASE("Pose: a joint is found by name")
{
    const Skeleton skeleton = TwoJointSkeleton();
    CHECK(FindJoint(skeleton, "child") == 1);
    CHECK(FindJoint(skeleton, "root") == 0);
    CHECK(FindJoint(skeleton, "tail") == kNoJoint);
}
