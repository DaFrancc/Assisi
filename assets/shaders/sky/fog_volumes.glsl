#ifndef ASSISI_SKY_FOG_VOLUMES_GLSL
#define ASSISI_SKY_FOG_VOLUMES_GLSL

// Placed fog volumes: a transcription of Render::FogVolumeSpan, FogVolumeColor
// and FogVolumeBlend in FogVolume.hpp, which is the specification.
//
// The including shader declares nothing at bindings 13 to 15: the volumes, their
// outline edges and the screen squares' lists of them live there in both the
// mesh pass and the sky pass.

#include "fog.glsl"

// Mirrors Render::FogVolumeGpu; the member order is the layout.
struct FogVolume
{
    // Rows of the transform into the volume's frame: coordinate i of a point is
    // dot(xyz, point) + w.
    vec4  toFrame[3];
    vec4  extents; // xyz = half extents in metres, w = edge softness in metres
    vec4  color;   // xyz = linear colour, w = optical depth per metre where full
    uvec4 info;    // x = kind, y = 1 while lit by the sky, z = first edge, w = edge count
};

// Mirrors Render::FogOutlineEdgeGpu.
struct FogOutlineEdge
{
    vec4 points; // xy = start, zw = end, in the frame's x-z
    vec4 normal; // xy = unit normal pointing inside
    vec4 miters; // xy = start corner's bisector normal, zw = the end's
};

layout(std430, binding = 13) readonly buffer FogVolumes      { FogVolume      fogVolumes[];      };
layout(std430, binding = 14) readonly buffer FogOutlineEdges { FogOutlineEdge fogOutlineEdges[]; };
// An offset and a count per screen square, row by row, then the volume indices
// the offsets point at.
layout(std430, binding = 15) readonly buffer FogTiles        { uint           fogTiles[];        };

// Must match Render::FogVolumeKind.
const uint kFogVolumeBox       = 0u;
const uint kFogVolumeEllipsoid = 1u;
const uint kFogVolumeOutline   = 2u;

// Must match FogVolume.cpp's kOutlineBoundsMargin.
const float kFogOutlineBoundsMargin = 1.0e-3;

// What FogVolumeSpan returns: metres of full fog, and where the box is reached.
// Render::FogSpan's `length` is `fullMetres` here, because GLSL reads `.length`
// as the array method.
struct FogSpan
{
    float fullMetres;
    float entry;
};

float FogCross(vec2 a, vec2 b)
{
    return a.x * b.y - a.y * b.x;
}

// The integral of max(0, a + b u) over u from p to q.
float FogPositivePart(float a, float b, float p, float q)
{
    float low  = p;
    float high = q;
    if (b > 0.0)
    {
        low = max(low, -a / b);
    }
    else if (b < 0.0)
    {
        high = min(high, -a / b);
    }
    else if (a <= 0.0)
    {
        return 0.0;
    }
    if (!(high > low))
    {
        return 0.0;
    }
    return (high - low) * (a + b * 0.5 * (low + high));
}

// Narrows [low, high] to where c + b u >= 0.
void FogKeepNonNegative(float c, float b, inout float low, inout float high)
{
    if (b > 0.0)
    {
        low = max(low, -c / b);
    }
    else if (b < 0.0)
    {
        high = min(high, -c / b);
    }
    else if (c < 0.0)
    {
        high = low;
    }
}

// The face softness's thinning over a stretch: see FaceThinning.
float FogFaceThinning(float depth, float rate, float soft, float len)
{
    return FogPositivePart(1.0 - depth / soft, -rate / soft, 0.0, len);
}

// The part of the line inside the box of half extents `extent` and in front of
// `distance`: false when there is none. See ClippedBoxStretch.
bool FogBoxStretch(vec3 origin, vec3 ray, vec3 extent, float distance, out vec3 start, out float entry,
                   out float len)
{
    start = vec3(0.0);
    entry = 0.0;
    len   = 0.0;
    float enter = -3.4e38;
    float leave = 3.4e38;
    for (int axis = 0; axis < 3; axis++)
    {
        if (ray[axis] == 0.0)
        {
            if (abs(origin[axis]) > extent[axis])
            {
                return false;
            }
            continue;
        }
        float inverse = 1.0 / ray[axis];
        float a = (-extent[axis] - origin[axis]) * inverse;
        float b = (extent[axis] - origin[axis]) * inverse;
        enter = max(enter, min(a, b));
        leave = min(leave, max(a, b));
    }
    float from = max(enter, 0.0);
    float to   = min(leave, distance);
    if (!(leave > enter) || !(to > from))
    {
        return false;
    }
    start = origin + ray * from;
    entry = from;
    len   = to - from;
    return true;
}

