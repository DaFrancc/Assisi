/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestFogVolume.cpp
/// @brief A placed fog volume's span along a line of sight, against lengths
/// known by hand and against the same fog summed in small steps; the colour it
/// shows; how volumes blend; and which squares of the screen list each one.

#include <doctest/doctest.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/FogVolume.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

using namespace Assisi::Render;

namespace
{

constexpr float kForever = std::numeric_limits<float>::infinity();

/// Steps the brute-force sums take across a span.
constexpr int32_t kSumSteps = 20000;

FogVolumeGpu Volume(FogVolumeKind kind, const glm::vec3 &half, float softness, const glm::mat4 &frame = glm::mat4(1.0f),
                    float falloff = 0.0f)
{
    return MakeFogVolume(FogVolumeDesc{.frame = frame,
                                       .halfExtents = half,
                                       .visibility = 10.0f,
                                       .edgeSoftness = softness,
                                       .falloff = falloff,
                                       .kind = kind});
}

float Span(const FogVolumeGpu &volume, const glm::vec3 &eye, const glm::vec3 &ray, float distance = kForever,
           std::span<const FogOutlineEdgeGpu> edges = {})
{
    return FogVolumeSpan(volume, edges, eye, glm::normalize(ray), distance).length;
}

/// Across a slab @p span thick, at @p depth into it: full in the middle, falling
/// straight to nothing over @p soft, at most half the slab, at each face.
float SlabFog(float depth, float span, float soft)
{
    if (depth < 0.0f || depth > span)
    {
        return 0.0f;
    }
    if (soft <= 0.0f)
    {
        return 1.0f;
    }
    return std::min(std::min(depth, span - depth) / std::min(soft, 0.5f * span), 1.0f);
}

/// What the falloff leaves of the fog @p height above the floor.
float Falloff(float height, float falloff)
{
    return falloff > 0.0f ? std::exp(-std::max(height, 0.0f) / falloff) : 1.0f;
}

/// A box's fog at @p p, point by point: each axis's softness, multiplied, and
/// the falloff above its floor.
float BoxDensity(const glm::vec3 &p, const glm::vec3 &half, float softness, float falloff = 0.0f)
{
    float density = Falloff(p.y + half.y, falloff);
    for (glm::length_t axis = 0; axis < 3; ++axis)
    {
        density *= SlabFog(p[axis] + half[axis], 2.0f * half[axis], softness);
    }
    return density;
}

/// An ellipsoid's fog at @p p: full inside the core, thinning as 1 - r^2 over
/// the shell @p band wide in r^2, and the falloff above its floor.
float EllipsoidDensity(const glm::vec3 &p, const glm::vec3 &radii, float band, float falloff = 0.0f)
{
    const glm::vec3 unit = p / radii;
    return std::clamp((1.0f - glm::dot(unit, unit)) / band, 0.0f, 1.0f) * Falloff(p.y + radii.y, falloff);
}

/// Square @p tile's list, out of what BuildFogTileLists wrote.
std::vector<uint32_t> ListOf(const std::vector<uint32_t> &tiles, uint32_t tile)
{
    const uint32_t offset = tiles[tile * 2u];
    const uint32_t count = tiles[tile * 2u + 1u];
    return std::vector<uint32_t>(tiles.begin() + offset, tiles.begin() + offset + count);
}

/// An outline volume's fog at @p p, point by point: inside the outline, less
/// each wall band's thinning, times the floor's and the top's softness and the
/// falloff above the floor.
float OutlineDensity(const glm::vec3 &p, const glm::vec3 &half, float softness,
                     std::span<const FogOutlineEdgeGpu> edges, float falloff = 0.0f)
{
    const glm::vec2 q(p.x, p.z);
    bool inside = false;
    for (const FogOutlineEdgeGpu &edge : edges)
    {
        const glm::vec2 a(edge.points.x, edge.points.y);
        const glm::vec2 b(edge.points.z, edge.points.w);
        if ((a.y > q.y) != (b.y > q.y) && q.x < a.x + (q.y - a.y) * (b.x - a.x) / (b.y - a.y))
        {
            inside = !inside;
        }
    }
    float across = inside ? 1.0f : 0.0f;
    if (softness > 0.0f)
    {
        for (const FogOutlineEdgeGpu &edge : edges)
        {
            const glm::vec2 a(edge.points.x, edge.points.y);
            const glm::vec2 b(edge.points.z, edge.points.w);
            const float depth = glm::dot(glm::vec2(edge.normal), q - a);
            const bool inBand = depth >= 0.0f && depth <= softness &&
                                glm::dot(glm::vec2(edge.miters.x, edge.miters.y), q - a) >= 0.0f &&
                                glm::dot(glm::vec2(edge.miters.z, edge.miters.w), q - b) <= 0.0f;
            if (inBand)
            {
                across -= 1.0f - depth / softness;
            }
        }
    }
    return across * SlabFog(p.y + half.y, 2.0f * half.y, softness) * Falloff(p.y + half.y, falloff);
}

/// The density @p density gives, summed in small steps from @p from to @p to
/// along the line, in the volume's frame.
template <typename Density>
float Summed(const glm::vec3 &eye, const glm::vec3 &ray, float from, float to, Density density)
{
    double sum = 0.0;
    const double step = (static_cast<double>(to) - static_cast<double>(from)) / kSumSteps;
    for (int32_t i = 0; i < kSumSteps; ++i)
    {
        const float t = static_cast<float>(static_cast<double>(from) + (static_cast<double>(i) + 0.5) * step);
        sum += static_cast<double>(density(eye + ray * t)) * step;
    }
    return static_cast<float>(sum);
}

std::vector<glm::vec2> Square(float half)
{
    return {{-half, -half}, {half, -half}, {half, half}, {-half, half}};
}

/// A U, open toward +z: two arms of width 2 joined along the bottom.
std::vector<glm::vec2> UShape()
{
    return {{-5.0f, -5.0f}, {5.0f, -5.0f}, {5.0f, 5.0f}, {3.0f, 5.0f},
        {3.0f, -3.0f}, {-3.0f, -3.0f}, {-3.0f, 5.0f}, {-5.0f, 5.0f}};
}

struct Outline
{
    std::vector<FogOutlineEdgeGpu> edges;
    FogOutlinePlacement placement;
    FogVolumeGpu volume;
};

Outline MakeOutline(std::span<const glm::vec2> points, float halfHeight, float softness, float falloff = 0.0f)
{
    Outline outline;
    const std::optional<FogOutlinePlacement> placement = AppendFogOutline(outline.edges, points);
    REQUIRE(placement.has_value());
    outline.placement = *placement;
    outline.volume = MakeFogVolume(FogVolumeDesc{
            .frame = glm::translate(glm::mat4(1.0f), glm::vec3(placement->centre.x, 0.0f, placement->centre.y)),
            .halfExtents = glm::vec3(placement->halfExtents.x, halfHeight, placement->halfExtents.y),
            .visibility = 10.0f,
            .edgeSoftness = softness,
            .falloff = falloff,
            .firstEdge = placement->firstEdge,
            .edgeCount = placement->edgeCount,
            .kind = FogVolumeKind::Outline});
    return outline;
}

float OutlineSpan(const Outline &outline, const glm::vec3 &eye, const glm::vec3 &ray, float distance = kForever)
{
    return Span(outline.volume, eye, ray, distance, outline.edges);
}

glm::mat4 Perspective()
{
    const glm::mat4 projection = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    return projection * view;
}

glm::mat4 At(const glm::vec3 &position)
{
    return glm::translate(glm::mat4(1.0f), position);
}

} // namespace

