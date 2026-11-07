/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/FogVolume.hpp>

#include <Assisi/Render/Fog.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

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

// The integral of max(0, a + b u) over u from p to q.
float PositivePartIntegral(float a, float b, float p, float q)
{
    float low = p;
    float high = q;
    if (b > 0.0f)
    {
        low = std::max(low, -a / b);
    }
    else if (b < 0.0f)
    {
        high = std::min(high, -a / b);
    }
    else if (a <= 0.0f)
    {
        return 0.0f;
    }
    if (!(high > low))
    {
        return 0.0f;
    }
    return (high - low) * (a + b * 0.5f * (low + high));
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

// How much a face's softness thins a stretch: the integral of (1 - d / soft)
// where d, the depth inside the face, is below @p soft. The depth at the start
// is @p depth, and it changes by @p rate per metre.
float FaceThinning(float depth, float rate, float soft, float length)
{
    return PositivePartIntegral(1.0f - depth / soft, -rate / soft, 0.0f, length);
}

FogSpan BoxSpan(const glm::vec3 &origin, const glm::vec3 &ray, const glm::vec3 &half, float softness, float distance)
{
    BoxStretch stretch;
    if (!ClippedBoxStretch(origin, ray, half, distance, stretch))
    {
        return FogSpan{};
    }
    float thinning = 0.0f;
    for (glm::length_t axis = 0; axis < 3; ++axis)
    {
        const float soft = std::min(softness, half[axis]);
        if (soft <= 0.0f)
        {
            continue;
        }
        thinning += FaceThinning(half[axis] - stretch.start[axis], -ray[axis], soft, stretch.length);
        thinning += FaceThinning(half[axis] + stretch.start[axis], ray[axis], soft, stretch.length);
    }
    return FogSpan{.length = std::max(stretch.length - thinning, 0.0f), .entry = stretch.entry};
}

// The integral, from @p from to @p to along the line, of (level - r^2), r being
// the distance from the centre in units of the semi-axes. @p closest is the
// line's nearest approach to the centre in those units, at @p closestAt along it,
// and @p stretch is how fast those units pass per metre, squared.
float RadialIntegral(float level, float closestSq, float closestAt, float stretch, float from, float to)
{
    const float a = from - closestAt;
    const float b = to - closestAt;
    return (level - closestSq) * (to - from) - stretch * (b * b * b - a * a * a) / 3.0f;
}

FogSpan EllipsoidSpan(const glm::vec3 &origin, const glm::vec3 &ray, const glm::vec3 &radii, float softness,
                      float distance)
{
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
    const float depth = std::min(softness / smallest, 1.0f);
    const float band = depth * (2.0f - depth);
    if (band <= 0.0f)
    {
        return FogSpan{.length = to - from, .entry = from};
    }
    float soft = RadialIntegral(1.0f, closestSq, closestAt, stretch, from, to);
    const float core = 1.0f - band;
    if (closestSq < core)
    {
        const float coreHalf = std::sqrt((core - closestSq) / stretch);
        const float coreFrom = std::max(closestAt - coreHalf, 0.0f);
        const float coreTo = std::min(closestAt + coreHalf, distance);
        if (coreTo > coreFrom)
        {
            soft -= RadialIntegral(core, closestSq, closestAt, stretch, coreFrom, coreTo);
        }
    }
    return FogSpan{.length = std::max(soft / band, 0.0f), .entry = from};
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

// The fog an outline stretch would have up to @p at, counting only the floor's
// and the top's softness: the stretch's own length, less what they thin away.
struct LayerSoftness
{
    glm::vec3 start{0.0f};
    glm::vec3 ray{0.0f};
    float halfHeight = 0.0f;
    float soft = 0.0f;

    [[nodiscard]] float Upto(float at) const
    {
        if (soft <= 0.0f)
        {
            return at;
        }
        return at - FaceThinning(halfHeight + start.y, ray.y, soft, at) -
               FaceThinning(halfHeight - start.y, -ray.y, soft, at);
    }
};

// How much a wall's softness thins the stretch: the integral of (1 - d / soft)
// over the part of it inside the edge's band, d being the depth inside the wall.
float WallThinning(const FogOutlineEdgeGpu &edge, const glm::vec2 &start, const glm::vec2 &ray, float soft,
                   float length)
{
    const glm::vec2 from = EdgeStart(edge);
    const glm::vec2 to = EdgeEnd(edge);
    const glm::vec2 normal(edge.normal.x, edge.normal.y);
    const glm::vec2 startMiter(edge.miters.x, edge.miters.y);
    const glm::vec2 endMiter(edge.miters.z, edge.miters.w);

    const float depth = glm::dot(normal, start - from);
    const float rate = glm::dot(normal, ray);
    float low = 0.0f;
    float high = length;
    KeepWhereNonNegative(depth, rate, low, high);
    KeepWhereNonNegative(soft - depth, -rate, low, high);
    KeepWhereNonNegative(glm::dot(startMiter, start - from), glm::dot(startMiter, ray), low, high);
    KeepWhereNonNegative(-glm::dot(endMiter, start - to), -glm::dot(endMiter, ray), low, high);
    if (!(high > low))
    {
        return 0.0f;
    }
    return (high - low) * (1.0f - (depth + rate * 0.5f * (low + high)) / soft);
}

FogSpan OutlineSpan(const glm::vec3 &origin, const glm::vec3 &ray, const glm::vec3 &half, float softness,
                    float distance, std::span<const FogOutlineEdgeGpu> edges)
{
    BoxStretch stretch;
    const glm::vec3 bounds = half + glm::vec3(kOutlineBoundsMargin, 0.0f, kOutlineBoundsMargin);
    if (edges.empty() || !ClippedBoxStretch(origin, ray, bounds, distance, stretch))
    {
        return FogSpan{};
    }
    const LayerSoftness layers{.start = stretch.start,
                               .ray = ray,
                               .halfHeight = half.y,
                               .soft = std::min(softness, half.y)};
    const glm::vec2 start(stretch.start.x, stretch.start.z);
    const glm::vec2 flat(ray.x, ray.z);
    const float length = stretch.length;

    // Inside where the stretch starts, then in and out at each wall it crosses.
    // Each crossing counts from where it is to the end, so the crossings need no
    // sorting. One pass over the edges does all three, so the shader fetches
    // each edge once.
    const float whole = layers.Upto(length);
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
                const float rest = whole - layers.Upto(at);
                fog += denominator < 0.0f ? rest : -rest;
            }
        }
        if (softness > 0.0f)
        {
            fog -= WallThinning(edge, start, flat, softness, length);
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
    const glm::vec3 half(volume.extents);
    const float softness = volume.extents.w;
    switch (static_cast<FogVolumeKind>(volume.info.x))
    {
    case FogVolumeKind::Box:
        return BoxSpan(origin, direction, half, softness, distance);
    case FogVolumeKind::Ellipsoid:
        return EllipsoidSpan(origin, direction, half, softness, distance);
    case FogVolumeKind::Outline:
    {
        const std::size_t first = volume.info.z;
        const std::size_t count = volume.info.w;
        if (first + count > edges.size())
        {
            return FogSpan{};
        }
        return OutlineSpan(origin, direction, half, softness, distance, edges.subspan(first, count));
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
