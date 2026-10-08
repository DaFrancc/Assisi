/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Editor/SkeletonOverlay.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Geometry/Pose.hpp>
#include <Assisi/Math/Matrix.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace Assisi::Editor
{

namespace
{

/// The three axes a joint's cross is drawn along.
constexpr std::array<glm::vec3, 3> kCrossAxes{glm::vec3(1.f, 0.f, 0.f), glm::vec3(0.f, 1.f, 0.f),
                                              glm::vec3(0.f, 0.f, 1.f)};

/// How far along a bone its diamond is widest, and how wide it is there, both as a
/// fraction of the bone's length: thick at the parent, so the eye reads the
/// direction from the shape.
constexpr float kBoneWidestAlong = 0.1f;
constexpr float kBoneWidestRadius = 0.1f;

/// Shorter than this, a bone has no direction to build its diamond around.
constexpr float kMinBoneLength = 1e-6f;

/// Rotates a bone's first ring corner to each of the next three.
constexpr float kQuarterTurn = glm::half_pi<float>();
constexpr uint32_t kRingCorners = 4;

void AppendLine(std::vector<LineVertex> &out, const glm::vec3 &a, const glm::vec3 &b, const glm::vec4 &color)
{
    out.push_back(LineVertex{.position = a, .color = color});
    out.push_back(LineVertex{.position = b, .color = color});
}

/// How close to straight up or down an axis may point before crossing it with
/// the up vector loses precision, and the side vector is used instead.
constexpr float kNearlyVertical = 0.9f;

/// A unit vector at right angles to the unit @p axis.
glm::vec3 Perpendicular(const glm::vec3 &axis)
{
    const glm::vec3 helper =
        std::abs(axis.y) < kNearlyVertical ? glm::vec3(0.f, 1.f, 0.f) : glm::vec3(1.f, 0.f, 0.f);
    return glm::normalize(glm::cross(axis, helper));
}

/// The diamond from @p parent to @p child: four edges out to a square ring near
/// the parent, the ring itself, and four edges in to a point at the child.
void AppendBone(std::vector<LineVertex> &out, const glm::vec3 &parent, const glm::vec3 &child, const glm::vec4 &color)
{
    const glm::vec3 along = child - parent;
    const float length = glm::length(along);
    if (length < kMinBoneLength)
    {
        return;
    }
    const glm::vec3 axis = along / length;
    const glm::vec3 ringCentre = parent + along * kBoneWidestAlong;
    const glm::vec3 spoke = Perpendicular(axis) * (length * kBoneWidestRadius);

    std::array<glm::vec3, kRingCorners> ring{};
    for (uint32_t corner = 0; corner < kRingCorners; ++corner)
    {
        const float angle = kQuarterTurn * static_cast<float>(corner);
        ring[corner] = ringCentre + glm::angleAxis(angle, axis) * spoke;
    }
    for (uint32_t corner = 0; corner < kRingCorners; ++corner)
    {
        AppendLine(out, parent, ring[corner], color);
        AppendLine(out, ring[corner], ring[(corner + 1) % kRingCorners], color);
        AppendLine(out, ring[corner], child, color);
    }
}

} // namespace

void AppendSkeletonLines(std::vector<LineVertex> &out, const Geometry::Skeleton &skeleton,
                         std::span<const glm::mat4> jointModel, const glm::mat4 &world, const glm::vec4 &color)
{
    ASSISI_ASSERT(jointModel.size() == skeleton.JointCount(), "one model transform per joint");
    for (std::size_t joint = 0; joint < jointModel.size(); ++joint)
    {
        const glm::vec3 position = glm::vec3(world * glm::vec4(Math::TranslationOf(jointModel[joint]), 1.f));
        const int32_t parent = skeleton.Parents[joint];
        if (parent != Geometry::kNoParent)
        {
            const glm::vec3 parentPosition =
                glm::vec3(world * glm::vec4(Math::TranslationOf(jointModel[static_cast<std::size_t>(parent)]), 1.f));
            AppendBone(out, parentPosition, position, color);
        }
        for (const glm::vec3 &axis : kCrossAxes)
        {
            AppendLine(out, position - axis * kJointMarkerHalfSize, position + axis * kJointMarkerHalfSize, color);
        }
    }
}

int32_t JointUnderCursor(const PickRay &ray, glm::vec2 cursor, std::span<const glm::mat4> jointModel,
                         const glm::mat4 &world, float &pixelsOut)
{
    int32_t nearest = Geometry::kNoJoint;
    pixelsOut = std::numeric_limits<float>::max();
    for (std::size_t joint = 0; joint < jointModel.size(); ++joint)
    {
        const glm::vec4 clip = ray.viewProjection * world * glm::vec4(Math::TranslationOf(jointModel[joint]), 1.f);
        // At or behind the eye there is no screen position; dividing through
        // would mirror the joint to the front.
        if (clip.w <= 0.f)
        {
            continue;
        }
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        const glm::vec2 screen{(ndc.x + 1.f) * 0.5f * ray.viewportSize.x, (1.f - ndc.y) * 0.5f * ray.viewportSize.y};
        const float pixels = glm::distance(screen, cursor);
        if (pixels <= kJointHoverPixels && pixels < pixelsOut)
        {
            pixelsOut = pixels;
            nearest = static_cast<int32_t>(joint);
        }
    }
    return nearest;
}

} // namespace Assisi::Editor
