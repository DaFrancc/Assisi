/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/SplineEdit.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Assisi::Editor
{
namespace
{

/// Clip-space w at or below this is the eye plane and behind it, where a point
/// has no screen position.
constexpr float kMinClipW = 1e-4f;

/// Shorter than this, a handle or direction has no length to keep.
constexpr float kMinLength = 1e-6f;

/// A point as drawn: where on screen, and its clip-space w, which is what
/// undoes perspective along a line between two of them.
struct Projected
{
    glm::vec2 screen{0.f};
    float w = 0.f;
};

/// Where @p point lands in framebuffer pixels, or nothing when it is at or
/// behind the eye. The same rule ScreenDistanceToSegment draws by: Y counts
/// down from the top, as the cursor does.
std::optional<Projected> Project(const PickRay &ray, const glm::vec3 &point)
{
    const glm::vec4 clip = ray.viewProjection * glm::vec4(point, 1.f);
    if (clip.w < kMinClipW)
    {
        return std::nullopt;
    }
    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
    return Projected{.screen = glm::vec2((ndc.x * 0.5f + 0.5f) * ray.viewportSize.x,
                                         (0.5f - ndc.y * 0.5f) * ray.viewportSize.y),
                     .w = clip.w};
}

glm::vec3 Placed(const glm::mat4 &model, const glm::vec3 &local)
{
    return glm::vec3(model * glm::vec4(local, 1.f));
}

/// @p partner turned to point opposite @p moved, keeping its own length, so a
/// node's two handles stay in one line. Left alone when @p moved has no
/// direction to oppose.
glm::vec3 Opposite(const glm::vec3 &partner, const glm::vec3 &moved)
{
    const float movedLength = glm::length(moved);
    if (movedLength < kMinLength)
    {
        return partner;
    }
    return moved * (-glm::length(partner) / movedLength);
}

/// The screen distance from @p cursor to @p point's marker, when it is in front
/// of the eye.
std::optional<float> PixelsTo(const PickRay &ray, glm::vec2 cursor, const glm::vec3 &point)
{
    const std::optional<Projected> projected = Project(ray, point);
    if (!projected)
    {
        return std::nullopt;
    }
    return glm::length(cursor - projected->screen);
}

/// Whether @p ray passes through the cube of half side @p halfExtent about
/// @p point, in the overlay's space.
bool RayThroughMarker(const PickRay &ray, const glm::vec3 &point, float halfExtent)
{
    const Geometry::Aabb cube{.min = glm::vec3(-halfExtent), .max = glm::vec3(halfExtent)};
    float t = 0.f;
    return RayAabbIntersect(ray.origin, ray.direction, cube, glm::translate(glm::mat4(1.f), point), t);
}

} // namespace

SplineMarker MarkerOf(const Geometry::Spline &spline, SplinePart part)
{
    if (part.kind != SplinePartKind::Node)
    {
        return SplineMarker::Handle;
    }
    if (part.node == 0)
    {
        return SplineMarker::Start;
    }
    // A closed spline runs on through its last node back to the first, so its
    // last node is one more on the way.
    if (!spline.closed && part.node + 1 == spline.nodes.size())
    {
        return SplineMarker::End;
    }
    return SplineMarker::Middle;
}

float MarkerHalfExtent(SplineMarker marker)
{
    switch (marker)
    {
    case SplineMarker::Start:
    case SplineMarker::End:
        // The disc reaches further across than the arrowhead does along.
        return std::max(kEndDiscRadius, kArrowheadHalfLength);
    case SplineMarker::Middle:
        return kArrowheadHalfLength;
    case SplineMarker::Handle:
    case SplineMarker::Count_:
        break;
    }
    return kHandleHalfExtent;
}

glm::vec3 PartPosition(const Geometry::Spline &spline, SplinePart part)
{
    const Geometry::SplineNode &node = spline.nodes[part.node];
    switch (part.kind)
    {
    case SplinePartKind::HandleIn:
        return node.position + node.handleIn;
    case SplinePartKind::HandleOut:
        return node.position + node.handleOut;
    case SplinePartKind::Node:
    case SplinePartKind::Count_:
        break;
    }
    return node.position;
}

void MovePart(Geometry::Spline &spline, SplinePart part, const glm::vec3 &position)
{
    Geometry::SplineNode &node = spline.nodes[part.node];
    switch (part.kind)
    {
    case SplinePartKind::HandleIn:
        node.handleIn = position - node.position;
        node.handleOut = Opposite(node.handleOut, node.handleIn);
        break;
    case SplinePartKind::HandleOut:
        node.handleOut = position - node.position;
        node.handleIn = Opposite(node.handleIn, node.handleOut);
        break;
    case SplinePartKind::Node:
    case SplinePartKind::Count_:
        node.position = position;
        break;
    }
    Geometry::SettleHandles(spline);
}

std::optional<SplinePartHit> PickSplinePart(const PickRay &ray, glm::vec2 cursor, const glm::mat4 &model,
                                            const Geometry::Spline &spline)
{
    if (!ray.valid)
    {
        return std::nullopt;
    }

    std::optional<SplinePartHit> nearest;
    float nearestPixels = std::numeric_limits<float>::max();
    const auto consider = [&](SplinePart part)
                          {
                              const glm::vec3 point = Placed(model, PartPosition(spline, part));
                              const std::optional<float> pixels = PixelsTo(ray, cursor, point);
                              const bool grabbed =
                                  pixels && (*pixels <= kSplinePartPickPixels ||
                                             RayThroughMarker(ray, point, MarkerHalfExtent(MarkerOf(spline, part))));
                              if (grabbed && *pixels < nearestPixels)
                              {
                                  nearestPixels = *pixels;
                                  nearest = SplinePartHit{.part = part, .pixels = *pixels};
                              }
                          };
    for (std::size_t i = 0; i < spline.nodes.size(); ++i)
    {
        consider(SplinePart{.node = i, .kind = SplinePartKind::Node});
        if (spline.nodes[i].mode == Geometry::SplineNodeMode::Bezier)
        {
            consider(SplinePart{.node = i, .kind = SplinePartKind::HandleIn});
            consider(SplinePart{.node = i, .kind = SplinePartKind::HandleOut});
        }
    }
    return nearest;
}

std::optional<SplineCurveHit> PickSplineCurve(const PickRay &ray, glm::vec2 cursor, const glm::mat4 &model,
                                              const Geometry::Spline &spline)
{
    if (!ray.valid)
    {
        return std::nullopt;
    }

    const Geometry::SplinePolyline chain = Geometry::FlattenSpline(spline, kSplineDrawTolerance);
    std::optional<SplineCurveHit> nearest;
    float nearestDistance = std::numeric_limits<float>::max();
    for (std::size_t i = 1; i < chain.points.size(); ++i)
    {
        const glm::vec3 a = Placed(model, chain.points[i - 1]);
        const glm::vec3 b = Placed(model, chain.points[i]);
        const std::optional<Projected> pa = Project(ray, a);
        const std::optional<Projected> pb = Project(ray, b);
        // A stretch crossing the eye plane is passed over: it runs beside the
        // camera, where nobody is aiming at it.
        if (!pa || !pb)
        {
            continue;
        }

        const glm::vec2 span = pb->screen - pa->screen;
        const float spanSquared = glm::dot(span, span);
        const float onScreen =
            spanSquared > 0.f ? std::clamp(glm::dot(cursor - pa->screen, span) / spanSquared, 0.f, 1.f) : 0.f;
        if (glm::length(cursor - (pa->screen + span * onScreen)) > kOutlinePickPixels)
        {
            continue;
        }

        // A fraction of the way across the screen is a different fraction of the
        // way along the line: weighting by each end's w undoes the divide.
        const float denominator = (1.f - onScreen) * pb->w + onScreen * pa->w;
        const float along = denominator > 0.f ? onScreen * pa->w / denominator : onScreen;
        const float distance = glm::length(glm::mix(a, b, along) - ray.origin);
        if (distance < nearestDistance)
        {
            nearestDistance = distance;
            nearest = SplineCurveHit{.parameter = glm::mix(chain.parameters[i - 1], chain.parameters[i], along),
                                     .distance = distance};
        }
    }
    return nearest;
}

std::size_t InsertSplineNode(Geometry::Spline &spline, float parameter)
{
    const std::size_t count = Geometry::SegmentCount(spline);
    if (count == 0)
    {
        return ExtendSpline(spline, /*atStart=*/ false, Geometry::SplinePoint(spline, parameter));
    }

    const float clamped = std::isfinite(parameter) ? std::clamp(parameter, 0.f, static_cast<float>(count)) : 0.f;
    const std::size_t segment = std::min(static_cast<std::size_t>(clamped), count - 1);
    const float t = clamped - static_cast<float>(segment);
    const Geometry::SplineSegment piece = Geometry::SegmentOf(spline, segment);
    Geometry::SplineNode &start = spline.nodes[segment];
    Geometry::SplineNode &end = spline.nodes[(segment + 1) % spline.nodes.size()];

    Geometry::SplineNode added{.position = Geometry::SegmentPoint(piece, t), .mode = Geometry::SplineNodeMode::Smooth};
    if (start.mode == Geometry::SplineNodeMode::Bezier && end.mode == Geometry::SplineNodeMode::Bezier)
    {
        // The piece split where it is cut, by de Casteljau: the two halves are
        // exactly the curve that was there, so nothing moves.
        const glm::vec3 ab = glm::mix(piece.start, piece.startPull, t);
        const glm::vec3 bc = glm::mix(piece.startPull, piece.endPull, t);
        const glm::vec3 cd = glm::mix(piece.endPull, piece.end, t);
        const glm::vec3 abc = glm::mix(ab, bc, t);
        const glm::vec3 bcd = glm::mix(bc, cd, t);
        start.handleOut = ab - piece.start;
        end.handleIn = cd - piece.end;
        added.handleIn = abc - added.position;
        added.handleOut = bcd - added.position;
        added.mode = Geometry::SplineNodeMode::Bezier;
    }

    // After the last node for a closed spline's joining piece, which is where
    // that piece starts.
    const std::size_t index = segment + 1;
    spline.nodes.insert(spline.nodes.begin() + static_cast<std::ptrdiff_t>(index), added);
    Geometry::SettleHandles(spline);
    return index;
}

std::size_t ExtendSpline(Geometry::Spline &spline, bool atStart, const glm::vec3 &position)
{
    Geometry::SplineNodeMode mode = Geometry::SplineNodeMode::Smooth;
    if (!spline.nodes.empty())
    {
        mode = atStart ? spline.nodes.front().mode : spline.nodes.back().mode;
    }

    const std::size_t index = atStart ? 0 : spline.nodes.size();
    spline.nodes.insert(spline.nodes.begin() + static_cast<std::ptrdiff_t>(index),
                        Geometry::SplineNode{.position = position, .mode = Geometry::SplineNodeMode::Smooth});

    // Placed as Smooth first, so a Bezier node starts from the handles that
    // flow on from the curve rather than from none.
    if (mode == Geometry::SplineNodeMode::Bezier)
    {
        const Geometry::SplineHandles flowing = Geometry::HandlesOf(spline, index);
        spline.nodes[index].handleIn = flowing.in;
        spline.nodes[index].handleOut = flowing.out;
    }
    spline.nodes[index].mode = mode;
    Geometry::SettleHandles(spline);
    return index;
}

void RemoveSplineNode(Geometry::Spline &spline, std::size_t index)
{
    spline.nodes.erase(spline.nodes.begin() + static_cast<std::ptrdiff_t>(index));
    Geometry::SettleHandles(spline);
}

std::optional<glm::vec3> RayOnLevel(const glm::vec3 &origin, const glm::vec3 &direction, float height)
{
    if (std::abs(direction.y) < kMinLength)
    {
        return std::nullopt;
    }
    const float t = (height - origin.y) / direction.y;
    if (t <= 0.f)
    {
        return std::nullopt;
    }
    return origin + direction * t;
}

} // namespace Assisi::Editor
