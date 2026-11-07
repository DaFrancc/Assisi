#ifndef ASSISI_SKY_FOG_VOLUMES_GLSL
#define ASSISI_SKY_FOG_VOLUMES_GLSL

// Placed fog volumes: a transcription of Render::FogVolumeSpan, FogVolumeColor
// and FogVolumeBlend in FogVolume.hpp and FogVolume.cpp, which are the
// specification.
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
    vec4  profile; // x = falloff in metres above the floor, zero for even
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

// Must match FogVolume.cpp's constants of the same names.
const float kFogOutlineBoundsMargin = 1.0e-3;
const float kFogGaussNode           = 0.57735027;
const float kFogMaxFalloffPerStep   = 2.0;
const float kFogMaxStepsPerPiece    = 8.0;

// Must match FogVolume.cpp's Density.
const int kFogDensityBox       = 0;
const int kFogDensityEllipsoid = 1;
const int kFogDensityLayers    = 2;
const int kFogDensityWall      = 3;

// Must match FogVolume.cpp's Breaks::kSlots.
const int kFogBreakSlots = 6;

// What FogVolumeSpan returns: metres of full fog, and where the box is reached.
// Render::FogSpan's `length` is `fullMetres` here, because GLSL reads `.length`
// as the array method.
struct FogSpan
{
    float fullMetres;
    float entry;
};

// See DensityContext.
struct FogDensity
{
    vec3  start;
    vec3  ray;
    vec3  extent;
    float softness;
    float falloff;
    vec2  wallFrom;
    vec2  wallNormal;
    float band;
    int   kind;
};

// See Breaks. Every slot is read at a constant index, so the array stays in
// registers.
struct FogBreaks
{
    float at[kFogBreakSlots];
};

float FogCross(vec2 a, vec2 b)
{
    return a.x * b.y - a.y * b.x;
}

// See Ramp.
float FogRamp(float depth, float span, float soft)
{
    if (depth < 0.0 || depth > span)
    {
        return 0.0;
    }
    if (soft <= 0.0)
    {
        return 1.0;
    }
    return min(min(depth, span - depth) / min(soft, 0.5 * span), 1.0);
}

// See FalloffAt.
float FogFalloffAt(float height, float falloff)
{
    return falloff > 0.0 ? exp(-max(height, 0.0) / falloff) : 1.0;
}

// See DensityAt.
float FogDensityAt(FogDensity d, float t)
{
    vec3  p       = d.start + d.ray * t;
    vec3  e       = d.extent;
    float falloff = FogFalloffAt(p.y + e.y, d.falloff);
    if (d.kind == kFogDensityBox)
    {
        return FogRamp(p.x + e.x, 2.0 * e.x, d.softness) * FogRamp(p.y + e.y, 2.0 * e.y, d.softness) *
               FogRamp(p.z + e.z, 2.0 * e.z, d.softness) * falloff;
    }
    if (d.kind == kFogDensityEllipsoid)
    {
        vec3  unit     = p / e;
        float radiusSq = dot(unit, unit);
        float shell    = d.band > 0.0 ? clamp((1.0 - radiusSq) / d.band, 0.0, 1.0) : (radiusSq <= 1.0 ? 1.0 : 0.0);
        return shell * falloff;
    }
    float layers = FogRamp(p.y + e.y, 2.0 * e.y, d.softness) * falloff;
    if (d.kind == kFogDensityWall)
    {
        float depth = dot(d.wallNormal, p.xz - d.wallFrom);
        return layers * clamp(1.0 - depth / d.softness, 0.0, 1.0);
    }
    return layers;
}

FogBreaks FogNoBreaks(float len)
{
    FogBreaks breaks;
    for (int i = 0; i < kFogBreakSlots; i++)
    {
        breaks.at[i] = len;
    }
    return breaks;
}

// See FaceBreaks.
vec2 FogFaceBreaks(float start, float rate, float halfExtent, float soft, float len)
{
    if (soft <= 0.0 || rate == 0.0)
    {
        return vec2(len);
    }
    float inner = halfExtent - min(soft, halfExtent);
    float a     = (-inner - start) / rate;
    float b     = (inner - start) / rate;
    return clamp(vec2(min(a, b), max(a, b)), vec2(0.0), vec2(len));
}

