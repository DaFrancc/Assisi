#ifndef ASSISI_SKY_FOG_GLSL
#define ASSISI_SKY_FOG_GLSL

// The level's fog: a transcription of Render::FogOpacity and Render::FogRadiance
// in Fog.hpp, which is the specification.

#include "scatter.glsl"

// Mirrors Render::FogConstants; the member order is the layout.
struct FogConstants
{
    // Haze then mist: x = optical depth per metre at full thickness, y = the
    // height it is full below, z = falloff in metres (zero for even).
    vec4 layers[2];
    // x = 1 while the fog is on, y = the camera's world height, z = the draw
    // distance, w = where the edge fade starts.
    vec4 view;
    // xyz = the colour where there is no sky, w = 1 while the sky gives it.
    vec4 color;
    SkyScatter scatter;
};

// Must match Render::kFogLevelSpan.
const float kFogLevelSpan = 0.01;
// Below this, expm1 is its first two terms: exp(x) - 1 has lost half its bits by
// then, and the series has lost none.
const float kFogSeriesLimit = 1e-3;

float FogExpm1(float x)
{
    return x < kFogSeriesLimit ? x * (1.0 + 0.5 * x) : exp(x) - 1.0;
}

float FogLayerDensityAt(vec4 layer, float height)
{
    if (layer.z <= 0.0 || height <= layer.y)
    {
        return layer.x;
    }
    return layer.x * exp((layer.y - height) / layer.z);
}

// The optical depth a straight line collects in one layer: full below the
// layer's height, exponential above it, integrated in height and scaled to
// distance by length over the height span.
float FogLayerOpticalDepth(vec4 layer, float len, float fromHeight, float toHeight)
{
    if (layer.x <= 0.0 || len <= 0.0)
    {
        return 0.0;
    }
    if (layer.z <= 0.0)
    {
        return layer.x * len;
    }
    float low  = min(fromHeight, toHeight);
    float high = max(fromHeight, toHeight);
    float span = high - low;
    if (span < kFogLevelSpan)
    {
        return len * FogLayerDensityAt(layer, 0.5 * (low + high));
    }
    float top      = clamp(layer.y, low, high);
    float full     = layer.x * (top - low);
    float thinning = layer.x * layer.z * exp((layer.y - high) / layer.z) * FogExpm1((high - top) / layer.z);
    return (full + thinning) * (len / span);
}

// How much of a surface `distance` away the fog hides, the surface at
// `pointHeight`: the layers' loss, and everything at and past the draw distance.
float FogOpacity(FogConstants fog, float distance, float pointHeight)
{
    float cameraHeight = fog.view.y;
    float depth = FogLayerOpticalDepth(fog.layers[0], distance, cameraHeight, pointHeight) +
                  FogLayerOpticalDepth(fog.layers[1], distance, cameraHeight, pointHeight);
    float edge = fog.view.z > 0.0 ? smoothstep(fog.view.w, fog.view.z, distance) : 0.0;
    return 1.0 - exp(-depth) * (1.0 - edge);
}

// The direction whose sky colours the fog: the ray above the horizon band, the
// horizon at the same bearing below it. See Render::FogDirection.
vec3 FogDirection(vec3 ray)
{
    if (ray.y >= kHorizonSoftness)
    {
        return ray;
    }
    float level = sqrt(1.0 - kHorizonSoftness * kHorizonSoftness);
    float bearingLength = length(ray.xz);
    // Must match Render::kMinTangentLength. Straight down has no bearing.
    if (bearingLength < 1e-6)
    {
        return vec3(level, kHorizonSoftness, 0.0);
    }
    vec2 bearing = ray.xz / bearingLength * level;
    return vec3(bearing.x, kHorizonSoftness, bearing.y);
}

// The fog's colour along a unit ray: the sky's scattered light toward
// FogDirection without either disk, or the authored colour with no sky.
vec3 FogRadiance(FogConstants fog, vec3 ray)
{
    if (fog.color.w == 0.0)
    {
        return fog.color.rgb;
    }
    SkyAir air = SkyAirRadiance(fog.scatter, FogDirection(ray));
    return max(air.sky * fog.scatter.groundColor.w, vec3(0.0));
}

#endif
