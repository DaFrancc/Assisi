/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Editor/SkeletonOverlay.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Geometry/Pose.hpp>
#include <Assisi/Math/Matrix.hpp>

#include <array>
#include <cstddef>
#include <limits>

namespace Assisi::Editor
{

namespace
{

/// The three axes a joint's cross is drawn along.
constexpr std::array<glm::vec3, 3> kCrossAxes{glm::vec3(1.f, 0.f, 0.f), glm::vec3(0.f, 1.f, 0.f),
                                              glm::vec3(0.f, 0.f, 1.f)};

void AppendLine(std::vector<LineVertex> &out, const glm::vec3 &a, const glm::vec3 &b, const glm::vec4 &color)
{
    out.push_back(LineVertex{.position = a, .color = color});
    out.push_back(LineVertex{.position = b, .color = color});
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
            AppendLine(out, parentPosition, position, color);
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
