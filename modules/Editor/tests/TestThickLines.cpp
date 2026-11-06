/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestThickLines.cpp
/// @brief A thick line's segments become two triangles each, which tile the
///        strip around the segment: both ends, both sides, no overlap.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <Assisi/Editor/Overlay/LineVertices.hpp>

using Assisi::Editor::AppendThickLines;
using Assisi::Editor::LineVertex;
using Assisi::Editor::ThickLineVertex;

namespace
{

/// The same side of the strip, worked out from where a corner sits: a corner
/// at the far end pushes the opposite way to reach the side a near one does.
float StripSide(const ThickLineVertex &corner, const glm::vec3 &start)
{
    return corner.position == start ? corner.side : -corner.side;
}

/// Which corner of the strip a vertex is: which end, and which side.
struct StripCorner
{
    bool atStart = false;
    bool positive = false;

    bool operator==(const StripCorner &) const = default;
};

StripCorner CornerOf(const ThickLineVertex &corner, const glm::vec3 &start)
{
    return StripCorner{.atStart = corner.position == start, .positive = StripSide(corner, start) > 0.f};
}

} // namespace

TEST_CASE("ThickLines: each segment becomes six corners, covering both ends and both sides")
{
    const glm::vec4 red(1.f, 0.f, 0.f, 1.f);
    const glm::vec4 blue(0.f, 0.f, 1.f, 1.f);
    const std::vector<LineVertex> lines = {{.position = {0.f, 0.f, 0.f}, .color = red},
        {.position = {5.f, 0.f, 0.f}, .color = blue},
        {.position = {9.f, 9.f, 9.f}, .color = red}};                                    // unpaired: dropped

    std::vector<ThickLineVertex> corners;
    AppendThickLines(corners, lines, 4.f);
    REQUIRE(corners.size() == 6);

    const glm::vec3 start = lines[0].position;
    const glm::vec3 end = lines[1].position;
    int32_t startNegative = 0;
    int32_t startPositive = 0;
    int32_t endNegative = 0;
    int32_t endPositive = 0;
    for (const ThickLineVertex &corner : corners)
    {
        CHECK(corner.halfWidth == doctest::Approx(2.f));
        const bool atStart = corner.position == start;
        CHECK(corner.other == (atStart ? end : start));
        CHECK(corner.color == (atStart ? red : blue));
        const float side = StripSide(corner, start);
        (atStart ? (side < 0.f ? startNegative : startPositive) : (side < 0.f ? endNegative : endPositive))++;
    }
    // All four corners of the strip are used.
    CHECK(startNegative >= 1);
    CHECK(startPositive >= 1);
    CHECK(endNegative >= 1);
    CHECK(endPositive >= 1);

    // The two triangles tile the strip rather than overlap: the corners they
    // share are a diagonal, at opposite ends and on opposite sides. Two shared
    // corners on one side would fold one triangle over the other and leave a
    // gap along the far edge.
    std::vector<StripCorner> shared;
    for (std::size_t a = 0; a < 3; ++a)
    {
        for (std::size_t b = 3; b < 6; ++b)
        {
            const StripCorner first = CornerOf(corners[a], start);
            if (first == CornerOf(corners[b], start))
            {
                shared.push_back(first);
            }
        }
    }
    REQUIRE(shared.size() == 2);
    CHECK(shared[0].atStart != shared[1].atStart);
    CHECK(shared[0].positive != shared[1].positive);
}
