// What water.vert and water.frag share: the body's constants, its wave
// textures, the scene's distance and the patches to draw, all in set 1 beside
// the mesh pass's frame in set 0.

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
