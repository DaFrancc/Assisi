/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Geometry/Spline.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace Assisi::Geometry
{
namespace
{

/// How far a computed handle reaches toward the node it points at, as a
/// fraction of the distance. A third is what makes the piece between two Corner
/// nodes a straight line whose parameter is the fraction of its length.
constexpr float kHandleReach = 1.f / 3.f;

/// Shorter than this, a length is two points in one place, with no direction
/// between them.
constexpr float kMinLength = 1e-6f;

/// How many times flattening may halve one piece: 4096 lines at most, which
/// only a tolerance far finer than the piece itself could ask for.
constexpr std::int32_t kMaxFlattenDepth = 12;

/// Below this a piece's parameter counts as its first half, for working out
/// which way a curve with a zero-length handle leaves or arrives.
constexpr float kHalfway = 0.5f;

struct Neighbours
{
    const SplineNode *previous = nullptr;
    const SplineNode *next = nullptr;
};

Neighbours NeighboursOf(const Spline &spline, std::size_t index)
{
    const std::size_t count = spline.nodes.size();
    const bool wraps = spline.closed && count > 1;
    Neighbours around;
    if (index > 0)
    {
        around.previous = &spline.nodes[index - 1];
    }
    else if (wraps)
    {
        around.previous = &spline.nodes[count - 1];
    }
    if (index + 1 < count)
    {
        around.next = &spline.nodes[index + 1];
    }
    else if (wraps)
    {
        around.next = &spline.nodes[0];
    }
    return around;
}

/// A handle from @p from reaching kHandleReach of the way to @p toward.
glm::vec3 Toward(const glm::vec3 &from, const glm::vec3 &toward)
{
    return (toward - from) * kHandleReach;
}

/// A Corner's handles: each toward its neighbour. An open end has nothing on
/// one side, and mirrors the other handle there, so the handle it keeps is the
/// one it would draw with if the spline went on past it.
SplineHandles CornerHandles(const glm::vec3 &position, const Neighbours &around)
{
    SplineHandles handles;
    if (around.previous != nullptr)
    {
        handles.in = Toward(position, around.previous->position);
    }
    if (around.next != nullptr)
    {
        handles.out = Toward(position, around.next->position);
    }
    if (around.previous == nullptr)
    {
        handles.in = -handles.out;
    }
    if (around.next == nullptr)
    {
        handles.out = -handles.in;
    }
    return handles;
}

/// A Smooth node's handles between two neighbours.
///
/// They lie along the direction halfway between the way the curve arrives and
/// the way it leaves, so it does not kink. Each reaches toward its own
/// neighbour by that neighbour's distance, so a short side gets a short handle
/// and unevenly spaced nodes do not make the curve overshoot.
SplineHandles SmoothBetween(const glm::vec3 &previous, const glm::vec3 &position, const glm::vec3 &next)
{
    const glm::vec3 arriving = position - previous;
    const glm::vec3 leaving = next - position;
    const float arrivingLength = glm::length(arriving);
    const float leavingLength = glm::length(leaving);
    if (arrivingLength < kMinLength || leavingLength < kMinLength)
    {
        // A neighbour in the same place gives no way to flow, so the node turns
        // as a corner does.
        return SplineHandles{.in = Toward(position, previous), .out = Toward(position, next)};
    }

    glm::vec3 direction = arriving / arrivingLength + leaving / leavingLength;
    const float directionLength = glm::length(direction);
    // A curve that doubles straight back has no halfway; it leaves the way it
    // goes.
    direction = directionLength < kMinLength ? leaving / leavingLength : direction / directionLength;
    return SplineHandles{.in = -direction * (arrivingLength * kHandleReach),
                         .out = direction * (leavingLength * kHandleReach)};
}

/// The handles node @p index draws with, judged from its own neighbours alone.
/// This is HandlesOf for every node but a Smooth open end, which also looks one
/// node further.
SplineHandles InnerHandles(const Spline &spline, std::size_t index)
{
    const SplineNode &node = spline.nodes[index];
    if (node.mode == SplineNodeMode::Bezier)
    {
        return SplineHandles{.in = node.handleIn, .out = node.handleOut};
    }
    const Neighbours around = NeighboursOf(spline, index);
    if (node.mode == SplineNodeMode::Smooth && around.previous != nullptr && around.next != nullptr)
    {
        return SmoothBetween(around.previous->position, node.position, around.next->position);
    }
    return CornerHandles(node.position, around);
}

/// A handle from @p position aimed at @p pull, reaching kHandleReach of the way
/// to @p neighbour.
glm::vec3 AimedAt(const glm::vec3 &position, const glm::vec3 &neighbour, const glm::vec3 &pull)
{
    const glm::vec3 aim = pull - position;
    const float aimLength = glm::length(aim);
    if (aimLength < kMinLength)
    {
        return Toward(position, neighbour);
    }
    return aim * (glm::length(neighbour - position) * kHandleReach / aimLength);
}

/// The distance from @p point to the line from @p a to @p b, ends included.
float DistanceToLine(const glm::vec3 &point, const glm::vec3 &a, const glm::vec3 &b)
{
    const glm::vec3 along = b - a;
    const float lengthSquared = glm::dot(along, along);
    const float t = lengthSquared > 0.f ? std::clamp(glm::dot(point - a, along) / lengthSquared, 0.f, 1.f) : 0.f;
    return glm::length(point - (a + along * t));
}

/// Which piece @p parameter falls in and how far through it, clamped to the
/// curve. @p count is at least one.
struct Location
{
    std::size_t segment = 0;
    float t = 0.f;
};

Location Locate(std::size_t count, float parameter)
{
    const float last = static_cast<float>(count);
    const float clamped = std::isfinite(parameter) ? std::clamp(parameter, 0.f, last) : 0.f;
    const std::size_t segment = std::min(static_cast<std::size_t>(clamped), count - 1);
    return Location{.segment = segment, .t = clamped - static_cast<float>(segment)};
}

/// The direction a piece leaves or arrives in where its derivative is zero,
/// which is where a handle has no length: toward the next control point that
/// is somewhere else.
glm::vec3 LimitingDirection(const SplineSegment &segment, float t)
{
    if (t < kHalfway)
    {
        const glm::vec3 towardPull = segment.endPull - segment.start;
        return glm::length(towardPull) >= kMinLength ? towardPull : segment.end - segment.start;
    }
    const glm::vec3 fromPull = segment.end - segment.startPull;
    return glm::length(fromPull) >= kMinLength ? fromPull : segment.end - segment.start;
}

/// Halves of a piece, by de Casteljau at its middle. Each half is a cubic of its
/// own whose parameter runs over half of the original's.
struct Halves
{
    SplineSegment first;
    SplineSegment second;
};

Halves Split(const SplineSegment &piece)
{
    const glm::vec3 ab = glm::mix(piece.start, piece.startPull, kHalfway);
    const glm::vec3 bc = glm::mix(piece.startPull, piece.endPull, kHalfway);
    const glm::vec3 cd = glm::mix(piece.endPull, piece.end, kHalfway);
    const glm::vec3 abc = glm::mix(ab, bc, kHalfway);
    const glm::vec3 bcd = glm::mix(bc, cd, kHalfway);
    const glm::vec3 middle = glm::mix(abc, bcd, kHalfway);
    return Halves{.first = SplineSegment{.start = piece.start, .startPull = ab, .endPull = abc, .end = middle},
                  .second = SplineSegment{.start = middle, .startPull = bcd, .endPull = cd, .end = piece.end}};
}

/// Builds a chain one piece at a time, appending each point after the first.
class Flattener
{
public:
    Flattener(SplinePolyline &chain, float tolerance) : _chain(chain), _tolerance(tolerance) {}

    /// Appends @p piece, which covers parameters @p from to @p to, splitting it
    /// until each line is within the tolerance.
    ///
    /// Within the tolerance because the curve lies inside the hull of its four
    /// control points: when both handles are that close to the line between the
    /// ends, so is every point of the curve.
    void Append(const SplineSegment &piece, float from, float to, std::int32_t depth)
    {
        const bool flat = DistanceToLine(piece.startPull, piece.start, piece.end) <= _tolerance &&
                          DistanceToLine(piece.endPull, piece.start, piece.end) <= _tolerance;
        if (flat || depth >= kMaxFlattenDepth)
        {
            AppendPoint(piece.end, to);
            return;
        }
        const Halves halves = Split(piece);
        const float middle = (from + to) * kHalfway;
        Append(halves.first, from, middle, depth + 1);
        Append(halves.second, middle, to, depth + 1);
    }

private:
    void AppendPoint(const glm::vec3 &point, float parameter)
    {
        _chain.distances.push_back(_chain.distances.back() + glm::length(point - _chain.points.back()));
        _chain.points.push_back(point);
        _chain.parameters.push_back(parameter);
    }

    SplinePolyline &_chain;
    float _tolerance;
};

} // namespace

std::size_t SegmentCount(const Spline &spline)
{
    const std::size_t count = spline.nodes.size();
    if (count < 2)
    {
        return 0;
    }
    return spline.closed ? count : count - 1;
}

SplineHandles HandlesOf(const Spline &spline, std::size_t index)
{
    const SplineNode &node = spline.nodes[index];
    const Neighbours around = NeighboursOf(spline, index);
    const bool openEnd = (around.previous == nullptr) != (around.next == nullptr);
    if (node.mode != SplineNodeMode::Smooth || !openEnd)
    {
        return InnerHandles(spline, index);
    }

    // A Smooth open end flows into the curve past its neighbour: it aims at the
    // point the neighbour pulls the shared piece toward, rather than at the
    // neighbour itself, which would leave the end heading the wrong way into
    // every bend after it. The neighbour's own handles never depend on this
    // node's, so there is nothing circular in asking for them.
    if (around.next != nullptr)
    {
        const glm::vec3 pull = around.next->position + InnerHandles(spline, index + 1).in;
        const glm::vec3 out = AimedAt(node.position, around.next->position, pull);
        return SplineHandles{.in = -out, .out = out};
    }
    const glm::vec3 pull = around.previous->position + InnerHandles(spline, index - 1).out;
    const glm::vec3 in = AimedAt(node.position, around.previous->position, pull);
    return SplineHandles{.in = in, .out = -in};
}

SplineSegment SegmentOf(const Spline &spline, std::size_t index)
{
    const std::size_t endIndex = (index + 1) % spline.nodes.size();
    const glm::vec3 &start = spline.nodes[index].position;
    const glm::vec3 &end = spline.nodes[endIndex].position;
    return SplineSegment{.start = start,
                         .startPull = start + HandlesOf(spline, index).out,
                         .endPull = end + HandlesOf(spline, endIndex).in,
                         .end = end};
}

void SettleHandles(Spline &spline)
{
    // Every node's handles worked out before any is written. None of them reads
    // a Corner or Smooth node's stored handles today, but writing in place would
    // make that true only by luck of the order.
    std::vector<SplineHandles> settled;
    settled.reserve(spline.nodes.size());
    for (std::size_t i = 0; i < spline.nodes.size(); ++i)
    {
        settled.push_back(HandlesOf(spline, i));
    }
    for (std::size_t i = 0; i < spline.nodes.size(); ++i)
    {
        SplineNode &node = spline.nodes[i];
        if (node.mode != SplineNodeMode::Bezier)
        {
            node.handleIn = settled[i].in;
            node.handleOut = settled[i].out;
        }
    }
}

glm::vec3 SegmentPoint(const SplineSegment &segment, float t)
{
    const float u = 1.f - t;
    return segment.start * (u * u * u) + segment.startPull * (3.f * u * u * t) + segment.endPull * (3.f * u * t * t) +
           segment.end * (t * t * t);
}

glm::vec3 SegmentDerivative(const SplineSegment &segment, float t)
{
    const float u = 1.f - t;
    return (segment.startPull - segment.start) * (3.f * u * u) + (segment.endPull - segment.startPull) * (6.f * u * t) +
           (segment.end - segment.endPull) * (3.f * t * t);
}

glm::vec3 SplinePoint(const Spline &spline, float parameter)
{
    const std::size_t count = SegmentCount(spline);
    if (count == 0)
    {
        return spline.nodes.empty() ? glm::vec3(0.f) : spline.nodes[0].position;
    }
    const Location at = Locate(count, parameter);
    return SegmentPoint(SegmentOf(spline, at.segment), at.t);
}

glm::vec3 SplineDirection(const Spline &spline, float parameter)
{
    const std::size_t count = SegmentCount(spline);
    if (count == 0)
    {
        return glm::vec3(0.f);
    }
    const Location at = Locate(count, parameter);
    const SplineSegment segment = SegmentOf(spline, at.segment);
    glm::vec3 direction = SegmentDerivative(segment, at.t);
    if (glm::length(direction) < kMinLength)
    {
        direction = LimitingDirection(segment, at.t);
    }
    const float length = glm::length(direction);
    return length < kMinLength ? glm::vec3(0.f) : direction / length;
}

SplinePolyline FlattenSpline(const Spline &spline, float tolerance)
{
    SplinePolyline chain;
    if (spline.nodes.empty())
    {
        return chain;
    }
    chain.points.push_back(spline.nodes[0].position);
    chain.parameters.push_back(0.f);
    chain.distances.push_back(0.f);

    Flattener flattener{chain, tolerance};
    const std::size_t count = SegmentCount(spline);
    for (std::size_t i = 0; i < count; ++i)
    {
        const float from = static_cast<float>(i);
        flattener.Append(SegmentOf(spline, i), from, from + 1.f, 0);
    }
    return chain;
}

float PolylineLength(const SplinePolyline &polyline)
{
    return polyline.distances.empty() ? 0.f : polyline.distances.back();
}

float ParameterAtDistance(const SplinePolyline &polyline, float distance)
{
    if (polyline.points.empty())
    {
        return 0.f;
    }
    if (!std::isfinite(distance) || distance <= 0.f || polyline.points.size() == 1)
    {
        return polyline.parameters.front();
    }
    if (distance >= polyline.distances.back())
    {
        return polyline.parameters.back();
    }

    const std::vector<float>::const_iterator above =
        std::upper_bound(polyline.distances.begin(), polyline.distances.end(), distance);
    const std::size_t high = static_cast<std::size_t>(above - polyline.distances.begin());
    const std::size_t low = high - 1;
    const float span = polyline.distances[high] - polyline.distances[low];
    const float f = span > 0.f ? (distance - polyline.distances[low]) / span : 0.f;
    return glm::mix(polyline.parameters[low], polyline.parameters[high], f);
}

SplineClosest ClosestOnSpline(const Spline &spline, const SplinePolyline &polyline, const glm::vec3 &point)
{
    SplineClosest closest;
    if (polyline.points.empty())
    {
        return closest;
    }

    float nearestSquared = std::numeric_limits<float>::max();
    if (polyline.points.size() == 1)
    {
        closest.parameter = polyline.parameters[0];
    }
    for (std::size_t i = 1; i < polyline.points.size(); ++i)
    {
        const glm::vec3 &a = polyline.points[i - 1];
        const glm::vec3 along = polyline.points[i] - a;
        const float lengthSquared = glm::dot(along, along);
        const float f = lengthSquared > 0.f ? std::clamp(glm::dot(point - a, along) / lengthSquared, 0.f, 1.f) : 0.f;
        const glm::vec3 offset = point - (a + along * f);
        const float distanceSquared = glm::dot(offset, offset);
        if (distanceSquared < nearestSquared)
        {
            nearestSquared = distanceSquared;
            closest.parameter = glm::mix(polyline.parameters[i - 1], polyline.parameters[i], f);
            closest.distance = glm::mix(polyline.distances[i - 1], polyline.distances[i], f);
        }
    }
    closest.position = SplinePoint(spline, closest.parameter);
    return closest;
}

} // namespace Assisi::Geometry