void FogSwapIfAfter(inout float a, inout float b)
{
    float low = min(a, b);
    b         = max(a, b);
    a         = low;
}

// A sorting network, the same as Breaks::Sort's: these twelve compare-and-swaps,
// in this order, sort any six values, the fewest any fixed sequence can. The
// pairs follow no pattern and each relies on the ones before it, so keep them
// as they are. Written out, every index is a constant and the slots stay in
// registers; a sort whose indices depend on the data pushes them out to memory.
void FogSortBreaks(inout FogBreaks breaks)
{
    FogSwapIfAfter(breaks.at[0], breaks.at[5]);
    FogSwapIfAfter(breaks.at[1], breaks.at[3]);
    FogSwapIfAfter(breaks.at[2], breaks.at[4]);
    FogSwapIfAfter(breaks.at[1], breaks.at[2]);
    FogSwapIfAfter(breaks.at[3], breaks.at[4]);
    FogSwapIfAfter(breaks.at[0], breaks.at[3]);
    FogSwapIfAfter(breaks.at[2], breaks.at[5]);
    FogSwapIfAfter(breaks.at[0], breaks.at[1]);
    FogSwapIfAfter(breaks.at[2], breaks.at[3]);
    FogSwapIfAfter(breaks.at[4], breaks.at[5]);
    FogSwapIfAfter(breaks.at[1], breaks.at[2]);
    FogSwapIfAfter(breaks.at[3], breaks.at[4]);
}

// See IntegratePiece.
float FogIntegratePiece(FogDensity d, float from, float to)
{
    float thinning = d.falloff > 0.0 ? abs(d.ray.y) * (to - from) / d.falloff : 0.0;
    float steps    = clamp(ceil(thinning / kFogMaxFalloffPerStep), 1.0, kFogMaxStepsPerPiece);
    float step     = (to - from) / steps;
    float offset   = 0.5 * step * kFogGaussNode;
    float sum      = 0.0;
    for (float index = 0.0; index < steps; index += 1.0)
    {
        float middle = from + (index + 0.5) * step;
        sum += FogDensityAt(d, middle - offset) + FogDensityAt(d, middle + offset);
    }
    return sum * 0.5 * step;
}

