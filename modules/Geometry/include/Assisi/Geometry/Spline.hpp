/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Spline.hpp
/// @brief A curve through a list of nodes, made of cubic Bezier pieces.
///
/// A component that needs a shape holds a Spline as a field, so one entity can
/// hold any number of them and each is copied and deleted with what uses it. The
/// nodes are in the holder's space: an entity carries its splines when it moves.
///
/// Each piece runs from one node to the next, pulled toward the first node's
/// out-handle and the second's in-handle. A node's mode decides which handles it
/// pulls with, so lines, flowing curves and hand-set curves are all one kind of
/// piece and one set of math.
///
/// A position along the curve is a parameter: 0 at the first node, each whole
/// number at the next node, and SegmentCount at the end.

#include <cstddef>
#include <cstdint>
#include <vector>

#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::Geometry
{

/// @brief How a node shapes the curve through it.
AENUM()
enum class SplineNodeMode : std::uint8_t
{
    /// Each handle points a third of the way to its neighbour, so the curve
    /// between two corners is a straight line and turns sharply at each.
    Corner,
    /// Handles worked out from the neighbours, so the curve flows through the
    /// node without a kink.
    Smooth,
    /// The stored handles, as set.
    Bezier,
    Count_
};

/// @brief One node of a spline.
ASTRUCT()
struct SplineNode
{
    AFIELD() glm::vec3 position{0.f};

    /// Where the curve arriving at this node is pulled from, relative to
    /// `position`. Only a Bezier node draws with it. The others keep the handle
    /// they last drew with (SettleHandles), so a node switched to Bezier starts
    /// from the curve already there.
    AFIELD(radioListen = {source = mode, value = Bezier, behavior = grey})
    glm::vec3 handleIn{0.f};

    /// Where the curve leaving this node is pulled toward, relative to
    /// `position`. Kept as handleIn is.
    AFIELD(radioListen = {source = mode, value = Bezier, behavior = grey})
    glm::vec3 handleOut{0.f};

    AFIELD(radioBroadcast) SplineNodeMode mode = SplineNodeMode::Smooth;
};

/// @brief A curve through a list of nodes.
ASTRUCT()
struct Spline
{
    AFIELD() std::vector<SplineNode> nodes;

    /// Whether one more piece joins the last node back to the first.
    AFIELD() bool closed = false;
};

/// @brief One piece of a spline: a cubic Bezier from `start` to `end`.
struct SplineSegment
{
    glm::vec3 start{0.f};
    glm::vec3 startPull{0.f}; ///< The start's out-handle, as a point.
    glm::vec3 endPull{0.f};   ///< The end's in-handle, as a point.
    glm::vec3 end{0.f};
};

/// @brief The handles a node draws with, relative to its position.
struct SplineHandles
{
    glm::vec3 in{0.f};
    glm::vec3 out{0.f};
};

/// @brief The curve as a chain of straight lines, each point with where it lies
///        on the curve.
struct SplinePolyline
{
    std::vector<glm::vec3> points;
    std::vector<float> parameters; ///< Each point's parameter.
    std::vector<float> distances;  ///< Each point's distance along the chain from the first.
};

/// @brief The point of a curve nearest some other point.
struct SplineClosest
{
    glm::vec3 position{0.f}; ///< On the curve.
    float parameter = 0.f;
    float distance = 0.f; ///< Along the chain from its start.
};

/// @brief How many pieces @p spline has: one between each pair of neighbouring
///        nodes, and one more joining the ends when it is closed. None with
///        fewer than two nodes.
[[nodiscard]] std::size_t SegmentCount(const Spline &spline);

/// @brief The handles node @p index draws with: worked out from its neighbours
///        for Corner and Smooth, stored for Bezier.
///
/// Nothing reads a Corner or Smooth node's stored handles, so code that moves a
/// node still gets a correct curve without settling anything.
[[nodiscard]] SplineHandles HandlesOf(const Spline &spline, std::size_t index);

/// @brief Piece @p index of @p spline, which must be below SegmentCount.
[[nodiscard]] SplineSegment SegmentOf(const Spline &spline, std::size_t index);

/// @brief Stores in every Corner and Smooth node the handles it draws with.
///
/// The curve is the same before and after. What it changes is what a node
/// switched to Bezier starts from: the curve on screen rather than handles
/// from whenever it was last Bezier.
void SettleHandles(Spline &spline);

/// @brief The point @p t of the way through @p segment, by its parameter.
[[nodiscard]] glm::vec3 SegmentPoint(const SplineSegment &segment, float t);

/// @brief How fast and which way @p segment moves at @p t: the derivative of
///        SegmentPoint, not of unit length.
[[nodiscard]] glm::vec3 SegmentDerivative(const SplineSegment &segment, float t);

/// @brief The point at @p parameter, clamped to the curve. A spline with one
///        node gives that node, and one with none gives the origin.
[[nodiscard]] glm::vec3 SplinePoint(const Spline &spline, float parameter);

/// @brief The unit direction of travel at @p parameter, clamped to the curve.
///
/// Where a handle has no length the derivative is zero, and the direction is
/// the one the curve leaves or arrives in. Zero only where the curve does not
/// move at all, such as two nodes in one place.
[[nodiscard]] glm::vec3 SplineDirection(const Spline &spline, float parameter);

/// @brief The curve as a chain of lines no further than @p tolerance from it.
///
/// Each piece is split in half until its handles lie within the tolerance of
/// the line between its ends, so straight runs take one line and tight bends
/// many. A spline with one node gives that one point, and one with none gives
/// nothing.
[[nodiscard]] SplinePolyline FlattenSpline(const Spline &spline, float tolerance);

/// @brief The chain's length, which is the curve's to within the tolerance it
///        was flattened at.
[[nodiscard]] float PolylineLength(const SplinePolyline &polyline);

/// @brief The parameter @p distance along the chain, clamped to it.
///
/// What moves something along a curve at a steady speed: equal steps of
/// distance are equal steps along the curve, whatever its parameters do.
[[nodiscard]] float ParameterAtDistance(const SplinePolyline &polyline, float distance);

/// @brief The point of @p spline nearest @p point, found on @p polyline, which
///        must have been flattened from it.
///
/// As exact as the chain is: within its tolerance of the true nearest point,
/// and always on the curve.
[[nodiscard]] SplineClosest ClosestOnSpline(const Spline &spline, const SplinePolyline &polyline,
                                            const glm::vec3 &point);

} // namespace Assisi::Geometry
