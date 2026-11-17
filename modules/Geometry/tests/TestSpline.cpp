/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include <Assisi/Geometry/Spline.hpp>
#include <Assisi/Math/GLM.hpp>

using Assisi::Geometry::ClosestOnSpline;
using Assisi::Geometry::FlattenSpline;
using Assisi::Geometry::ParameterAtDistance;
using Assisi::Geometry::PolylineLength;
using Assisi::Geometry::SegmentCount;
using Assisi::Geometry::SegmentDerivative;
using Assisi::Geometry::SegmentOf;
using Assisi::Geometry::SegmentPoint;
using Assisi::Geometry::SettleHandles;
using Assisi::Geometry::Spline;
using Assisi::Geometry::SplineClosest;
using Assisi::Geometry::SplineDirection;
using Assisi::Geometry::SplineNode;
using Assisi::Geometry::SplineNodeMode;
using Assisi::Geometry::SplinePoint;
using Assisi::Geometry::SplinePolyline;

namespace
{

/// Room for float rounding in positions of a few metres.
constexpr float kPositionTolerance = 1e-4f;

/// Two unit directions this close in dot product point the same way.
constexpr float kSameDirection = 0.9999f;

/// How far a flattened chain may stray from the curve in these tests.
constexpr float kFlattenTolerance = 0.01f;

/// Points sampled along each line of a chain when checking it against the curve.
constexpr std::int32_t kSamplesPerLine = 8;

Spline Through(std::initializer_list<glm::vec3> positions, SplineNodeMode mode, bool closed = false)
{
    Spline spline;
    spline.closed = closed;
    for (const glm::vec3 &position : positions)
    {
        spline.nodes.push_back(SplineNode{.position = position, .mode = mode});
    }
    return spline;
}

/// A zigzag across the ground with uneven spacing, so a rule that only works on
/// evenly spaced nodes shows.
Spline Zigzag(SplineNodeMode mode)
{
    return Through({{0.f, 0.f, 0.f}, {3.f, 0.f, 2.f}, {4.f, 0.f, -1.f}, {9.f, 0.f, 1.f}}, mode);
}

float DistanceToLine(const glm::vec3 &point, const glm::vec3 &a, const glm::vec3 &b)
{
    const glm::vec3 along = b - a;
    const float t = glm::clamp(glm::dot(point - a, along) / glm::dot(along, along), 0.f, 1.f);
    return glm::length(point - (a + along * t));
}

void CheckNear(const glm::vec3 &actual, const glm::vec3 &expected, float tolerance = kPositionTolerance)
{
    CHECK(glm::length(actual - expected) <= tolerance);
}

/// Chords per arc when measuring a curve's length by brute force.
constexpr std::int32_t kArcChords = 256;

/// The curve's length between two parameters, summed over many short chords.
float ArcLength(const Spline &spline, float from, float to)
{
    float length = 0.f;
    glm::vec3 previous = SplinePoint(spline, from);
    for (std::int32_t i = 1; i <= kArcChords; ++i)
    {
        const glm::vec3 point =
            SplinePoint(spline, glm::mix(from, to, static_cast<float>(i) / static_cast<float>(kArcChords)));
        length += glm::length(point - previous);
        previous = point;
    }
    return length;
}

/// The direction a node is arrived at and left in, from the pieces either side.
float TurnAt(const Spline &spline, std::size_t arriving, std::size_t leaving)
{
    const glm::vec3 in = glm::normalize(SegmentDerivative(SegmentOf(spline, arriving), 1.f));
    const glm::vec3 out = glm::normalize(SegmentDerivative(SegmentOf(spline, leaving), 0.f));
    return glm::dot(in, out);
}

} // namespace

TEST_CASE("Spline: fewer than two nodes make no pieces")
{
    const Spline empty;
    CHECK(SegmentCount(empty) == 0);
    CheckNear(SplinePoint(empty, 0.5f), glm::vec3(0.f));
    CHECK(FlattenSpline(empty, kFlattenTolerance).points.empty());
    CHECK(PolylineLength(FlattenSpline(empty, kFlattenTolerance)) == 0.f);

    const Spline one = Through({{1.f, 2.f, 3.f}}, SplineNodeMode::Smooth, /*closed=*/ true);
    CHECK(SegmentCount(one) == 0);
    CheckNear(SplinePoint(one, 0.f), glm::vec3(1.f, 2.f, 3.f));
    const SplinePolyline chain = FlattenSpline(one, kFlattenTolerance);
    REQUIRE(chain.points.size() == 1);
    CheckNear(chain.points[0], glm::vec3(1.f, 2.f, 3.f));
}

TEST_CASE("Spline: an open spline has a piece between each pair of nodes, and a closed one joins its ends")
{
    CHECK(SegmentCount(Zigzag(SplineNodeMode::Smooth)) == 3);

    Spline loop = Zigzag(SplineNodeMode::Smooth);
    loop.closed = true;
    CHECK(SegmentCount(loop) == 4);
    CheckNear(SplinePoint(loop, 4.f), loop.nodes[0].position);
}

