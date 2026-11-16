// What water.vert and water.frag share: the body's constants, its wave
// textures, the scene's distance and the patches to draw, all in set 1 beside
// the mesh pass's frame in set 0; and how tall each cascade's waves stand at a
// point, where the gusts and the distance from the shore change them.

#include "wind/gusts.glsl"

// Must match Render::kWaterFftSize and kWaterCascadeCount, and WaterGrid.hpp's
// kWaterPatchQuads, kWaterRangeScale and kWaterMorphStart.
const uint  kSurfaceFftSize      = 256u;
const uint  kSurfaceCascadeCount = 4u;
const uint  kWaterPatchQuads     = 32u;
const float kWaterRangeScale     = 3.75;
const float kWaterMorphStart     = 0.9;

// Mirrors Render::WaterDrawConstants; the member order is the layout.
layout(set = 1, binding = 256) uniform WaterConstants
{
    // Cascade c's offset into its tile, in metres: [c / 2].xy or .zw.
    vec4 cascadeOffsets[2];
    vec4 tileLength;
    // xy = the body's x axis across the world, along x and z.
    vec4 axes;
    // The body's rectangle in its own frame, measured from the camera: xy the
    // corner nearest negative x and z, zw the far one.
    vec4 bounds;
    // x = the still water's height above the camera, y = the waves'
    // significant height, z = the slope variance finer than the finest
    // cascade, w = how deep the water must be to hide its edge, in metres.
    vec4 surface;
    // rgb = the colour deep water sends back, a = clarity in metres.
    vec4 color;
    // rgb = the colour crests glow with against the sun.
    vec4 crestColor;
    // Per cascade, how much of its slopes are ripples that follow the gusts.
    vec4 rippleShare;
    // Per cascade, how much of its slopes are swell, which nothing here scales.
    vec4 swellShare;
    // xy = which way the wind blows in the body's frame; z = the longest
    // stretch of it the wind crosses, zero for endless water; w = the share of
    // the level's wind that reaches it.
    vec4 wind;
    // x = how strongly its ripples show, 1 as the wind raises them.
    vec4 ripples;
    // Cascade c's place in its blending noise, in metres: [c / 2].xy or .zw.
    vec4 breakupOffsets[2];
    // Per cascade, 1 where its tile would repeat across the body, so it is
    // drawn twice and blended; 0 where it is drawn once.
    vec4 breakup;
    // Per cascade, 0 where even the strongest gust leaves its slopes too faint
    // to change a pixel, so it is not read at all; 1 where it is drawn.
    vec4 shown;
    GustConstants gusts;
} uWater;

layout(set = 1, binding = 0) uniform texture2DArray uDisplacement;
layout(set = 1, binding = 1) uniform texture2DArray uSlopes;
layout(set = 1, binding = 2) uniform texture2D uSceneDistance;
layout(set = 1, binding = 128) uniform sampler uWaterSampler;

// Mirrors Render::WaterNode.
struct WaterNode
{
    vec2  corner;
    float size;
    uint  level;
};
layout(std430, set = 1, binding = 3) readonly buffer WaterNodes { WaterNode waterNodes[]; };

vec2 CascadeOffset(uint cascade)
{
    vec4 pair = uWater.cascadeOffsets[cascade / 2u];
    return (cascade & 1u) == 0u ? pair.xy : pair.zw;
}

// Where a point @p fromCamera metres from the camera along world x and z lands
// in @p cascade's texture. A texel's value belongs to its corner, which the
// sampler takes for its centre, hence the half texel.
vec2 WaterUv(vec2 fromCamera, uint cascade)
{
    return (fromCamera + CascadeOffset(cascade)) / uWater.tileLength[cascade] + 0.5 / float(kSurfaceFftSize);
}

// Must match Render::kWaterBreakupCellTiles and kWaterBreakupPeriod.
const float kBreakupCellTiles = 1.5;
const int   kBreakupPeriod    = 64;
// How far a broken-up cascade's second copy is shifted, in tiles: far enough
// that the two copies are unrelated where they meet.
const vec2 kBreakupShift = vec2(0.37, 0.61);
// Stretches the blending noise so most of the surface shows mostly one copy.
const float kBreakupContrast = 1.5;
const float kQuarterTurn     = 1.57079632679;
// The noise is read halfway between its lattice planes. Gradient noise is zero
// at every lattice point, so on a plane through them the copies would blend
// evenly on a regular grid, which shows the repeat again.
const float kBreakupSliceDepth = 0.5;