// See Integrate.
float FogIntegrate(FogDensity d, FogBreaks breaks, float from, float to)
{
    float sum = 0.0;
    float at  = from;
    for (int i = 0; i < kFogBreakSlots; i++)
    {
        float next = clamp(breaks.at[i], from, to);
        if (next > at)
        {
            sum += FogIntegratePiece(d, at, next);
            at = next;
        }
    }
    if (to > at)
    {
        sum += FogIntegratePiece(d, at, to);
    }
    return sum;
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

FogSpan FogBoxSpan(vec3 origin, vec3 ray, FogVolume volume, float distance)
{
    vec3  extent = volume.extents.xyz;
    float soft   = volume.extents.w;
    vec3  start;
    float entry;
    float len;
    if (!FogBoxStretch(origin, ray, extent, distance, start, entry, len))
    {
        return FogSpan(0.0, 0.0);
    }
    FogBreaks breaks;
    for (int axis = 0; axis < 3; axis++)
    {
        vec2 pair = FogFaceBreaks(start[axis], ray[axis], extent[axis], soft, len);
        breaks.at[2 * axis]     = pair.x;
        breaks.at[2 * axis + 1] = pair.y;
    }
    FogSortBreaks(breaks);
    FogDensity d = FogDensity(start, ray, extent, soft, volume.profile.x, vec2(0.0), vec2(0.0), 0.0,
                              kFogDensityBox);
    return FogSpan(FogIntegrate(d, breaks, 0.0, len), entry);
}

FogSpan FogEllipsoidSpan(vec3 origin, vec3 ray, FogVolume volume, float distance)
{
    vec3  radii    = volume.extents.xyz;
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
    float depth = min(volume.extents.w / smallest, 1.0);
    float band  = depth * (2.0 - depth);
    float core  = 1.0 - band;
    FogBreaks breaks = FogNoBreaks(to - from);
    if (band > 0.0 && closestSq < core)
    {
        // In order already, so no sorting.
        float coreHalf = sqrt((core - closestSq) / stretch);
        vec2  coreEnds = clamp(vec2(closestAt - coreHalf, closestAt + coreHalf) - from, vec2(0.0), vec2(to - from));
        breaks.at[0] = coreEnds.x;
        breaks.at[1] = coreEnds.y;
    }
    FogDensity d = FogDensity(origin + ray * from, ray, radii, volume.extents.w, volume.profile.x, vec2(0.0),
                              vec2(0.0), band, kFogDensityEllipsoid);
    return FogSpan(FogIntegrate(d, breaks, 0.0, to - from), from);
}

// Whether the edge from `a` to `b` crosses the ray from `point` toward +x: an
// odd number of such edges puts the point inside. See EdgeFlipsInside.
bool FogEdgeFlipsInside(vec2 a, vec2 b, vec2 point)
{
    return (a.y > point.y) != (b.y > point.y) && point.x < a.x + (point.y - a.y) * (b.x - a.x) / (b.y - a.y);
}

// The stretch's part inside one wall's softness band: see WallBandOf.
vec2 FogWallBand(FogOutlineEdge edge, vec2 start, vec2 ray, float soft, float len)
{
    vec2  from  = edge.points.xy;
    vec2  to    = edge.points.zw;
    float depth = dot(edge.normal.xy, start - from);
    float rate  = dot(edge.normal.xy, ray);
    float low   = 0.0;
    float high  = len;
    FogKeepNonNegative(depth, rate, low, high);
    FogKeepNonNegative(soft - depth, -rate, low, high);
    FogKeepNonNegative(dot(edge.miters.xy, start - from), dot(edge.miters.xy, ray), low, high);
    FogKeepNonNegative(-dot(edge.miters.zw, start - to), -dot(edge.miters.zw, ray), low, high);
    return vec2(low, high);
}

FogSpan FogOutlineSpan(vec3 origin, vec3 ray, FogVolume volume, float distance)
{
    vec3  extent = volume.extents.xyz;
    float soft   = volume.extents.w;
    uint  first  = volume.info.z;
    uint  count  = volume.info.w;
    vec3  start3;
    float entry;
    float len;
    vec3  bounds = extent + vec3(kFogOutlineBoundsMargin, 0.0, kFogOutlineBoundsMargin);
    if (count == 0u || !FogBoxStretch(origin, ray, bounds, distance, start3, entry, len))
    {
        return FogSpan(0.0, 0.0);
    }
    // One pair, in order already, and every other slot at the end: no sorting.
    FogBreaks breaks = FogNoBreaks(len);
    vec2 layerBreaks = FogFaceBreaks(start3.y, ray.y, extent.y, soft, len);
    breaks.at[0] = layerBreaks.x;
    breaks.at[1] = layerBreaks.y;
    FogDensity layers = FogDensity(start3, ray, extent, soft, volume.profile.x, vec2(0.0), vec2(0.0), 0.0,
                                   kFogDensityLayers);
    vec2 start = start3.xz;
    vec2 flat2 = ray.xz;

    // One pass, each edge fetched once: OutlineSpan's even-odd test, its walls
    // crossed and its walls' softness.
    float whole  = FogIntegrate(layers, breaks, 0.0, len);
    bool  inside = false;
    float fog    = 0.0;
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
                float rest = FogIntegrate(layers, breaks, at, len);
                fog += denominator < 0.0 ? rest : -rest;
            }
        }
        if (soft > 0.0)
        {
            vec2 band = FogWallBand(edge, start, flat2, soft, len);
            if (band.y > band.x)
            {
                FogDensity wall = layers;
                wall.kind       = kFogDensityWall;
                wall.wallFrom   = from;
                wall.wallNormal = edge.normal.xy;
                fog -= FogIntegrate(wall, breaks, band.x, band.y);
            }
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
    uint kind = volume.info.x;
    if (kind == kFogVolumeBox)
    {
        return FogBoxSpan(origin, direction, volume, distance);
    }
    if (kind == kFogVolumeEllipsoid)
    {
        return FogEllipsoidSpan(origin, direction, volume, distance);
    }
    if (kind == kFogVolumeOutline)
    {
        return FogOutlineSpan(origin, direction, volume, distance);
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
    float depth     = 0.0;
    vec3  weighted  = vec3(0.0);
    vec3  haze      = vec3(0.0);
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