TEST_CASE("FogVolume: a hard box holds its width of fog, from the face the line meets")
{
    const FogVolumeGpu box = Volume(FogVolumeKind::Box, glm::vec3(5.0f, 4.0f, 3.0f), 0.0f);
    const FogSpan span = FogVolumeSpan(box, {}, glm::vec3(-20.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), kForever);
    CHECK(span.length == doctest::Approx(10.0f));
    CHECK(span.entry == doctest::Approx(15.0f));
    CHECK(Span(box, glm::vec3(-20.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f)) == 0.0f);
    CHECK(Span(box, glm::vec3(-20.0f, 5.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f)) == 0.0f);
}

TEST_CASE("FogVolume: soft faces thin a straight crossing by half the softness each")
{
    const FogVolumeGpu box = Volume(FogVolumeKind::Box, glm::vec3(5.0f, 4.0f, 3.0f), 1.0f);
    CHECK(Span(box, glm::vec3(-20.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f)) == doctest::Approx(9.0f));
    // From inside, only the face it leaves by.
    CHECK(Span(box, glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f)) == doctest::Approx(4.5f));
    // A surface inside stops it before the far face.
    CHECK(Span(box, glm::vec3(-20.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 22.0f) == doctest::Approx(6.5f));
}

TEST_CASE("FogVolume: a box's silhouette fades out instead of ending at a hard edge")
{
    const glm::vec3 half(5.0f, 4.0f, 3.0f);
    const float softness = 1.0f;
    const FogVolumeGpu box = Volume(FogVolumeKind::Box, half, softness);
    // Seen face on, along z, a line half the softness inside the x face: the
    // x face thins it to half all the way through, which multiplies the depth
    // across, less the two z faces' half softness each.
    const float near = Span(box, glm::vec3(half.x - 0.5f, 0.0f, -20.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    CHECK(near == doctest::Approx((2.0f * half.z - softness) * 0.5f));
    // And next to nothing a tenth of a millimetre from the face.
    CHECK(Span(box, glm::vec3(half.x - 1.0e-4f, 0.0f, -20.0f), glm::vec3(0.0f, 0.0f, 1.0f)) < 1.0e-3f);
}

TEST_CASE("FogVolume: along a box's edge two faces' softness multiplies, and never takes away more fog than there is")
{
    const glm::vec3 half(5.0f, 4.0f, 3.0f);
    const float softness = 1.0f;
    const FogVolumeGpu box = Volume(FogVolumeKind::Box, half, softness);
    // Along x, a quarter of the softness inside both the top and the far z
    // face: a quarter of a quarter of the fog the line would hold mid-box.
    const glm::vec3 edgeEye(-20.0f, half.y - 0.25f, half.z - 0.25f);
    CHECK(Span(box, edgeEye, glm::vec3(1.0f, 0.0f, 0.0f)) == doctest::Approx((2.0f * half.x - softness) / 16.0f));
    // Through where the top's and a side's softness overlap, then on through
    // the box: no less fog than summing it in steps finds.
    const glm::vec3 eye(-12.0f, half.y + 2.0f, 0.4f);
    const glm::vec3 ray = glm::normalize(glm::vec3(1.0f, -0.4f, 0.05f));
    CHECK(Span(box, eye, ray) ==
          doctest::Approx(Summed(eye, ray, 0.0f, 40.0f, [&](const glm::vec3 &p) { return BoxDensity(p, half, softness); }))
          .epsilon(1.0e-3));
}

TEST_CASE("FogVolume: the falloff thins a volume's fog upward from its floor")
{
    const glm::vec3 half(5.0f, 4.0f, 5.0f);
    const float falloff = 2.0f;
    const FogVolumeGpu box = Volume(FogVolumeKind::Box, half, 0.0f, glm::mat4(1.0f), falloff);
    // Straight up through all eight metres: the integral of exp(-h / 2).
    CHECK(Span(box, glm::vec3(0.0f, -20.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)) ==
          doctest::Approx(falloff * (1.0f - std::exp(-2.0f * half.y / falloff))).epsilon(5.0e-3));
    // Level, three metres above the floor: the whole width at exp(-3 / 2).
    CHECK(Span(box, glm::vec3(-20.0f, 3.0f - half.y, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f)) ==
          doctest::Approx(2.0f * half.x * std::exp(-3.0f / falloff)).epsilon(1.0e-4));

    // Soft and slanted, a box, an ellipsoid and an outline each hold what
    // summing their fog in steps does.
    const float softness = 1.0f;
    const FogVolumeGpu softBox = Volume(FogVolumeKind::Box, half, softness, glm::mat4(1.0f), falloff);
    const glm::vec3 eye(-12.0f, 6.0f, -3.0f);
    const glm::vec3 ray = glm::normalize(glm::vec3(1.0f, -0.6f, 0.3f));
    CHECK(Span(softBox, eye, ray) ==
          doctest::Approx(Summed(eye, ray, 0.0f, 40.0f,
                                 [&](const glm::vec3 &p) { return BoxDensity(p, half, softness, falloff); }))
          .epsilon(5.0e-3));

    const glm::vec3 radii(5.0f, 3.0f, 4.0f);
    const FogVolumeGpu ellipsoid = Volume(FogVolumeKind::Ellipsoid, radii, softness, glm::mat4(1.0f), falloff);
    const float depth = softness / 3.0f;
    const float band = depth * (2.0f - depth);
    CHECK(Span(ellipsoid, eye, ray) ==
          doctest::Approx(Summed(eye, ray, 0.0f, 40.0f,
                                 [&](const glm::vec3 &p) { return EllipsoidDensity(p, radii, band, falloff); }))
          .epsilon(5.0e-3));

    const Outline outline = MakeOutline(UShape(), 2.0f, 0.6f, 1.0f);
    const glm::vec3 outlineHalf(outline.volume.extents);
    const glm::vec3 outlineEye(-8.0f, 6.0f, -8.0f);
    const glm::vec3 outlineRay = glm::normalize(glm::vec3(0.7f, -0.5f, 1.0f));
    CHECK(OutlineSpan(outline, outlineEye, outlineRay) ==
          doctest::Approx(Summed(outlineEye, outlineRay, 0.0f, 40.0f,
                                 [&](const glm::vec3 &p) {
        return OutlineDensity(p, outlineHalf, 0.6f, outline.edges, 1.0f);
    }))
          .epsilon(5.0e-3));
}

TEST_CASE("FogVolume: a slanted line through a soft box holds what summing its fog in steps does")
{
    const glm::vec3 half(5.0f, 4.0f, 3.0f);
    const float softness = 1.5f;
    const FogVolumeGpu box = Volume(FogVolumeKind::Box, half, softness);
    const glm::vec3 eye(-12.0f, 3.0f, -5.0f);
    const glm::vec3 ray = glm::normalize(glm::vec3(1.0f, -0.35f, 0.4f));
    const float closedForm = Span(box, eye, ray);
    const float summed = Summed(eye, ray, 0.0f, 40.0f, [&](const glm::vec3 &p) { return BoxDensity(p, half, softness); });
    CHECK(closedForm > 0.0f);
    CHECK(closedForm == doctest::Approx(summed).epsilon(1.0e-3));
}

TEST_CASE("FogVolume: a box turns with its frame")
{
    const glm::mat4 frame = glm::rotate(At(glm::vec3(100.0f, 0.0f, 0.0f)), glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const FogVolumeGpu box = Volume(FogVolumeKind::Box, glm::vec3(5.0f, 4.0f, 2.0f), 0.0f, frame);
    // Turned a quarter about y, its z extent lies along world x.
    CHECK(Span(box, glm::vec3(80.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f)) == doctest::Approx(4.0f));
    CHECK(Span(box, glm::vec3(100.0f, 0.0f, -20.0f), glm::vec3(0.0f, 0.0f, 1.0f)) == doctest::Approx(10.0f));
}

TEST_CASE("FogVolume: a hard ellipsoid holds its chord")
{
    const FogVolumeGpu sphere = Volume(FogVolumeKind::Ellipsoid, glm::vec3(3.0f), 0.0f);
    CHECK(Span(sphere, glm::vec3(-10.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f)) == doctest::Approx(6.0f));
    // Two units off centre, a chord of 2 sqrt(9 - 4).
    CHECK(Span(sphere, glm::vec3(-10.0f, 2.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f)) ==
          doctest::Approx(2.0f * std::sqrt(5.0f)));
    const FogVolumeGpu stretched = Volume(FogVolumeKind::Ellipsoid, glm::vec3(6.0f, 1.0f, 1.0f), 0.0f);
    CHECK(Span(stretched, glm::vec3(-10.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f)) == doctest::Approx(12.0f));
    CHECK(Span(stretched, glm::vec3(0.0f, -10.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)) == doctest::Approx(2.0f));
}

TEST_CASE("FogVolume: an ellipsoid soft all the way through thins from its centre out")
{
    const float radius = 4.0f;
    const FogVolumeGpu sphere = Volume(FogVolumeKind::Ellipsoid, glm::vec3(radius), radius);
    // The integral of 1 - r^2 / R^2 across a diameter: 2R - 2R / 3.
    CHECK(Span(sphere, glm::vec3(-10.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f)) ==
          doctest::Approx(4.0f * radius / 3.0f));
}

TEST_CASE("FogVolume: a partly soft ellipsoid holds what summing its fog in steps does")
{
    const glm::vec3 radii(4.0f, 3.0f, 5.0f);
    const float softness = 1.0f;
    const FogVolumeGpu ellipsoid = Volume(FogVolumeKind::Ellipsoid, radii, softness);
    // The shell is the softness deep where the ellipsoid is thinnest.
    const float depth = softness / 3.0f;
    const float band = depth * (2.0f - depth);
    const glm::vec3 eye(-10.0f, 1.2f, -0.7f);
    const glm::vec3 ray = glm::normalize(glm::vec3(1.0f, 0.1f, 0.2f));
    CHECK(Span(ellipsoid, eye, ray) ==
          doctest::Approx(Summed(eye, ray, 0.0f, 25.0f, [&](const glm::vec3 &p) { return EllipsoidDensity(p, radii, band); }))
          .epsilon(1.0e-3));
    // And stopped by a surface inside it.
    CHECK(Span(ellipsoid, eye, ray, 9.5f) ==
          doctest::Approx(Summed(eye, ray, 0.0f, 9.5f, [&](const glm::vec3 &p) { return EllipsoidDensity(p, radii, band); }))
          .epsilon(1.0e-3));
    // Through the full core as well as the shell around it.
    const glm::vec3 coreEye(-10.0f, 0.3f, 0.2f);
    const glm::vec3 coreRay = glm::normalize(glm::vec3(1.0f, 0.02f, 0.05f));
    CHECK(Span(ellipsoid, coreEye, coreRay) ==
          doctest::Approx(Summed(coreEye, coreRay, 0.0f, 25.0f, [&](const glm::vec3 &p) { return EllipsoidDensity(p, radii, band); }))
          .epsilon(1.0e-3));
}

TEST_CASE("FogVolume: a far ellipsoid keeps its precision")
{
    const FogVolumeGpu sphere = Volume(FogVolumeKind::Ellipsoid, glm::vec3(5.0f), 0.0f, At(glm::vec3(0.0f, 0.0f, -5000.0f)));
    CHECK(Span(sphere, glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)) == doctest::Approx(8.0f).epsilon(1.0e-3));
}

TEST_CASE("FogVolume: an outline places its edges anticlockwise around its middle")
{
    std::vector<FogOutlineEdgeGpu> edges;
    // Clockwise, with a repeated point and the first point again at the end.
    const std::vector<glm::vec2> points = {{12.0f, 2.0f}, {12.0f, 6.0f}, {12.0f, 6.0f}, {16.0f, 6.0f}, {16.0f, 2.0f}, {12.0f, 2.0f}};
    const std::optional<FogOutlinePlacement> placement = AppendFogOutline(edges, points);
    REQUIRE(placement.has_value());
    CHECK(placement->edgeCount == 4u);
    CHECK(placement->centre == glm::vec2(14.0f, 4.0f));
    CHECK(placement->halfExtents == glm::vec2(2.0f, 2.0f));
    for (const FogOutlineEdgeGpu &edge : edges)
    {
        const glm::vec2 middle = (glm::vec2(edge.points.x, edge.points.y) + glm::vec2(edge.points.z, edge.points.w)) * 0.5f;
        // Every normal points back toward the middle.
        CHECK(glm::dot(glm::vec2(edge.normal), -middle) > 0.0f);
    }
    CHECK_FALSE(AppendFogOutline(edges, std::vector<glm::vec2>{{0.0f, 0.0f}, {1.0f, 0.0f}}).has_value());
    CHECK_FALSE(AppendFogOutline(edges, std::vector<glm::vec2>{{0.0f, 0.0f}, {1.0f, 0.0f}, {2.0f, 0.0f}}).has_value());
    CHECK(edges.size() == 4u);
}

TEST_CASE("FogVolume: a square outline holds what a box of its size does")
{
    const std::vector<glm::vec2> square = Square(5.0f);
    for (const float softness : {0.0f, 1.0f})
    {
        const Outline outline = MakeOutline(square, 2.0f, softness);
        const FogVolumeGpu box = Volume(FogVolumeKind::Box, glm::vec3(5.0f, 2.0f, 5.0f), softness);
        // Lines that stay clear of the box's upright edges, where the two
        // differ: a box thins its corners from both faces, an outline from the
        // nearer wall.
        const std::array<std::array<glm::vec3, 2>, 5> lines = {{
            {glm::vec3(-20.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f)},
            {glm::vec3(-20.0f, 1.5f, 1.0f), glm::vec3(1.0f, -0.1f, 0.05f)},
            {glm::vec3(0.5f, 10.0f, -0.5f), glm::vec3(0.0f, -1.0f, 0.0f)},
            {glm::vec3(1.0f, 10.0f, -20.0f), glm::vec3(0.0f, -0.4f, 1.0f)},
            {glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.3f, 1.0f)},
        }};
        for (const std::array<glm::vec3, 2> &line : lines)
        {
            CAPTURE(softness);
            CAPTURE(line[0]);
            CHECK(OutlineSpan(outline, line[0], line[1]) == doctest::Approx(Span(box, line[0], line[1])).epsilon(1.0e-4));
        }
    }
}

TEST_CASE("FogVolume: an outline is the same fog whichever way round it is drawn")
{
    std::vector<glm::vec2> points = UShape();
    const Outline forward = MakeOutline(points, 2.0f, 0.5f);
    std::reverse(points.begin(), points.end());
    const Outline backward = MakeOutline(points, 2.0f, 0.5f);
    const glm::vec3 eye(-20.0f, 0.5f, 2.0f);
    const glm::vec3 ray(1.0f, -0.02f, 0.1f);
    CHECK(OutlineSpan(forward, eye, ray) == doctest::Approx(OutlineSpan(backward, eye, ray)));
}

TEST_CASE("FogVolume: a line across both arms of a U holds both arms' fog and none of the gap's")
{
    const Outline outline = MakeOutline(UShape(), 2.0f, 0.0f);
    // Across at z = 3, through the arms at x in [-5, -3] and [3, 5].
    CHECK(OutlineSpan(outline, glm::vec3(-20.0f, 0.0f, 3.0f), glm::vec3(1.0f, 0.0f, 0.0f)) == doctest::Approx(4.0f));
    // Starting inside the left arm.
    CHECK(OutlineSpan(outline, glm::vec3(-4.0f, 0.0f, 3.0f), glm::vec3(1.0f, 0.0f, 0.0f)) == doctest::Approx(3.0f));
    // Straight down into the gap, and into an arm.
    CHECK(OutlineSpan(outline, glm::vec3(0.0f, 10.0f, 3.0f), glm::vec3(0.0f, -1.0f, 0.0f)) == 0.0f);
    CHECK(OutlineSpan(outline, glm::vec3(4.0f, 10.0f, 3.0f), glm::vec3(0.0f, -1.0f, 0.0f)) == doctest::Approx(4.0f));
}

TEST_CASE("FogVolume: slanted lines through a soft U hold what summing its fog in steps does")
{
    const float softness = 0.6f;
    const Outline outline = MakeOutline(UShape(), 2.0f, softness);
    const glm::vec3 half(outline.volume.extents);
    const std::array<std::array<glm::vec3, 2>, 4> lines = {{
        {glm::vec3(-20.0f, 1.0f, 2.5f), glm::vec3(1.0f, -0.05f, 0.08f)},
        {glm::vec3(-8.0f, 6.0f, -8.0f), glm::vec3(0.7f, -0.5f, 1.0f)},
        {glm::vec3(4.7f, 10.0f, 1.0f), glm::vec3(0.02f, -1.0f, 0.01f)},
        {glm::vec3(0.0f, 0.5f, -4.0f), glm::vec3(1.0f, 0.0f, 0.3f)},
    }};
    for (const std::array<glm::vec3, 2> &line : lines)
    {
        const glm::vec3 ray = glm::normalize(line[1]);
        CAPTURE(line[0]);
        const float summed =
            Summed(line[0], ray, 0.0f, 40.0f, [&](const glm::vec3 &p) { return OutlineDensity(p, half, softness, outline.edges); });
        CHECK(OutlineSpan(outline, line[0], ray) == doctest::Approx(std::max(summed, 0.0f)).epsilon(1.0e-3));
    }
}

TEST_CASE("FogVolume: a soft outline has no seam where a line's crossing passes a corner")
{
    // A pentagon's corners are where one wall's band hands over to the next.
    // Lines a step apart sweeping across one must change smoothly.
    const std::vector<glm::vec2> pentagon = {{0.0f, -5.0f}, {5.0f, -1.5f}, {3.0f, 4.5f}, {-3.0f, 4.5f}, {-5.0f, -1.5f}};
    const Outline outline = MakeOutline(pentagon, 2.0f, 1.5f);
    // Lines along z, half a millimetre apart, from left of the outline to
    // right of it, passing all five corners on the way.
    constexpr int32_t kLines = 22000;
    constexpr float kFirst = -5.5f;
    constexpr float kStep = 0.0005f;
    const glm::vec3 ray(0.0f, 0.0f, 1.0f);
    float largestJump = 0.0f;
    float previous = OutlineSpan(outline, glm::vec3(kFirst, 0.0f, -20.0f), ray);
    for (int32_t i = 1; i < kLines; ++i)
    {
        const float x = kFirst + static_cast<float>(i) * kStep;
        const float span = OutlineSpan(outline, glm::vec3(x, 0.0f, -20.0f), ray);
        largestJump = std::max(largestJump, std::abs(span - previous));
        previous = span;
    }
    // Every wall meets these lines at 18 degrees or more, so a smooth span
    // moves by at most twice cot(18) per metre of step: about 3 mm here.
    CHECK(largestJump < 0.01f);
}

TEST_CASE("FogVolume: straight down near a wall, the fog thins toward it")
{
    const float softness = 1.0f;
    const Outline outline = MakeOutline(Square(5.0f), 2.0f, softness);
    // Half the softness inside the x = 5 wall: half of the 4 m height, less
    // the floor's and the top's half softness each.
    CHECK(OutlineSpan(outline, glm::vec3(4.5f, 10.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)) ==
          doctest::Approx((4.0f - softness) * 0.5f));
    CHECK(OutlineSpan(outline, glm::vec3(4.9999f, 10.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)) < 1.0e-3f);
}

TEST_CASE("FogVolume: a lit volume tints the haze's colour, and the haze in front covers it")
{
    FogVolumeGpu volume = MakeFogVolume(FogVolumeDesc{.color = glm::vec3(0.5f, 1.0f, 0.25f), .visibility = 10.0f, .litBySky = true});
    const glm::vec3 haze(0.2f, 0.4f, 0.8f);
    CHECK(FogVolumeColor(volume, haze, 0.0f) == glm::vec3(0.1f, 0.4f, 0.2f));
    CHECK(FogVolumeColor(volume, haze, 1.0f) == haze);
    volume.info.y = 0u;
    CHECK(FogVolumeColor(volume, haze, 0.0f) == glm::vec3(0.5f, 1.0f, 0.25f));
}

TEST_CASE("FogVolume: volumes blend in any order, as one fog of their summed depth")
{
    const glm::vec3 behind(1.0f, 0.0f, 0.0f);
    FogVolumeBlend none;
    CHECK(none.Over(behind) == behind);

    FogVolumeBlend one;
    one.Add(0.7f, glm::vec3(0.0f, 1.0f, 0.0f));
    FogVolumeBlend halves;
    halves.Add(0.3f, glm::vec3(0.0f, 1.0f, 0.0f));
    halves.Add(0.4f, glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec3 expected = behind * std::exp(-0.7f) + glm::vec3(0.0f, 1.0f, 0.0f) * (1.0f - std::exp(-0.7f));
    CHECK(glm::all(glm::epsilonEqual(one.Over(behind), expected, 1.0e-6f)));
    CHECK(glm::all(glm::epsilonEqual(halves.Over(behind), expected, 1.0e-6f)));

    FogVolumeBlend ab;
    ab.Add(0.5f, glm::vec3(0.0f, 1.0f, 0.0f));
    ab.Add(1.5f, glm::vec3(0.0f, 0.0f, 1.0f));
    FogVolumeBlend ba;
    ba.Add(1.5f, glm::vec3(0.0f, 0.0f, 1.0f));
    ba.Add(0.5f, glm::vec3(0.0f, 1.0f, 0.0f));
    CHECK(glm::all(glm::epsilonEqual(ab.Over(behind), ba.Over(behind), 1.0e-6f)));
}

TEST_CASE("FogVolume: the screen is cut into squares of a fixed size, larger only on a huge screen")
{
    const FogTileGrid hd = FogTileGridFor(1920u, 1080u);
    CHECK(hd.tilePixels == kFogTilePixels);
    CHECK(hd.columns == 30u);
    CHECK(hd.rows == 17u);
    const FogTileGrid portrait = FogTileGridFor(1080u, 1920u);
    CHECK(portrait.columns == 17u);
    CHECK(portrait.rows == 30u);
    const FogTileGrid huge = FogTileGridFor(16384u, 16384u);
    CHECK(huge.tilePixels > kFogTilePixels);
    CHECK(huge.columns * huge.rows <= kMaxFogTiles);
}

TEST_CASE("FogVolume: a volume lists in the squares it covers on screen, and in none off it")
{
    const glm::mat4 viewProjection = Perspective();
    const FogTileGrid grid = FogTileGridFor(1920u, 1080u);
    const glm::vec3 half(1.0f);

    const std::optional<FogTileRect> ahead = FogVolumeTileRect(viewProjection, At(glm::vec3(0.0f, 0.0f, -20.0f)), half, grid);
    REQUIRE(ahead.has_value());
    // Around the middle square, well short of the edges.
    CHECK(ahead->minColumn <= 15u);
    CHECK(ahead->maxColumn >= 14u);
    CHECK(ahead->minRow <= 8u);
    CHECK(ahead->maxRow >= 8u);
    CHECK(ahead->minColumn > 0u);
    CHECK(ahead->maxColumn < grid.columns - 1u);

    // To the right of the view: the right-hand squares only.
    const std::optional<FogTileRect> right = FogVolumeTileRect(viewProjection, At(glm::vec3(19.0f, 0.0f, -20.0f)), half, grid);
    REQUIRE(right.has_value());
    CHECK(right->minColumn > 15u);
    CHECK(right->maxColumn == grid.columns - 1u);

    // Above the view's middle: the top rows, the top of the screen being row 0.
    const std::optional<FogTileRect> above = FogVolumeTileRect(viewProjection, At(glm::vec3(0.0f, 10.0f, -20.0f)), half, grid);
    REQUIRE(above.has_value());
    CHECK(above->maxRow < 8u);

    CHECK_FALSE(FogVolumeTileRect(viewProjection, At(glm::vec3(0.0f, 0.0f, 20.0f)), half, grid).has_value());
    CHECK_FALSE(FogVolumeTileRect(viewProjection, At(glm::vec3(200.0f, 0.0f, -20.0f)), half, grid).has_value());

    // Around the eye: every square.
    const std::optional<FogTileRect> around = FogVolumeTileRect(viewProjection, glm::mat4(1.0f), glm::vec3(5.0f), grid);
    REQUIRE(around.has_value());
    CHECK(around->minColumn == 0u);
    CHECK(around->minRow == 0u);
    CHECK(around->maxColumn == grid.columns - 1u);
    CHECK(around->maxRow == grid.rows - 1u);
}

TEST_CASE("FogVolume: each square lists the volumes covering it, after an offset and a count per square")
{
    const FogTileGrid grid{.width = 3u, .height = 2u, .tilePixels = 1u, .columns = 3u, .rows = 2u};
    const std::vector<FogTileRect> rects = {{.minColumn = 0u, .minRow = 0u, .maxColumn = 1u, .maxRow = 0u},
        {.minColumn = 1u, .minRow = 0u, .maxColumn = 2u, .maxRow = 1u}};
    std::vector<uint32_t> tiles;
    BuildFogTileLists(grid, rects, tiles);
    REQUIRE(tiles.size() == 12u + 6u);
    CHECK(ListOf(tiles, 0u) == std::vector<uint32_t>{0u});
    CHECK(ListOf(tiles, 1u) == std::vector<uint32_t>{0u, 1u});
    CHECK(ListOf(tiles, 2u) == std::vector<uint32_t>{1u});
    CHECK(ListOf(tiles, 3u).empty());
    CHECK(ListOf(tiles, 4u) == std::vector<uint32_t>{1u});
    CHECK(ListOf(tiles, 5u) == std::vector<uint32_t>{1u});
}

TEST_CASE("FogVolume: the lists stop at their room, and the squares listed last lose volumes")
{
    const FogTileGrid grid = FogTileGridFor(8192u, 4096u);
    const uint32_t tiles = grid.columns * grid.rows;
    const uint32_t volumes = kMaxFogTileEntries / tiles + 1u;
    const std::vector<FogTileRect> rects(volumes, FogTileRect{.maxColumn = grid.columns - 1u, .maxRow = grid.rows - 1u});
    std::vector<uint32_t> out;
    BuildFogTileLists(grid, rects, out);
    CHECK(out.size() == tiles * 2u + kMaxFogTileEntries);
    CHECK(out[1] == volumes);
    CHECK(out[(tiles - 1u) * 2u + 1u] < volumes);
}