// How much of each of a cascade's two copies shows @p fromCamera metres from
// the camera, as amplitudes whose squares add to one, so the waves keep their
// height. A cascade whose tile would repeat across the body is drawn twice,
// the second copy shifted, and the two blended by noise that changes every
// tile and a half, so no two neighbouring tiles match; a shift keeps every wave
// travelling its own way. Any other cascade is drawn once.
vec2 BreakupWeights(vec2 fromCamera, uint cascade)
{
    if (uWater.breakup[cascade] == 0.0)
    {
        return vec2(1.0, 0.0);
    }
    vec4  pair   = uWater.breakupOffsets[cascade / 2u];
    vec2  offset = (cascade & 1u) == 0u ? pair.xy : pair.zw;
    vec2  cell   = (fromCamera + offset) / (uWater.tileLength[cascade] * kBreakupCellTiles);
    float noise  = GradientNoise(vec3(cell, kBreakupSliceDepth), kBreakupPeriod, cascade + 1u);
    float angle  = clamp(0.5 + kBreakupContrast * noise, 0.0, 1.0) * kQuarterTurn;
    return vec2(cos(angle), sin(angle));
}

// Must match Render::kRippleOnsetSpeed, kRippleGrownSpeed, kRippleSlopeBase,
// kRippleSlopePerSpeed and kFetchGrowthExponent.
const float kRippleOnsetSpeed    = 3.0;
const float kRippleGrownSpeed    = 6.0;
const float kRippleSlopeBase     = 0.003;
const float kRippleSlopePerSpeed = 0.00512;
const float kFetchGrowthExponent = 2.0 / 3.0 - 0.22 / 2.0;

// A wind this close to parallel with one of the body's axes crosses no shore
// along it.
const float kMinWindAcross = 1.0e-4;

// Render::RippleStrength.
float RippleStrength(float localSpeed, float steadySpeed)
{
    if (!(steadySpeed > 0.0) || !(localSpeed > 0.0))
    {
        return 0.0;
    }
    float onset = smoothstep(kRippleOnsetSpeed, kRippleGrownSpeed, localSpeed);
    return onset * sqrt((kRippleSlopeBase + kRippleSlopePerSpeed * localSpeed) /
                        (kRippleSlopeBase + kRippleSlopePerSpeed * steadySpeed));
}

// How far upwind the shore is from @p local, the body's frame measured from the
// camera: back along the wind to the rectangle's edge.
float LocalFetch(vec2 local)
{
    vec2  wind  = uWater.wind.xy;
    float fetch = uWater.wind.z;
    if (wind.x > kMinWindAcross)
    {
        fetch = min(fetch, (local.x - uWater.bounds.x) / wind.x);
    }
    else if (wind.x < -kMinWindAcross)
    {
        fetch = min(fetch, (uWater.bounds.z - local.x) / -wind.x);
    }
    if (wind.y > kMinWindAcross)
    {
        fetch = min(fetch, (local.y - uWater.bounds.y) / wind.y);
    }
    else if (wind.y < -kMinWindAcross)
    {
        fetch = min(fetch, (uWater.bounds.w - local.y) / -wind.y);
    }
    return fetch;
}

// How tall each cascade's waves stand at a point, as a share of the spectrum's:
// the ripples follow the gust there, as much of it as the body's shelter lets
// through and as strongly as its ripples are set to show; the longer wind waves
// follow the distance from the shore upwind; the swell stays as it is. The
// three parts add as variances. @p fromCamera is along world x and z, @p local
// the same point in the body's frame.
struct WaveScale
{
    vec4  cascades;
    // The ripples' share, which the slopes finer than any cascade follow too.
    float ripples;
};

WaveScale WaveScaleAt(vec2 fromCamera, vec2 local)
{
    float share   = uWater.wind.w;
    float steady  = uWater.gusts.wind.z * share;
    float ripples = uWater.ripples.x * RippleStrength(GustSpeedNear(uWater.gusts, fromCamera) * share, steady);
    float growth  = 1.0;
    if (uWater.wind.z > 0.0)
    {
        growth = pow(clamp(LocalFetch(local) / uWater.wind.z, 0.0, 1.0), kFetchGrowthExponent);
    }
    WaveScale scale;
    vec4 longShare = max(vec4(1.0) - uWater.rippleShare - uWater.swellShare, vec4(0.0));
    scale.cascades = sqrt(uWater.swellShare + longShare * growth * growth + uWater.rippleShare * ripples * ripples);
    scale.ripples  = ripples;
    return scale;
}
