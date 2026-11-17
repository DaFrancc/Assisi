#ifndef ASSISI_SKY_CLOUD_MAP_GLSL
#define ASSISI_SKY_CLOUD_MAP_GLSL

// Where a direction above the horizon lands in the clouds' sky map, and back: a
// transcription of Render::CloudMapUv and Render::CloudMapDirection in
// Clouds.hpp, which is the specification. cloud_map.comp writes the map through
// one and sky.frag reads it through the other, so they cannot disagree.
//
// Across is the bearing, all the way round; up is the elevation, from the
// horizon to straight up. The map is four times as wide as it is tall, so a
// texel spans the same angle both ways: the clouds near and overhead, which fill
// the most of the screen, get as fine a texel as the ones on the horizon.

const float kCloudMapTwoPi  = 6.28318530718;
const float kCloudMapHalfPi = 1.57079632679;
// Must match Render::kMinTangentLength. Straight up has no bearing.
const float kCloudMapMinBearingLength = 1e-6;

vec2 CloudMapUv(vec3 direction)
{
    float bearing = length(direction.xz) > kCloudMapMinBearingLength ? atan(direction.z, direction.x) : 0.0;
    float elevation = asin(clamp(direction.y, 0.0, 1.0));
    return vec2(fract(bearing / kCloudMapTwoPi), elevation / kCloudMapHalfPi);
}

vec3 CloudMapDirection(vec2 uv)
{
    float bearing   = uv.x * kCloudMapTwoPi;
    float elevation = uv.y * kCloudMapHalfPi;
    float level     = cos(elevation);
    return vec3(cos(bearing) * level, sin(elevation), sin(bearing) * level);
}

#endif
