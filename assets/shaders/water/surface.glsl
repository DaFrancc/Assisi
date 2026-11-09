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
    // Per cascade, how much of its band is ripples that follow the gusts.
    vec4 rippleShare;
    // xy = which way the wind blows in the body's frame; z = the longest
    // stretch of it the wind crosses, zero for endless water; w = the share of
    // the level's wind that reaches it.
    vec4 wind;
    // x = how strongly its ripples show, 1 as the wind raises them.
    vec4 ripples;
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

// Must match Render::kRippleOnsetSpeed, kRippleGrownSpeed, kRippleSlopeBase,
// kRippleSlopePerSpeed and kFetchGrowthExponent.
const float kRippleOnsetSpeed    = 1.0;
const float kRippleGrownSpeed    = 3.0;
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
// through and as strongly as its ripples are set to show, and the longer waves
// the distance from the shore upwind. @p fromCamera is along world x and z,
// @p local the same point in the body's frame.
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
    scale.cascades = mix(vec4(growth), vec4(ripples), uWater.rippleShare);
    scale.ripples  = ripples;
    return scale;
}