TEST_CASE("Spline: two corners make a straight line, evenly parameterised")
{
    const Spline line = Through({{0.f, 0.f, 0.f}, {4.f, 2.f, -6.f}}, SplineNodeMode::Corner);
    for (const float t : {0.1f, 0.25f, 0.5f, 0.75f, 0.9f})
    {
        const glm::vec3 point = SplinePoint(line, t);
        CHECK(DistanceToLine(point, line.nodes[0].position, line.nodes[1].position) <= kPositionTolerance);
        // Handles a third of the way along make the parameter the fraction of
        // the length, which is what a line drawn by hand would expect.
        CheckNear(point, glm::mix(line.nodes[0].position, line.nodes[1].position, t));
    }
    CHECK(PolylineLength(FlattenSpline(line, kFlattenTolerance)) == doctest::Approx(glm::length(glm::vec3(4.f, 2.f, -6.f))));
    // Straight, so it needs only its two ends.
    CHECK(FlattenSpline(line, kFlattenTolerance).points.size() == 2);
}

TEST_CASE("Spline: a curve passes through every node, whatever their modes")
{
    for (const SplineNodeMode mode : {SplineNodeMode::Corner, SplineNodeMode::Smooth, SplineNodeMode::Bezier})
    {
        const Spline spline = Zigzag(mode);
        for (std::size_t i = 0; i < spline.nodes.size(); ++i)
        {
            CheckNear(SplinePoint(spline, static_cast<float>(i)), spline.nodes[i].position);
        }
    }
}

TEST_CASE("Spline: a Smooth node does not kink, and a Corner node does")
{
    const Spline smooth = Zigzag(SplineNodeMode::Smooth);
    CHECK(TurnAt(smooth, 0, 1) >= kSameDirection);
    CHECK(TurnAt(smooth, 1, 2) >= kSameDirection);

    const Spline corner = Zigzag(SplineNodeMode::Corner);
    CHECK(TurnAt(corner, 0, 1) < kSameDirection);
}

TEST_CASE("Spline: a closed Smooth spline joins its last node to its first without a kink")
{
    const Spline loop =
        Through({{0.f, 0.f, 0.f}, {4.f, 0.f, 0.f}, {5.f, 0.f, 3.f}, {-1.f, 0.f, 4.f}}, SplineNodeMode::Smooth,
                /*closed=*/ true);
    CHECK(TurnAt(loop, 3, 0) >= kSameDirection);
    CHECK(TurnAt(loop, 0, 1) >= kSameDirection);
}

TEST_CASE("Spline: a Bezier node pulls with its stored handles")
{
    Spline spline = Through({{0.f, 0.f, 0.f}, {6.f, 0.f, 0.f}}, SplineNodeMode::Bezier);
    spline.nodes[0].handleOut = {1.f, 2.f, 0.f};
    spline.nodes[1].handleIn = {-1.f, 0.f, 3.f};

    const auto segment = SegmentOf(spline, 0);
    CheckNear(segment.startPull, glm::vec3(1.f, 2.f, 0.f));
    CheckNear(segment.endPull, glm::vec3(5.f, 0.f, 3.f));
}

TEST_CASE("Spline: settling stores what Corner and Smooth nodes draw with, and leaves Bezier nodes alone")
{
    Spline spline = Zigzag(SplineNodeMode::Smooth);
    spline.nodes[0].mode = SplineNodeMode::Corner;
    spline.nodes[3].mode = SplineNodeMode::Bezier;
    spline.nodes[3].handleIn = {-2.f, 0.f, 1.f};

    std::vector<glm::vec3> before;
    for (float parameter = 0.f; parameter <= 3.f; parameter += 0.25f)
    {
        before.push_back(SplinePoint(spline, parameter));
    }

    SettleHandles(spline);
    CheckNear(spline.nodes[3].handleIn, glm::vec3(-2.f, 0.f, 1.f));

    // Switching a settled node to Bezier keeps the curve it had.
    spline.nodes[0].mode = SplineNodeMode::Bezier;
    spline.nodes[1].mode = SplineNodeMode::Bezier;
    spline.nodes[2].mode = SplineNodeMode::Bezier;
    std::size_t sample = 0;
    for (float parameter = 0.f; parameter <= 3.f; parameter += 0.25f)
    {
        CheckNear(SplinePoint(spline, parameter), before[sample++]);
    }
}

