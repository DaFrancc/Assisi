#version 450

// Climbing the bloom's chain back up: the smaller copy, read through a 3x3
// tent, is blended over the larger one by the scatter, which this writes as
// its alpha. A transcription of Render::BloomUpsample in Bloom.hpp, which the
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
    vec2  texelSize; // one pixel of the smaller copy, in texture coordinates
    float scatter;   // the share the smaller copy takes over the larger
    uint  karis;     // unused going up
} pc;

const float kTentMiddle = 4.0;
const float kTentEdge   = 2.0;
const float kTentCorner = 1.0;
const float kTentTotal  = 16.0;

vec3 Tap(vec2 offset)
{
    return texture(sampler2D(uSource, uSourceSampler), vTexCoords + offset * pc.texelSize).rgb;
}

void main()
{
    vec3 corners = Tap(vec2(-1.0, 1.0)) + Tap(vec2(1.0, 1.0)) + Tap(vec2(-1.0, -1.0)) + Tap(vec2(1.0, -1.0));
    vec3 edges   = Tap(vec2(0.0, 1.0)) + Tap(vec2(-1.0, 0.0)) + Tap(vec2(1.0, 0.0)) + Tap(vec2(0.0, -1.0));
    vec3 tent    = (Tap(vec2(0.0)) * kTentMiddle + edges * kTentEdge + corners * kTentCorner) / kTentTotal;
    outColor = vec4(tent, pc.scatter);
}
