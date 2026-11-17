#version 450

// One copy of the bloom's chain, half the size of the copy above it: thirteen
// bilinear taps in five overlapping 2x2 blocks. A transcription of
// Render::BloomDownsample and Render::BloomSafeRadiance in Bloom.hpp, which the
// tests hold; the two must agree.

layout(location = 0) in  vec2 vTexCoords;
layout(location = 0) out vec4 outColor;

// Texture_SRV/Sampler are separate descriptors in NVRHI's Vulkan backend, offset
// by VulkanBindingOffsets: shaderResource at +0, sampler at +128.
layout(binding = 0)   uniform texture2D uSource;
layout(binding = 128) uniform sampler   uSourceSampler;

// Render::BloomConstants.
layout(push_constant) uniform PushConstants
{
    vec2  texelSize; // one pixel of the copy read, in texture coordinates
    float scatter;   // unused going down
    uint  karis;     // 1 for the first copy of the scene
} pc;

const vec3  kLumaWeights       = vec3(0.2126, 0.7152, 0.0722);
const float kMaxHalfFloat      = 65504.0;
const float kInnerBlockWeight  = 0.5;
const float kCornerBlockWeight = 0.125;

// Render::BloomSafeRadiance: not a number to black, held under the largest
// half float, so one broken pixel cannot spread over the screen.
vec3 SafeRadiance(vec3 radiance)
{
    return mix(vec3(0.0), min(radiance, vec3(kMaxHalfFloat)), greaterThan(radiance, vec3(0.0)));
}

vec3 Tap(vec2 offset)
{
    vec3 radiance = texture(sampler2D(uSource, uSourceSampler), vTexCoords + offset * pc.texelSize).rgb;
    return pc.karis != 0u ? SafeRadiance(radiance) : radiance;
}

// A block's share, less the brighter it is on the first copy.
float BlockWeight(vec3 block, float share)
{
    return pc.karis != 0u ? share / (1.0 + dot(block, kLumaWeights)) : share;
}

void main()
{
    // a . b . c
    // . j . k .
    // d . e . f
    // . l . m .
    // g . h . i
    vec3 a = Tap(vec2(-2.0,  2.0));
    vec3 b = Tap(vec2( 0.0,  2.0));
    vec3 c = Tap(vec2( 2.0,  2.0));
    vec3 d = Tap(vec2(-2.0,  0.0));
    vec3 e = Tap(vec2( 0.0,  0.0));
    vec3 f = Tap(vec2( 2.0,  0.0));
    vec3 g = Tap(vec2(-2.0, -2.0));
    vec3 h = Tap(vec2( 0.0, -2.0));
    vec3 i = Tap(vec2( 2.0, -2.0));
    vec3 j = Tap(vec2(-1.0,  1.0));
    vec3 k = Tap(vec2( 1.0,  1.0));
    vec3 l = Tap(vec2(-1.0, -1.0));
    vec3 m = Tap(vec2( 1.0, -1.0));

    vec3 inner       = 0.25 * (j + k + l + m);
    vec3 topLeft     = 0.25 * (a + b + d + e);
    vec3 topRight    = 0.25 * (b + c + e + f);
    vec3 bottomLeft  = 0.25 * (d + e + g + h);
    vec3 bottomRight = 0.25 * (e + f + h + i);

    float wInner       = BlockWeight(inner, kInnerBlockWeight);
    float wTopLeft     = BlockWeight(topLeft, kCornerBlockWeight);
    float wTopRight    = BlockWeight(topRight, kCornerBlockWeight);
    float wBottomLeft  = BlockWeight(bottomLeft, kCornerBlockWeight);
    float wBottomRight = BlockWeight(bottomRight, kCornerBlockWeight);

    vec3 sum = inner * wInner + topLeft * wTopLeft + topRight * wTopRight + bottomLeft * wBottomLeft
             + bottomRight * wBottomRight;
    float total = wInner + wTopLeft + wTopRight + wBottomLeft + wBottomRight;
    outColor = vec4(sum / total, 1.0);
}
