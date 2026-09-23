/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file EditorCameraPaths.cpp
/// @brief The benchmark's camera route, drawn where it will fly.
///
/// The route is traced through Runtime::EvaluateCameraLeg, the same function
/// the benchmark flies it with, so the drawing cannot disagree with the flight.
///
/// Each path shows its course, arrows for which way it travels, a box where it
/// starts, and a sight line to what it looks at. A dashed line joins one path's
/// end to the next one's start: the camera cuts across that gap, and the dashes
/// show the order the paths play in.

#include <Assisi/Editor/EditorApp.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <Assisi/Editor/Overlay/LinePass.hpp>
#include <Assisi/Editor/Overlay/OverlayRenderer.hpp>
#include <Assisi/Editor/WireShapes.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Runtime/CameraPath.hpp>

namespace Assisi::Editor
{
namespace
{
namespace Rt = Assisi::Runtime;

/// Cyan, so a route is never mistaken for the orange of a selection.
constexpr glm::vec4 kPathColor{0.2f, 0.85f, 1.0f, 0.9f};
constexpr glm::vec4 kSelectedColor{kSelectionOutline, 1.0f};
constexpr glm::vec4 kActiveSelectedColor{kActiveSelectionOutline, 1.0f};

/// Sight lines and the jumps between paths are context, not the course, so they
/// draw fainter than it.
constexpr float kContextAlpha = 0.35f;

/// Where along a path its direction arrows sit, as fractions of its progress.
constexpr float kArrowProgress[] = {0.25f, 0.5f, 0.75f, 1.0f};

/// Progress step used to read the direction of travel at an arrow.
constexpr float kDirectionStep = 1e-3f;

/// An arrow's length as a fraction of its path's length, clamped to a range
/// that stays readable on a short dolly and a wide orbit alike.
constexpr float kArrowFraction = 0.06f;
constexpr float kMinArrowLength = 0.3f;
constexpr float kMaxArrowLength = 2.0f;

/// Half the side of the box marking where a path starts, and of the one marking
/// what it looks at.
constexpr float kMarkerHalfExtent = 0.2f;

/// Length of each dash, and of each gap, on the line joining two paths.
constexpr float kDashLength = 0.5f;

/// Below this a direction or a gap is too short to draw.
constexpr float kMinDrawLength = 1e-4f;

/// Segments in a straight path's polyline. A line needs only one.
constexpr std::int32_t kLineSegments = 1;

/// Degrees in one full turn, for converting a sweep into segments.
constexpr float kDegreesPerTurn = 360.f;

[[nodiscard]] glm::vec4 Faded(const glm::vec4 &color)
{
    return glm::vec4(glm::vec3(color), color.a * kContextAlpha);
}

[[nodiscard]] std::int32_t SegmentsFor(const Rt::CameraRouteLeg &leg)
{
    if (!leg.isCircle)
    {
        return kLineSegments;
    }
    const float turns = std::abs(leg.circle.sweepDegrees) / kDegreesPerTurn;
    return std::max(1, static_cast<std::int32_t>(std::ceil(turns * static_cast<float>(kCircleSegments))));
}

void AddDashedSegment(std::vector<LineVertex> &out, const glm::vec4 &color, const glm::vec3 &a, const glm::vec3 &b)
{
    const float length = glm::length(b - a);
    if (length < kMinDrawLength)
    {
        return;
    }
    const glm::vec3 direction = (b - a) / length;
    const glm::mat4 identity(1.f);
    for (float start = 0.f; start < length; start += 2.f * kDashLength)
    {
        const float end = std::min(start + kDashLength, length);
        AddSegment(out, identity, color, a + direction * start, a + direction * end);
    }
}

void AddLeg(std::vector<LineVertex> &out, const glm::vec4 &color, const Rt::CameraRouteLeg &leg)
{
    const glm::mat4 identity(1.f);

    // The course, as a polyline through the evaluator.
    const std::int32_t segments = SegmentsFor(leg);
    float pathLength = 0.f;
    glm::vec3 previous = Rt::EvaluateCameraLeg(leg, 0.f).eye;
    for (std::int32_t i = 1; i <= segments; ++i)
    {
        const glm::vec3 point =
            Rt::EvaluateCameraLeg(leg, static_cast<float>(i) / static_cast<float>(segments)).eye;
        AddSegment(out, identity, color, previous, point);
        pathLength += glm::length(point - previous);
        previous = point;
    }

    // Direction arrows, each laid along the course where it sits.
    const float arrowLength = std::clamp(pathLength * kArrowFraction, kMinArrowLength, kMaxArrowLength);
    for (const float progress : kArrowProgress)
    {
        const glm::vec3 behind = Rt::EvaluateCameraLeg(leg, progress - kDirectionStep).eye;
        const glm::vec3 tip = Rt::EvaluateCameraLeg(leg, progress).eye;
        const glm::vec3 travel = tip - behind;
        if (glm::length(travel) < kMinDrawLength)
        {
            continue;
        }
        // The arrow runs from its origin along -Y, so its origin goes one length
        // behind the tip for the head to land on the course.
        const glm::vec3 direction = glm::normalize(travel);
        const glm::mat4 model = glm::translate(glm::mat4(1.f), tip - direction * arrowLength) * AimAlong(direction);
        AddArrowWireframe(out, model, color, arrowLength);
    }

    // Where it starts, and what it looks at from there.
    const Rt::CameraAim start = Rt::EvaluateCameraLeg(leg, 0.f);
    AddBoxWireframe(out, glm::translate(glm::mat4(1.f), start.eye), color, glm::vec3(kMarkerHalfExtent));

    const bool looksAtPoint = leg.isCircle || !leg.line.lookAlongPath;
    if (looksAtPoint)
    {
        const glm::vec3 target = leg.isCircle ? leg.circle.lookAt : leg.line.lookAt;
        AddBoxWireframe(out, glm::translate(glm::mat4(1.f), target), Faded(color), glm::vec3(kMarkerHalfExtent));
        AddSegment(out, identity, Faded(color), start.eye, target);
    }
}
} // namespace

void EditorApp::SubmitCameraPathGizmos()
{
    if (_scene == nullptr)
    {
        return;
    }

    _cameraPathLines.clear();

    const std::vector<Rt::CameraRouteLeg> route = Rt::BuildCameraRoute(*_scene);
    for (std::size_t i = 0; i < route.size(); ++i)
    {
        const Rt::CameraRouteLeg &leg = route[i];
        glm::vec4 color = kPathColor;
        if (IsSelected(leg.entity))
        {
            color = leg.entity == _selectedEntity ? kActiveSelectedColor : kSelectedColor;
        }

        if (i > 0)
        {
            AddDashedSegment(_cameraPathLines, Faded(kPathColor), Rt::EvaluateCameraLeg(route[i - 1], 1.f).eye,
                             Rt::EvaluateCameraLeg(leg, 0.f).eye);
        }
        AddLeg(_cameraPathLines, color, leg);
    }

    // On top: a fly-through passes inside the geometry it flies through, and the
    // part a wall hides is the part being placed.
    _overlays.SubmitOverlayLines(_cameraPathLines, /*onTop=*/ true);
}

} // namespace Assisi::Editor
