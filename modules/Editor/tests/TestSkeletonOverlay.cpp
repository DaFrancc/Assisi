/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestSkeletonOverlay.cpp
/// @brief A skeleton is drawn as one line per bone and a cross per joint, where
/// the entity puts it, and the cursor names the joint drawn under it.

#include <doctest/doctest.h>

#include <cmath>
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

/// Vertices per line segment, segments per joint cross (one along each axis), and
/// segments per bone diamond (four to the ring, four around it, four to the tip).
constexpr std::size_t kVerticesPerLine = 2;
constexpr std::size_t kLinesPerCross = 3;
constexpr std::size_t kLinesPerBone = 12;

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

TEST_CASE("Skeleton overlay: each bone is a diamond, widest near its parent and pointed at its child")
{
    Skeleton skeleton;
    skeleton.Names = {"root", "up"};
    skeleton.Parents = {kNoParent, 0};
    skeleton.RestLocal.resize(2);
    skeleton.InverseBind.resize(2, glm::mat4(1.f));
    const std::vector<glm::mat4> joints{glm::mat4(1.f), glm::translate(glm::mat4(1.f), glm::vec3(0.f, 1.f, 0.f))};
    const glm::mat4 world = glm::translate(glm::mat4(1.f), glm::vec3(10.f, 0.f, 0.f));

    std::vector<LineVertex> lines;
    AppendSkeletonLines(lines, skeleton, joints, world, kColor);

    const glm::vec3 parent{10.f, 0.f, 0.f};
    const glm::vec3 child{10.f, 1.f, 0.f};
    REQUIRE(lines.size() == (kLinesPerBone + skeleton.JointCount() * kLinesPerCross) * kVerticesPerLine);

    // The bone's segments are the ones longer than a joint cross's arms. Every
    // point of them that is not one of the bone's two ends is on the ring of four
    // corners: off the bone's axis, and nearer the parent than the child.
    constexpr float kCrossSpan = 2.f * Assisi::Editor::kJointMarkerHalfSize;
    uint32_t boneSegments = 0;
    uint32_t parentEnds = 0;
    uint32_t childEnds = 0;
    for (std::size_t i = 0; i + 1 < lines.size(); i += kVerticesPerLine)
    {
        if (glm::distance(lines[i].position, lines[i + 1].position) <= kCrossSpan + 1e-5f)
        {
            continue;
        }
        ++boneSegments;
        for (std::size_t end = i; end < i + kVerticesPerLine; ++end)
        {
            const glm::vec3 point = lines[end].position;
            if (glm::distance(point, parent) < 1e-5f)
            {
                ++parentEnds;
                continue;
            }
            if (glm::distance(point, child) < 1e-5f)
            {
                ++childEnds;
                continue;
            }
            CAPTURE(end);
            CHECK(glm::distance(point, parent) < glm::distance(point, child));
            CHECK(glm::length(glm::vec2(point.x - 10.f, point.z)) > 0.f);
        }
    }
    CHECK(boneSegments == kLinesPerBone);
    // Four edges run from the parent to the ring, and four from the ring to the child.
    CHECK(parentEnds == 4);
    CHECK(childEnds == 4);
    for (const LineVertex &vertex : lines)
    {
        CHECK(vertex.color == kColor);
    }
}

TEST_CASE("Skeleton overlay: every bone of a branching skeleton is drawn")
{
    const Skeleton skeleton = ForkedSkeleton();
    std::vector<LineVertex> lines;
    AppendSkeletonLines(lines, skeleton, ForkedJoints(), glm::mat4(1.f), kColor);

    const std::size_t bones = 2;
    CHECK(lines.size() == (bones * kLinesPerBone + skeleton.JointCount() * kLinesPerCross) * kVerticesPerLine);
}

TEST_CASE("Skeleton overlay: a bone of zero length draws no diamond")
{
    // Its direction is undefined, so there is no ring to build around it.
    Skeleton skeleton;
    skeleton.Names = {"root", "same"};
    skeleton.Parents = {kNoParent, 0};
    skeleton.RestLocal.resize(2);
    skeleton.InverseBind.resize(2, glm::mat4(1.f));
    const std::vector<glm::mat4> joints{glm::mat4(1.f), glm::mat4(1.f)};

    std::vector<LineVertex> lines;
    AppendSkeletonLines(lines, skeleton, joints, glm::mat4(1.f), kColor);

    CHECK(lines.size() == skeleton.JointCount() * kLinesPerCross * kVerticesPerLine);
    for (const LineVertex &vertex : lines)
    {
        CHECK(std::isfinite(vertex.position.x));
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
