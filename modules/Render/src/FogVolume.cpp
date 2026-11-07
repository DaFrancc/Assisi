/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/FogVolume.hpp>

#include <Assisi/Render/Fog.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace Assisi::Render
{
namespace
{

// Outline points closer than this, in metres, are one point.
constexpr float kMinOutlineEdgeLength = 1.0e-4f;

// An outline enclosing less than this, in square metres, holds no fog.
constexpr float kMinOutlineArea = 1.0e-6f;

// Below this length the two edges at a corner fold back on each other, and the
// line between their bands is taken square to the edge instead.
constexpr float kMinMiterLength = 1.0e-3f;

// How far, in metres, an outline's box reaches past the outline sideways. The
// outline touches its bounds, and a line entering the box exactly where it does
// would start on the outline, where inside and outside are a coin toss.
constexpr float kOutlineBoundsMargin = 1.0e-3f;

// A box corner this close to the eye's plane, or behind it, has no place on
// screen, and the box may then cover any part of it.
constexpr float kMinTileClipW = 1.0e-4f;

float Cross(const glm::vec2 &a, const glm::vec2 &b)
{
    return a.x * b.y - a.y * b.x;
}

// Two-point Gauss-Legendre: a step's integral from its function at 1/sqrt(3) of
// the way from its middle to each end. Exact for a cubic, which every piece of
// fog without falloff is: a product of at most three straight ramps.
constexpr float kGaussNode = 0.57735027f;

// How many times the falloff may thin the fog across one step of the rule.
// Two keeps it within half a percent of the exact integral.
constexpr float kMaxFalloffPerStep = 2.0f;

// The most steps one piece is cut into. A piece that would need more has
// thinned to almost nothing across most of its length.
constexpr float kMaxStepsPerPiece = 8.0f;

/// @brief The shape a volume's fog fills, in its own frame.
struct FogShape
{
    /// Half extents, or an ellipsoid's semi-axes.
    glm::vec3 extent{0.0f};
    float softness = 0.0f;
    /// Zero for fog as thick at the top as at the floor.
    float falloff = 0.0f;
};

/// @brief Which formula the fog along a stretch follows.
enum class Density : std::uint8_t
{
    /// Each axis's softness ramp, multiplied.
    Box,
    /// A soft shell around a full core.
    Ellipsoid,
    /// An outline's floor and top softness alone.
    Layers,
    /// Layers, thinned toward one outline wall across its softness band.
    Wall,
};

/// @brief What the fog at a point along a stretch depends on.
struct DensityContext
{
    glm::vec3 start{0.0f};
    glm::vec3 ray{0.0f};
    FogShape shape;
    /// Wall: where the wall's edge starts, and its normal pointing inside.
    glm::vec2 wallFrom{0.0f};
    glm::vec2 wallNormal{0.0f};
    /// Ellipsoid: how deep the soft shell is, in units of the radius squared.
    float band = 0.0f;
    Density kind = Density::Box;
};

// The fog across a slab @p span thick, at @p depth into it: full in the middle,
// falling straight to nothing over @p soft at each face.
float Ramp(float depth, float span, float soft)
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

// How thick the falloff leaves the fog @p height above the floor.
float FalloffAt(float height, float falloff)
{
    return falloff > 0.0f ? std::exp(-std::max(height, 0.0f) / falloff) : 1.0f;
}

// The fog @p t along a stretch, as a share of full.
float DensityAt(const DensityContext &context, float t)
{
    const glm::vec3 p = context.start + context.ray * t;
    const glm::vec3 &extent = context.shape.extent;
    const float softness = context.shape.softness;
    const float falloff = FalloffAt(p.y + extent.y, context.shape.falloff);
    switch (context.kind)
    {
    case Density::Box:
        return Ramp(p.x + extent.x, 2.0f * extent.x, softness) * Ramp(p.y + extent.y, 2.0f * extent.y, softness) *
               Ramp(p.z + extent.z, 2.0f * extent.z, softness) * falloff;
    case Density::Ellipsoid:
    {
        const glm::vec3 unit = p / extent;
        const float radiusSq = glm::dot(unit, unit);
        const float shell = context.band > 0.0f ? std::clamp((1.0f - radiusSq) / context.band, 0.0f, 1.0f)
                                                : (radiusSq <= 1.0f ? 1.0f : 0.0f);
        return shell * falloff;
    }
    case Density::Layers:
        return Ramp(p.y + extent.y, 2.0f * extent.y, softness) * falloff;
    case Density::Wall:
    {
        const float depth = glm::dot(context.wallNormal, glm::vec2(p.x, p.z) - context.wallFrom);
        const float wall = std::clamp(1.0f - depth / softness, 0.0f, 1.0f);
        return Ramp(p.y + extent.y, 2.0f * extent.y, softness) * falloff * wall;
    }
    }
    return 0.0f;
}

/// @brief Where along a stretch the fog's formula changes, which the
/// integration steps must not straddle: in pairs, two per axis of a box, at the
/// inner edges of each face's softness. A slot with no point holds the
/// stretch's length, where it adds no piece.
///
/// Fixed slots sorted by a fixed network rather than a list, so the shader's
/// copy reads every slot at a constant index and keeps them all in registers.
struct Breaks
{
    static constexpr std::size_t kSlots = 6;

    explicit Breaks(float length) { at.fill(length); }

    void SetPair(std::size_t pair, const glm::vec2 &points)
    {
        at[2 * pair] = points.x;
        at[2 * pair + 1] = points.y;
    }

    /// A sorting network: these twelve compare-and-swaps, in this order, sort
    /// any six values, the fewest any fixed sequence can. The pairs follow no
    /// pattern and each relies on the ones before it, so keep them as they are.
    /// The shader sorts with the same list written out, because a fixed list
    /// keeps every index constant and the slots in registers.
    void Sort()
    {
        static constexpr std::array<std::array<std::size_t, 2>, 12> kNetwork{
            {{0, 5}, {1, 3}, {2, 4}, {1, 2}, {3, 4}, {0, 3}, {2, 5}, {0, 1}, {2, 3}, {4, 5}, {1, 2}, {3, 4}}};
        for (const std::array<std::size_t, 2> &pair : kNetwork)
        {
            if (at[pair[0]] > at[pair[1]])
            {
                std::swap(at[pair[0]], at[pair[1]]);
            }
        }
    }

    std::array<float, kSlots> at{};
};

// Where a line starting @p start across a slab of half thickness @p half, and
// moving @p rate per metre across it, meets the inner edges of the slab's
// softness @p soft, within a stretch @p length long. Nearer first.
glm::vec2 FaceBreaks(float start, float rate, float half, float soft, float length)
{
    if (soft <= 0.0f || rate == 0.0f)
    {
        return glm::vec2(length);
    }
    const float inner = half - std::min(soft, half);
    const float a = (-inner - start) / rate;
    const float b = (inner - start) / rate;
    return glm::clamp(glm::vec2(std::min(a, b), std::max(a, b)), glm::vec2(0.0f), glm::vec2(length));
}

// The two-point rule over [from, to], in steps short enough for the falloff.
float IntegratePiece(const DensityContext &context, float from, float to)
{
    const float thinning =
        context.shape.falloff > 0.0f ? std::abs(context.ray.y) * (to - from) / context.shape.falloff : 0.0f;
    const float steps = std::clamp(std::ceil(thinning / kMaxFalloffPerStep), 1.0f, kMaxStepsPerPiece);
    const float step = (to - from) / steps;
    const float offset = 0.5f * step * kGaussNode;
    float sum = 0.0f;
    for (float index = 0.0f; index < steps; index += 1.0f)
    {
        const float middle = from + (index + 0.5f) * step;
        sum += DensityAt(context, middle - offset) + DensityAt(context, middle + offset);
    }
    return sum * 0.5f * step;
}

// The fog's integral from @p from to @p to along the stretch, piece by piece
// between @p breaks, which must be sorted.
float Integrate(const DensityContext &context, const Breaks &breaks, float from, float to)
{
    float sum = 0.0f;
    float at = from;
    for (const float point : breaks.at)
    {
        const float next = std::clamp(point, from, to);
        if (next > at)
        {
            sum += IntegratePiece(context, at, next);
            at = next;
        }
    }
    if (to > at)
    {
        sum += IntegratePiece(context, at, to);
    }
    return sum;
}

// Narrows [low, high] to where c + b u >= 0.
void KeepWhereNonNegative(float c, float b, float &low, float &high)
{
    if (b > 0.0f)
    {
        low = std::max(low, -c / b);
    }
    else if (b < 0.0f)
    {
        high = std::min(high, -c / b);
    }
    else if (c < 0.0f)
    {
        high = low;
    }
}

// Where the line from @p origin along @p ray is inside the box of half extents
// @p half, or false when it misses.
bool BoxChord(const glm::vec3 &origin, const glm::vec3 &ray, const glm::vec3 &half, float &enter, float &exit)
{
    enter = -std::numeric_limits<float>::infinity();
    exit = std::numeric_limits<float>::infinity();
    for (glm::length_t axis = 0; axis < 3; ++axis)
    {
        if (ray[axis] == 0.0f)
        {
            if (std::abs(origin[axis]) > half[axis])
            {
                return false;
            }
            continue;
        }
        const float inverse = 1.0f / ray[axis];
        const float a = (-half[axis] - origin[axis]) * inverse;
        const float b = (half[axis] - origin[axis]) * inverse;
        enter = std::max(enter, std::min(a, b));
        exit = std::min(exit, std::max(a, b));
    }
    return exit > enter;
}

// The part of a line inside a volume's box and in front of the surface: it
// starts at @p start, `entry` along the line, and runs @p length further.
struct BoxStretch
{
    glm::vec3 start{0.0f};
    float entry = 0.0f;
    float length = 0.0f;
};

bool ClippedBoxStretch(const glm::vec3 &origin, const glm::vec3 &ray, const glm::vec3 &half, float distance,
                       BoxStretch &stretch)
{
    float enter = 0.0f;
    float exit = 0.0f;
    if (!BoxChord(origin, ray, half, enter, exit))
    {
        return false;
    }
    const float from = std::max(enter, 0.0f);
    const float to = std::min(exit, distance);
    if (!(to > from))
    {
        return false;
    }
    // Measured from where the stretch starts rather than from the eye, so the
    // softness below works with lengths the size of the volume and not with the
    // difference of two distances the size of the world.
    stretch = BoxStretch{.start = origin + ray * from, .entry = from, .length = to - from};
    return true;
}

FogSpan BoxSpan(const glm::vec3 &origin, const glm::vec3 &ray, const FogShape &shape, float distance)
{
    BoxStretch stretch;
    if (!ClippedBoxStretch(origin, ray, shape.extent, distance, stretch))
    {
        return FogSpan{};
    }
    Breaks breaks(stretch.length);
    for (glm::length_t axis = 0; axis < 3; ++axis)
    {
        breaks.SetPair(static_cast<std::size_t>(axis), FaceBreaks(stretch.start[axis], ray[axis], shape.extent[axis],
                                                                  shape.softness, stretch.length));
    }
    breaks.Sort();
    const DensityContext context{.start = stretch.start, .ray = ray, .shape = shape, .kind = Density::Box};
    return FogSpan{.length = Integrate(context, breaks, 0.0f, stretch.length), .entry = stretch.entry};
}

FogSpan EllipsoidSpan(const glm::vec3 &origin, const glm::vec3 &ray, const FogShape &shape, float distance)
{
    const glm::vec3 &radii = shape.extent;
    const float smallest = std::min({radii.x, radii.y, radii.z});
    if (!(smallest > 0.0f))
    {
        return FogSpan{};
    }
    // In units where the ellipsoid is a unit sphere, the line is unit + step * t.
    const glm::vec3 unit = origin / radii;
    const glm::vec3 step = ray / radii;
    const float stretch = glm::dot(step, step);
    // From the nearest approach rather than the textbook quadratic, which loses
    // every digit of a far volume to cancellation.
    const float closestAt = -glm::dot(unit, step) / stretch;
    const glm::vec3 closest = unit + step * closestAt;
    const float closestSq = glm::dot(closest, closest);
    if (closestSq >= 1.0f)
    {
        return FogSpan{};
    }
    const float halfChord = std::sqrt((1.0f - closestSq) / stretch);
    const float from = std::max(closestAt - halfChord, 0.0f);
    const float to = std::min(closestAt + halfChord, distance);
    if (!(to > from))
    {
        return FogSpan{};
    }

    // Full fog inside the sphere of radius squared 1 - band, thinning as
    // (1 - r^2) / band across the shell outside it. The shell is about the
    // softness deep where the ellipsoid is thinnest.
    const float depth = std::min(shape.softness / smallest, 1.0f);
    const float band = depth * (2.0f - depth);
    Breaks breaks(to - from);
    const float core = 1.0f - band;
    if (band > 0.0f && closestSq < core)
    {
        // In order already, so no sorting.
        const float coreHalf = std::sqrt((core - closestSq) / stretch);
        breaks.SetPair(0, glm::clamp(glm::vec2(closestAt - coreHalf, closestAt + coreHalf) - from, glm::vec2(0.0f),
                                     glm::vec2(to - from)));
    }
    const DensityContext context{
        .start = origin + ray * from, .ray = ray, .shape = shape, .band = band, .kind = Density::Ellipsoid};
    return FogSpan{.length = Integrate(context, breaks, 0.0f, to - from), .entry = from};
}

glm::vec2 EdgeStart(const FogOutlineEdgeGpu &edge)
{
    return glm::vec2(edge.points.x, edge.points.y);
}

glm::vec2 EdgeEnd(const FogOutlineEdgeGpu &edge)
{
    return glm::vec2(edge.points.z, edge.points.w);
}

// Whether the edge from @p a to @p b crosses the ray from @p point toward +x.
// An odd number of such edges puts the point inside the outline.
bool EdgeFlipsInside(const glm::vec2 &a, const glm::vec2 &b, const glm::vec2 &point)
{
    return (a.y > point.y) != (b.y > point.y) && point.x < a.x + (point.y - a.y) * (b.x - a.x) / (b.y - a.y);
}

/// @brief Where along a stretch it is inside one wall's softness band.
struct WallBand
{
    float from = 0.0f;
    float to = 0.0f;
};

// The part of the stretch from @p start along @p ray, @p length long, within
// @p soft inside @p edge's wall and between the lines bisecting its corners.
WallBand WallBandOf(const FogOutlineEdgeGpu &edge, const glm::vec2 &start, const glm::vec2 &ray, float soft,
                    float length)
{
    const glm::vec2 from = EdgeStart(edge);
    const glm::vec2 to = EdgeEnd(edge);
    const glm::vec2 normal(edge.normal.x, edge.normal.y);
    const glm::vec2 startMiter(edge.miters.x, edge.miters.y);
    const glm::vec2 endMiter(edge.miters.z, edge.miters.w);

    const float depth = glm::dot(normal, start - from);
    const float rate = glm::dot(normal, ray);
    WallBand band{.from = 0.0f, .to = length};
    KeepWhereNonNegative(depth, rate, band.from, band.to);
    KeepWhereNonNegative(soft - depth, -rate, band.from, band.to);
    KeepWhereNonNegative(glm::dot(startMiter, start - from), glm::dot(startMiter, ray), band.from, band.to);
    KeepWhereNonNegative(-glm::dot(endMiter, start - to), -glm::dot(endMiter, ray), band.from, band.to);
    return band;
}

FogSpan OutlineSpan(const glm::vec3 &origin, const glm::vec3 &ray, const FogShape &shape, float distance,
                    std::span<const FogOutlineEdgeGpu> edges)
{
    BoxStretch stretch;
    const glm::vec3 bounds = shape.extent + glm::vec3(kOutlineBoundsMargin, 0.0f, kOutlineBoundsMargin);
    if (edges.empty() || !ClippedBoxStretch(origin, ray, bounds, distance, stretch))
    {
        return FogSpan{};
    }
    const float length = stretch.length;
    // One pair, in order already, and every other slot at the end: no sorting.
    Breaks breaks(length);
    breaks.SetPair(0, FaceBreaks(stretch.start.y, ray.y, shape.extent.y, shape.softness, length));
    const DensityContext layers{.start = stretch.start, .ray = ray, .shape = shape, .kind = Density::Layers};
    const glm::vec2 start(stretch.start.x, stretch.start.z);
    const glm::vec2 flat(ray.x, ray.z);

    // The fog is the outline's inside, thinned near each wall, times the floor's
    // and the top's softness and the falloff. The inside is where the stretch
    // starts, then in and out at each wall it crosses; each crossing counts
    // from where it is to the end, so the crossings need no sorting. Each wall's
    // band then takes back what its softness thins. One pass over the edges does
    // all three, so the shader fetches each edge once.
    const float whole = Integrate(layers, breaks, 0.0f, length);
    bool inside = false;
    float fog = 0.0f;
    for (const FogOutlineEdgeGpu &edge : edges)
    {
        const glm::vec2 from = EdgeStart(edge);
        const glm::vec2 to = EdgeEnd(edge);
        if (EdgeFlipsInside(from, to, start))
        {
            inside = !inside;
        }
        // Crossed when its ends lie on opposite sides of the line, a corner on
        // the line counting as on the right. A line through a corner then
        // crosses one of its two edges, and a line grazing a corner crosses
        // both or neither, so the in and the out cancel.
        if ((Cross(flat, from - start) > 0.0f) != (Cross(flat, to - start) > 0.0f))
        {
            const glm::vec2 along = to - from;
            const float denominator = Cross(flat, along);
            const float at = Cross(from - start, along) / denominator;
            if (at > 0.0f && at < length)
            {
                // The inside is on the edge's left, which a line crossing it
                // enters when its direction has the edge on its right.
                const float rest = Integrate(layers, breaks, at, length);
                fog += denominator < 0.0f ? rest : -rest;
            }
        }
        if (shape.softness > 0.0f)
        {
            const WallBand band = WallBandOf(edge, start, flat, shape.softness, length);
            if (band.to > band.from)
            {
                DensityContext wall = layers;
                wall.kind = Density::Wall;
                wall.wallFrom = from;
                wall.wallNormal = glm::vec2(edge.normal.x, edge.normal.y);
                fog -= Integrate(wall, breaks, band.from, band.to);
            }
        }
    }
    fog += inside ? whole : 0.0f;
    return FogSpan{.length = std::clamp(fog, 0.0f, length), .entry = stretch.entry};
}

glm::vec2 UnitOr(const glm::vec2 &v, const glm::vec2 &fallback)
{
    const float length = glm::length(v);
    return length >= kMinMiterLength ? v / length : fallback;
}

// The square @p pixel falls in, of @p count squares @p tilePixels wide.
uint32_t TileIndex(float pixel, uint32_t tilePixels, uint32_t count)
{
    const float index = std::floor(pixel / static_cast<float>(tilePixels));
    return static_cast<uint32_t>(std::clamp(index, 0.0f, static_cast<float>(count - 1u)));
}

} // namespace

float SanitizedFogVolumeFalloff(float falloff)
{
    if (!std::isfinite(falloff) || falloff <= 0.0f)
    {
        return 0.0f;
    }
    return std::clamp(falloff, kMinFogVolumeFalloff, kMaxFogVolumeFalloff);
}

FogVolumeGpu MakeFogVolume(const FogVolumeDesc &desc)
{
    FogVolumeGpu volume;
    const glm::vec3 centre(desc.frame[3]);
    for (glm::length_t row = 0; row < 3; ++row)
    {
        const glm::vec3 axis(desc.frame[row]);
        volume.toFrame[static_cast<std::size_t>(row)] = glm::vec4(axis, -glm::dot(axis, centre));
    }
    const float density = std::isfinite(desc.visibility) ? FogDensityFor(desc.visibility) : 0.0f;
    const float softness = std::isfinite(desc.edgeSoftness) ? std::max(desc.edgeSoftness, 0.0f) : 0.0f;
    volume.extents = glm::vec4(glm::max(desc.halfExtents, glm::vec3(0.0f)), softness);
    volume.color = glm::vec4(glm::max(desc.color, glm::vec3(0.0f)), density);
    volume.profile = glm::vec4(SanitizedFogVolumeFalloff(desc.falloff), 0.0f, 0.0f, 0.0f);
    volume.info = glm::uvec4(static_cast<uint32_t>(desc.kind), desc.litBySky ? 1u : 0u, desc.firstEdge, desc.edgeCount);
    return volume;
}

std::optional<FogOutlinePlacement> AppendFogOutline(std::vector<FogOutlineEdgeGpu> &edges,
                                                    std::span<const glm::vec2> points)
{
    std::vector<glm::vec2> outline;
    outline.reserve(points.size());
    for (const glm::vec2 &point : points)
    {
        if (outline.empty() || glm::distance(outline.back(), point) >= kMinOutlineEdgeLength)
        {
            outline.push_back(point);
        }
    }
    while (outline.size() > 1 && glm::distance(outline.back(), outline.front()) < kMinOutlineEdgeLength)
    {
        outline.pop_back();
    }
    if (outline.size() < 3)
    {
        return std::nullopt;
    }

    float twiceArea = 0.0f;
    glm::vec2 low = outline.front();
    glm::vec2 high = outline.front();
    for (std::size_t i = 0; i < outline.size(); ++i)
    {
        twiceArea += Cross(outline[i], outline[(i + 1) % outline.size()]);
        low = glm::min(low, outline[i]);
        high = glm::max(high, outline[i]);
    }
    if (std::abs(twiceArea) * 0.5f < kMinOutlineArea)
    {
        return std::nullopt;
    }
    if (twiceArea < 0.0f)
    {
        std::reverse(outline.begin(), outline.end());
    }
    const std::size_t count = outline.size();
    if (edges.size() + count > kMaxFogOutlineEdges)
    {
        return std::nullopt;
    }

    const FogOutlinePlacement placement{.centre = (low + high) * 0.5f,
                                        .halfExtents = (high - low) * 0.5f,
                                        .firstEdge = static_cast<uint32_t>(edges.size()),
                                        .edgeCount = static_cast<uint32_t>(count)};
    for (std::size_t i = 0; i < count; ++i)
    {
        const glm::vec2 previous = outline[(i + count - 1) % count];
        const glm::vec2 from = outline[i];
        const glm::vec2 to = outline[(i + 1) % count];
        const glm::vec2 next = outline[(i + 2) % count];

        const glm::vec2 along = glm::normalize(to - from);
        const glm::vec2 before = glm::normalize(from - previous);
        const glm::vec2 after = glm::normalize(next - to);
        const glm::vec2 startMiter = UnitOr(before + along, along);
        const glm::vec2 endMiter = UnitOr(along + after, along);

        const glm::vec2 a = from - placement.centre;
        const glm::vec2 b = to - placement.centre;
        edges.push_back(FogOutlineEdgeGpu{.points = glm::vec4(a, b),
                                          .normal = glm::vec4(-along.y, along.x, 0.0f, 0.0f),
                                          .miters = glm::vec4(startMiter, endMiter)});
    }
    return placement;
}

FogSpan FogVolumeSpan(const FogVolumeGpu &volume, std::span<const FogOutlineEdgeGpu> edges, const glm::vec3 &eye,
                      const glm::vec3 &ray, float distance)
{
    glm::vec3 origin(0.0f);
    glm::vec3 direction(0.0f);
    for (glm::length_t row = 0; row < 3; ++row)
    {
        const glm::vec4 &toFrame = volume.toFrame[static_cast<std::size_t>(row)];
        origin[row] = glm::dot(glm::vec3(toFrame), eye) + toFrame.w;
        direction[row] = glm::dot(glm::vec3(toFrame), ray);
    }
    const FogShape shape{
        .extent = glm::vec3(volume.extents), .softness = volume.extents.w, .falloff = volume.profile.x};
    switch (static_cast<FogVolumeKind>(volume.info.x))
    {
    case FogVolumeKind::Box:
        return BoxSpan(origin, direction, shape, distance);
    case FogVolumeKind::Ellipsoid:
        return EllipsoidSpan(origin, direction, shape, distance);
    case FogVolumeKind::Outline:
    {
        const std::size_t first = volume.info.z;
        const std::size_t count = volume.info.w;
        if (first + count > edges.size())
        {
            return FogSpan{};
        }
        return OutlineSpan(origin, direction, shape, distance, edges.subspan(first, count));
    }
    case FogVolumeKind::Count_:
        break;
    }
    return FogSpan{};
}

glm::vec3 FogVolumeColor(const FogVolumeGpu &volume, const glm::vec3 &haze, float hazeOpacity)
{
    const glm::vec3 own(volume.color);
    const glm::vec3 shown = volume.info.y != 0u ? own * haze : own;
    return glm::mix(shown, haze, hazeOpacity);
}

void FogVolumeBlend::Add(float opticalDepth, const glm::vec3 &color)
{
    if (opticalDepth > 0.0f)
    {
        depth += opticalDepth;
        weighted += color * opticalDepth;
    }
}

glm::vec3 FogVolumeBlend::Over(const glm::vec3 &behind) const
{
    if (depth <= 0.0f)
    {
        return behind;
    }
    const float through = std::exp(-depth);
    return behind * through + weighted / depth * (1.0f - through);
}

FogTileGrid FogTileGridFor(uint32_t width, uint32_t height)
{
    FogTileGrid grid{.width = width, .height = height};
    for (;;)
    {
        grid.columns = (width + grid.tilePixels - 1u) / grid.tilePixels;
        grid.rows = (height + grid.tilePixels - 1u) / grid.tilePixels;
        if (grid.columns * grid.rows <= kMaxFogTiles)
        {
            return grid;
        }
        grid.tilePixels *= 2u;
    }
}

std::optional<FogTileRect> FogVolumeTileRect(const glm::mat4 &viewProjection, const glm::mat4 &frame,
                                             const glm::vec3 &halfExtents, const FogTileGrid &grid)
{
    if (grid.columns == 0u || grid.rows == 0u)
    {
        return std::nullopt;
    }
    const glm::mat4 toClip = viewProjection * frame;
    // Whether every corner is outside one plane: past the left, right, bottom
    // or top edge of the view, or behind the eye.
    bool allLeft = true;
    bool allRight = true;
    bool allBelow = true;
    bool allAbove = true;
    bool allBehind = true;
    bool anyBehind = false;
    glm::vec2 low(std::numeric_limits<float>::max());
    glm::vec2 high(std::numeric_limits<float>::lowest());
    constexpr int32_t kCorners = 8;
    for (int32_t corner = 0; corner < kCorners; ++corner)
    {
        const glm::vec3 sign((corner & 1) != 0 ? 1.0f : -1.0f, (corner & 2) != 0 ? 1.0f : -1.0f,
                             (corner & 4) != 0 ? 1.0f : -1.0f);
        const glm::vec4 clip = toClip * glm::vec4(sign * halfExtents, 1.0f);
        allLeft = allLeft && clip.x < -clip.w;
        allRight = allRight && clip.x > clip.w;
        allBelow = allBelow && clip.y < -clip.w;
        allAbove = allAbove && clip.y > clip.w;
        const bool behind = clip.w <= kMinTileClipW;
        allBehind = allBehind && behind;
        anyBehind = anyBehind || behind;
        if (!behind)
        {
            const glm::vec2 ndc = glm::vec2(clip) / clip.w;
            // The top of the screen is +1 in y.
            const glm::vec2 pixel((ndc.x * 0.5f + 0.5f) * static_cast<float>(grid.width),
                                  (0.5f - ndc.y * 0.5f) * static_cast<float>(grid.height));
            low = glm::min(low, pixel);
            high = glm::max(high, pixel);
        }
    }
    if (allLeft || allRight || allBelow || allAbove || allBehind)
    {
        return std::nullopt;
    }
    const FogTileRect everything{.minColumn = 0u, .minRow = 0u, .maxColumn = grid.columns - 1u, .maxRow = grid.rows - 1u};
    if (anyBehind)
    {
        return everything;
    }
    return FogTileRect{.minColumn = TileIndex(low.x, grid.tilePixels, grid.columns),
                       .minRow = TileIndex(low.y, grid.tilePixels, grid.rows),
                       .maxColumn = TileIndex(high.x, grid.tilePixels, grid.columns),
                       .maxRow = TileIndex(high.y, grid.tilePixels, grid.rows)};
}

void BuildFogTileLists(const FogTileGrid &grid, std::span<const FogTileRect> rects, std::vector<uint32_t> &out)
{
    const uint32_t tiles = grid.columns * grid.rows;
    const uint32_t header = tiles * 2u;
    std::vector<uint32_t> listed(tiles, 0u);
    for (const FogTileRect &rect : rects)
    {
        for (uint32_t row = rect.minRow; row <= rect.maxRow; ++row)
        {
            for (uint32_t column = rect.minColumn; column <= rect.maxColumn; ++column)
            {
                ++listed[row * grid.columns + column];
            }
        }
    }

    out.assign(header, 0u);
    uint32_t next = header;
    uint32_t room = kMaxFogTileEntries;
    for (uint32_t tile = 0; tile < tiles; ++tile)
    {
        const uint32_t count = std::min(listed[tile], room);
        room -= count;
        out[tile * 2u] = next;
        out[tile * 2u + 1u] = count;
        next += count;
    }
    out.resize(next, 0u);

    // Counted again from zero as each square fills, up to the room it was given.
    std::fill(listed.begin(), listed.end(), 0u);
    for (std::size_t index = 0; index < rects.size(); ++index)
    {
        const FogTileRect &rect = rects[index];
        for (uint32_t row = rect.minRow; row <= rect.maxRow; ++row)
        {
            for (uint32_t column = rect.minColumn; column <= rect.maxColumn; ++column)
            {
                const uint32_t tile = row * grid.columns + column;
                if (listed[tile] < out[tile * 2u + 1u])
                {
                    out[out[tile * 2u] + listed[tile]] = static_cast<uint32_t>(index);
                    ++listed[tile];
                }
            }
        }
    }
}

void FogVolumeFrame::Clear()
{
    volumes.clear();
    edges.clear();
    tiles.clear();
    grid = FogTileGrid{};
}

glm::uvec4 FogVolumeFrame::Lanes() const
{
    if (volumes.empty() || tiles.empty())
    {
        return glm::uvec4(0u);
    }
    return glm::uvec4(static_cast<uint32_t>(volumes.size()), grid.columns, grid.rows, grid.tilePixels);
}

} // namespace Assisi::Render
