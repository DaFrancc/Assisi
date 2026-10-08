/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestSkeletonOverlay.cpp
/// @brief A skeleton is drawn as one line per bone and a cross per joint, where
/// the entity puts it, and the cursor names the joint drawn under it.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include <Assisi/Editor/SkeletonOverlay.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Geometry/Pose.hpp>
#include <Assisi/Math/GLM.hpp>

using Assisi::Editor::AppendSkeletonLines;
using Assisi::Editor::JointUnderCursor;
using Assisi::Editor::kJointHoverPixels;
using Assisi::Editor::LineVertex;
using Assisi::Editor::PickRay;
using Assisi::Geometry::kNoJoint;
using Assisi::Geometry::kNoParent;
using Assisi::Geometry::Skeleton;

namespace
{

/// Vertices per line segment, and segments per joint cross (one along each axis).
constexpr std::size_t kVerticesPerLine = 2;
constexpr std::size_t kLinesPerCross = 3;

constexpr glm::vec4 kColor{0.2f, 0.6f, 1.f, 1.f};

/// A root with two children: a chain of one joint above it and one to its side.
Skeleton ForkedSkeleton()
{
    Skeleton skeleton;
    skeleton.Names = {"root", "up", "side"};
    skeleton.Parents = {kNoParent, 0, 0};
    skeleton.RestLocal.resize(3);
    skeleton.InverseBind.resize(3, glm::mat4(1.f));
    return skeleton;
}

std::vector<glm::mat4> ForkedJoints()
{
    return {glm::mat4(1.f), glm::translate(glm::mat4(1.f), glm::vec3(0.f, 1.f, 0.f)),
            glm::translate(glm::mat4(1.f), glm::vec3(1.f, 0.f, 0.f))};
}

bool HasSegment(const std::vector<LineVertex> &lines, glm::vec3 a, glm::vec3 b)
{
    constexpr float kTolerance = 1e-5f;
    for (std::size_t i = 0; i + 1 < lines.size(); i += kVerticesPerLine)
    {
        const glm::vec3 p = lines[i].position;
        const glm::vec3 q = lines[i + 1].position;
        const bool forward = glm::distance(p, a) < kTolerance && glm::distance(q, b) < kTolerance;
        const bool backward = glm::distance(p, b) < kTolerance && glm::distance(q, a) < kTolerance;
        if (forward || backward)
        {
            return true;
        }
    }
    return false;
}

/// A camera at (0, 0, 5) looking down -Z at the origin, onto a 1000×1000 view.
PickRay Camera()
{
    constexpr float kViewportPixels = 1000.f;
    constexpr float kFovRadians = glm::half_pi<float>();
    constexpr float kNear = 0.1f;
    constexpr float kFar = 100.f;
    PickRay ray;
    ray.origin = {0.f, 0.f, 5.f};
    const glm::mat4 view = glm::lookAt(ray.origin, glm::vec3(0.f), glm::vec3(0.f, 1.f, 0.f));
    ray.viewProjection = glm::perspective(kFovRadians, 1.f, kNear, kFar) * view;
    ray.viewportSize = {kViewportPixels, kViewportPixels};
    ray.valid = true;
    return ray;
}

/// The viewport's centre, where the origin is drawn.
constexpr glm::vec2 kCentre{500.f, 500.f};

} // namespace

TEST_CASE("Skeleton overlay: one line per bone, between joint and parent, placed by the entity")
{
    const Skeleton skeleton = ForkedSkeleton();
    const std::vector<glm::mat4> joints = ForkedJoints();
    const glm::mat4 world = glm::translate(glm::mat4(1.f), glm::vec3(10.f, 0.f, 0.f));

    std::vector<LineVertex> lines;
    AppendSkeletonLines(lines, skeleton, joints, world, kColor);

    CHECK(HasSegment(lines, {10.f, 0.f, 0.f}, {10.f, 1.f, 0.f}));
    CHECK(HasSegment(lines, {10.f, 0.f, 0.f}, {11.f, 0.f, 0.f}));
    // Two bones, plus a cross at each of the three joints.
    const std::size_t bones = 2;
    CHECK(lines.size() == (bones + skeleton.JointCount() * kLinesPerCross) * kVerticesPerLine);
    for (const LineVertex &vertex : lines)
    {
        CHECK(vertex.color == kColor);
    }
}

TEST_CASE("Skeleton overlay: a lone root joint still shows, as a cross")
{
    Skeleton skeleton;
    skeleton.Names = {"root"};
    skeleton.Parents = {kNoParent};
    skeleton.RestLocal.resize(1);
    skeleton.InverseBind.resize(1, glm::mat4(1.f));
    const std::vector<glm::mat4> joints{glm::mat4(1.f)};

    std::vector<LineVertex> lines;
    AppendSkeletonLines(lines, skeleton, joints, glm::mat4(1.f), kColor);

    REQUIRE(lines.size() == kLinesPerCross * kVerticesPerLine);
    // The cross is centred on the joint.
    glm::vec3 centre{0.f};
    for (const LineVertex &vertex : lines)
    {
        centre += vertex.position;
    }
    centre /= static_cast<float>(lines.size());
    CHECK(glm::length(centre) == doctest::Approx(0.f));
}

TEST_CASE("Skeleton overlay: the cursor names the joint drawn nearest it")
{
    const std::vector<glm::mat4> joints = ForkedJoints();
    float pixels = 0.f;

    // The root is drawn at the centre of the view.
    CHECK(JointUnderCursor(Camera(), kCentre + glm::vec2(2.f, -1.f), joints, glm::mat4(1.f), pixels) == 0);
    CHECK(pixels < kJointHoverPixels);

    // "up" is one unit above, so drawn above the centre: screen y grows downwards.
    float upPixels = 0.f;
    const glm::vec4 clip = Camera().viewProjection * glm::vec4(0.f, 1.f, 0.f, 1.f);
    const glm::vec2 upOnScreen{(clip.x / clip.w + 1.f) * 500.f, (1.f - clip.y / clip.w) * 500.f};
    CHECK(upOnScreen.y < kCentre.y);
    CHECK(JointUnderCursor(Camera(), upOnScreen, joints, glm::mat4(1.f), upPixels) == 1);
}

TEST_CASE("Skeleton overlay: a cursor far from every joint names none")
{
    const std::vector<glm::mat4> joints = ForkedJoints();
    float pixels = 0.f;
    CHECK(JointUnderCursor(Camera(), kCentre + glm::vec2(0.f, 300.f), joints, glm::mat4(1.f), pixels) == kNoJoint);
}

TEST_CASE("Skeleton overlay: a joint behind the eye is never named")
{
    // Dividing a point behind the camera through its negative w mirrors it to the
    // front, where it can land under the cursor.
    const std::vector<glm::mat4> joints{glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.f, 10.f))};
    float pixels = 0.f;
    CHECK(JointUnderCursor(Camera(), kCentre, joints, glm::mat4(1.f), pixels) == kNoJoint);
}