FogSpan FogBoxSpan(vec3 origin, vec3 ray, vec3 extent, float softness, float distance)
{
    vec3  start;
    float entry;
    float len;
    if (!FogBoxStretch(origin, ray, extent, distance, start, entry, len))
    {
        return FogSpan(0.0, 0.0);
    }
    float thinning = 0.0;
    for (int axis = 0; axis < 3; axis++)
    {
        float soft = min(softness, extent[axis]);
        if (soft <= 0.0)
        {
            continue;
        }
        thinning += FogFaceThinning(extent[axis] - start[axis], -ray[axis], soft, len);
        thinning += FogFaceThinning(extent[axis] + start[axis], ray[axis], soft, len);
    }
    return FogSpan(max(len - thinning, 0.0), entry);
}

// See RadialIntegral.
float FogRadialIntegral(float level, float closestSq, float closestAt, float stretch, float from, float to)
{
    float a = from - closestAt;
    float b = to - closestAt;
    return (level - closestSq) * (to - from) - stretch * (b * b * b - a * a * a) / 3.0;
}

FogSpan FogEllipsoidSpan(vec3 origin, vec3 ray, vec3 radii, float softness, float distance)
{
    float smallest = min(radii.x, min(radii.y, radii.z));
    if (!(smallest > 0.0))
    {
        return FogSpan(0.0, 0.0);
    }
    vec3  unit      = origin / radii;
    vec3  stepUnit  = ray / radii;
    float stretch   = dot(stepUnit, stepUnit);
    float closestAt = -dot(unit, stepUnit) / stretch;
    vec3  closest   = unit + stepUnit * closestAt;
    float closestSq = dot(closest, closest);
    if (closestSq >= 1.0)
    {
        return FogSpan(0.0, 0.0);
    }
    float halfChord = sqrt((1.0 - closestSq) / stretch);
    float from      = max(closestAt - halfChord, 0.0);
    float to        = min(closestAt + halfChord, distance);
    if (!(to > from))
    {
        return FogSpan(0.0, 0.0);
    }
    float depth = min(softness / smallest, 1.0);
    float band  = depth * (2.0 - depth);
    if (band <= 0.0)
    {
        return FogSpan(to - from, from);
    }
    float soft = FogRadialIntegral(1.0, closestSq, closestAt, stretch, from, to);
    float core = 1.0 - band;
    if (closestSq < core)
    {
        float coreHalf = sqrt((core - closestSq) / stretch);
        float coreFrom = max(closestAt - coreHalf, 0.0);
        float coreTo   = min(closestAt + coreHalf, distance);
        if (coreTo > coreFrom)
        {
            soft -= FogRadialIntegral(core, closestSq, closestAt, stretch, coreFrom, coreTo);
        }
    }
    return FogSpan(max(soft / band, 0.0), from);
}

// Whether the edge from `a` to `b` crosses the ray from `point` toward +x: an
// odd number of such edges puts the point inside. See EdgeFlipsInside.
bool FogEdgeFlipsInside(vec2 a, vec2 b, vec2 point)
{
    return (a.y > point.y) != (b.y > point.y) && point.x < a.x + (point.y - a.y) * (b.x - a.x) / (b.y - a.y);
}

// The stretch's own length up to `at`, less the floor's and the top's thinning:
// LayerSoftness::Upto.
float FogLayersUpto(vec3 start, vec3 ray, float halfHeight, float soft, float at)
{
    if (soft <= 0.0)
    {
        return at;
    }
    return at - FogFaceThinning(halfHeight + start.y, ray.y, soft, at) -
           FogFaceThinning(halfHeight - start.y, -ray.y, soft, at);
}

// See WallThinning.
float FogWallThinning(FogOutlineEdge edge, vec2 start, vec2 ray, float soft, float len)
{
    vec2 from = edge.points.xy;
    vec2 to   = edge.points.zw;
    vec2 n    = edge.normal.xy;

    float depth = dot(n, start - from);
    float rate  = dot(n, ray);
    float low   = 0.0;
    float high  = len;
    FogKeepNonNegative(depth, rate, low, high);
    FogKeepNonNegative(soft - depth, -rate, low, high);
    FogKeepNonNegative(dot(edge.miters.xy, start - from), dot(edge.miters.xy, ray), low, high);
    FogKeepNonNegative(-dot(edge.miters.zw, start - to), -dot(edge.miters.zw, ray), low, high);
    if (!(high > low))
    {
        return 0.0;
    }
    return (high - low) * (1.0 - (depth + rate * 0.5 * (low + high)) / soft);
}