TEST_CASE("Spline: a node's stored handles mean nothing unless it is Bezier")
{
    Spline spline = Zigzag(SplineNodeMode::Smooth);
    const glm::vec3 untouched = SplinePoint(spline, 1.5f);
    spline.nodes[1].handleIn = {10.f, 10.f, 10.f};
    spline.nodes[1].handleOut = {-10.f, 5.f, 0.f};
    spline.nodes[2].mode = SplineNodeMode::Corner;
    spline.nodes[2].handleIn = {0.f, -7.f, 0.f};
    const glm::vec3 withStaleHandles = SplinePoint(spline, 1.5f);

    Spline reference = Zigzag(SplineNodeMode::Smooth);
    reference.nodes[2].mode = SplineNodeMode::Corner;
    CheckNear(withStaleHandles, SplinePoint(reference, 1.5f));
    CHECK(glm::length(untouched - withStaleHandles) > kPositionTolerance); // the corner did change it
}

TEST_CASE("Spline: the direction where a Bezier handle has no length is the one the curve leaves in")
{
    // No handles at all: the derivative is zero at both ends, and the piece is
    // the straight line between them.
    const Spline spline = Through({{0.f, 0.f, 0.f}, {0.f, 0.f, 5.f}}, SplineNodeMode::Bezier);
    CheckNear(SplineDirection(spline, 0.f), glm::vec3(0.f, 0.f, 1.f));
    CheckNear(SplineDirection(spline, 1.f), glm::vec3(0.f, 0.f, 1.f));
    CheckNear(SplineDirection(spline, 0.5f), glm::vec3(0.f, 0.f, 1.f));
}

TEST_CASE("Spline: a flattened chain stays within its tolerance of the curve")
{
    Spline spline = Zigzag(SplineNodeMode::Smooth);
    spline.closed = true;
    const SplinePolyline chain = FlattenSpline(spline, kFlattenTolerance);

    REQUIRE(chain.points.size() == chain.parameters.size());
    REQUIRE(chain.points.size() == chain.distances.size());
    CHECK(chain.points.size() > SegmentCount(spline) + 1); // it did split the bends
    CheckNear(chain.points.front(), spline.nodes[0].position);
    CheckNear(chain.points.back(), spline.nodes[0].position);
    CHECK(chain.parameters.back() == doctest::Approx(static_cast<float>(SegmentCount(spline))));

    for (std::size_t i = 1; i < chain.points.size(); ++i)
    {
        CheckNear(chain.points[i], SplinePoint(spline, chain.parameters[i]));
        CHECK(chain.distances[i] >= chain.distances[i - 1]);
        for (std::int32_t s = 1; s < kSamplesPerLine; ++s)
        {
            const float f = static_cast<float>(s) / static_cast<float>(kSamplesPerLine);
            const float parameter = glm::mix(chain.parameters[i - 1], chain.parameters[i], f);
            CHECK(DistanceToLine(SplinePoint(spline, parameter), chain.points[i - 1], chain.points[i]) <=
                  kFlattenTolerance + kPositionTolerance);
        }
    }
}

TEST_CASE("Spline: equal steps of distance are equal steps along the curve")
{
    const Spline spline = Zigzag(SplineNodeMode::Smooth);
    const SplinePolyline chain = FlattenSpline(spline, kFlattenTolerance * 0.1f);
    const float length = PolylineLength(chain);
    REQUIRE(length > 0.f);

    // Each step measured along the true curve, independently of the chain.
    constexpr std::int32_t kSteps = 20;
    const float step = length / static_cast<float>(kSteps);
    float previous = ParameterAtDistance(chain, 0.f);
    for (std::int32_t i = 1; i <= kSteps; ++i)
    {
        const float parameter = ParameterAtDistance(chain, step * static_cast<float>(i));
        CHECK(ArcLength(spline, previous, parameter) == doctest::Approx(step).epsilon(0.005));
        previous = parameter;
    }
    CHECK(ParameterAtDistance(chain, -1.f) == doctest::Approx(0.f));
    CHECK(ParameterAtDistance(chain, length + 1.f) == doctest::Approx(3.f));
}

TEST_CASE("Spline: the nearest point of the curve is found")
{
    const Spline spline = Zigzag(SplineNodeMode::Smooth);
    const SplinePolyline chain = FlattenSpline(spline, kFlattenTolerance * 0.1f);

    // The zigzag lies flat, so a point straight above the curve is nearest the
    // curve point beneath it.
    const float parameter = 1.37f;
    const glm::vec3 onCurve = SplinePoint(spline, parameter);
    const SplineClosest closest = ClosestOnSpline(spline, chain, onCurve + glm::vec3(0.f, 0.5f, 0.f));
    CHECK(closest.parameter == doctest::Approx(parameter).epsilon(0.01));
    CheckNear(closest.position, onCurve, kFlattenTolerance);
    CheckNear(closest.position, SplinePoint(spline, closest.parameter));
    // The distance it reports is where that parameter lies along the chain.
    CHECK(ParameterAtDistance(chain, closest.distance) == doctest::Approx(closest.parameter).epsilon(1e-4));
}