FogSpan FogOutlineSpan(vec3 origin, vec3 ray, vec3 extent, float softness, float distance, uint first, uint count)
{
    vec3  start3;
    float entry;
    float len;
    vec3  bounds = extent + vec3(kFogOutlineBoundsMargin, 0.0, kFogOutlineBoundsMargin);
    if (count == 0u || !FogBoxStretch(origin, ray, bounds, distance, start3, entry, len))
    {
        return FogSpan(0.0, 0.0);
    }
    float layerSoft = min(softness, extent.y);
    vec2  start     = start3.xz;
    vec2  flat2     = ray.xz;

    float whole  = FogLayersUpto(start3, ray, extent.y, layerSoft, len);
    bool  inside = false;
    float fog    = 0.0;
    // One pass, each edge fetched once: OutlineSpan's even-odd test, its walls
    // crossed and its walls' softness.
    for (uint i = 0u; i < count; i++)
    {
        FogOutlineEdge edge = fogOutlineEdges[first + i];
        vec2 from = edge.points.xy;
        vec2 to   = edge.points.zw;
        if (FogEdgeFlipsInside(from, to, start))
        {
            inside = !inside;
        }
        if ((FogCross(flat2, from - start) > 0.0) != (FogCross(flat2, to - start) > 0.0))
        {
            vec2  along       = to - from;
            float denominator = FogCross(flat2, along);
            float at          = FogCross(from - start, along) / denominator;
            if (at > 0.0 && at < len)
            {
                float rest = whole - FogLayersUpto(start3, ray, extent.y, layerSoft, at);
                fog += denominator < 0.0 ? rest : -rest;
            }
        }
        if (softness > 0.0)
        {
            fog -= FogWallThinning(edge, start, flat2, softness, len);
        }
    }
    fog += inside ? whole : 0.0;
    return FogSpan(clamp(fog, 0.0, len), entry);
}

FogSpan FogVolumeSpan(FogVolume volume, vec3 eye, vec3 ray, float distance)
{
    vec3 origin;
    vec3 direction;
    for (int row = 0; row < 3; row++)
    {
        origin[row]    = dot(volume.toFrame[row].xyz, eye) + volume.toFrame[row].w;
        direction[row] = dot(volume.toFrame[row].xyz, ray);
    }
    vec3  extent   = volume.extents.xyz;
    float softness = volume.extents.w;
    uint  kind     = volume.info.x;
    if (kind == kFogVolumeBox)
    {
        return FogBoxSpan(origin, direction, extent, softness, distance);
    }
    if (kind == kFogVolumeEllipsoid)
    {
        return FogEllipsoidSpan(origin, direction, extent, softness, distance);
    }
    if (kind == kFogVolumeOutline)
    {
        return FogOutlineSpan(origin, direction, extent, softness, distance, volume.info.z, volume.info.w);
    }
    return FogSpan(0.0, 0.0);
}

// `color` seen through every volume listed for this pixel's square, along the
// unit `ray` from `eye` as far as `distance`. `volumes` is the frame's
// fogVolumes lanes (Render::FogVolumeFrame::Lanes); `fog` is the level's fog,
// whose haze colours a lit volume and covers what lies beyond it.
vec3 ApplyFogVolumes(vec3 color, uvec4 volumes, FogConstants fog, vec3 eye, vec3 ray, float distance,
                     vec2 fragCoord)
{
    if (volumes.x == 0u)
    {
        return color;
    }
    uvec2 square = min(uvec2(fragCoord) / volumes.w, volumes.yz - 1u);
    uint  tile   = square.y * volumes.y + square.x;
    uint  offset = fogTiles[tile * 2u];
    uint  count  = fogTiles[tile * 2u + 1u];
    if (count == 0u)
    {
        return color;
    }

    // FogVolumeBlend, with each volume's colour as FogVolumeColor gives it.
    float depth    = 0.0;
    vec3  weighted = vec3(0.0);
    vec3  haze     = vec3(0.0);
    bool  hazeKnown = false;
    for (uint i = 0u; i < count; i++)
    {
        FogVolume volume = fogVolumes[fogTiles[offset + i]];
        FogSpan   span   = FogVolumeSpan(volume, eye, ray, distance);
        float     tau    = span.fullMetres * volume.color.w;
        if (tau <= 0.0)
        {
            continue;
        }
        // A sky evaluation, so made once, and only for a pixel some volume fogs.
        if (!hazeKnown)
        {
            haze      = FogRadiance(fog, ray);
            hazeKnown = true;
        }
        vec3  shown       = volume.info.y != 0u ? volume.color.rgb * haze : volume.color.rgb;
        float hazeOpacity = fog.view.x != 0.0 ? FogOpacity(fog, span.entry, fog.view.y + ray.y * span.entry) : 0.0;
        depth    += tau;
        weighted += tau * mix(shown, haze, hazeOpacity);
    }
    if (depth <= 0.0)
    {
        return color;
    }
    float through = exp(-depth);
    return color * through + weighted / depth * (1.0 - through);
}

#endif
