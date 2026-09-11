#version 450
#extension GL_EXT_samplerless_texture_functions : require

// Screen-space occlusion, steps three and four: one axis of a separable blur,
// the axis a push constant names.
//
// Four taps, which is one period of ssao.frag's rotation tile: the horizontal
// pass averages a row's four rotations and the vertical pass averages four of
// those rows, so every pixel ends as the mean of all sixteen and the pattern
// cancels exactly. A tap on a different surface is left out rather than
// averaged in — that is what keeps occlusion from bleeding across an outline.

layout(location = 0) in vec2 vTexCoords;
layout(location = 0) out float outOcclusion;

layout(binding = 0) uniform texture2D uOcclusion;
// Distance in metres per pixel (ssao_depth.frag), for telling surfaces apart.
layout(binding = 1) uniform texture2D uDistance;

// Mirrors Render::SsaoConstants.
layout(binding = 256) uniform SsaoConstants
{
    vec4  projection;
    vec4  viewport; // xy = target size in pixels
    vec4  params;
    vec4  blur;     // x = relative depth tolerance
    uvec4 counts;
    vec4  kernel[32];
} uSsao;

layout(push_constant) uniform BlurAxis
{
    ivec2 step; // (1, 0) for the horizontal pass, (0, 1) for the vertical
} pc;

// Must match Render::kSsaoBlurTapOffsets.
const int kTapCount = 4;
const int kTapOffsets[kTapCount] = int[kTapCount](-2, -1, 0, 1);

// Render::SsaoBlurWeight.
float BlurWeight(float centerDistance, float tapDistance)
{
    float step = abs(tapDistance - centerDistance) / (uSsao.blur.x * centerDistance);
    return clamp(1.0 - step, 0.0, 1.0);
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    ivec2 last = ivec2(uSsao.viewport.xy) - 1;
    float centerDistance = texelFetch(uDistance, pixel, 0).r;

    // The centre tap always weighs one, so the sum never divides by zero.
    float sum = 0.0;
    float weight = 0.0;
    for (int i = 0; i < kTapCount; ++i)
    {
        ivec2 tap = clamp(pixel + pc.step * kTapOffsets[i], ivec2(0), last);
        float w = BlurWeight(centerDistance, texelFetch(uDistance, tap, 0).r);
        sum += w * texelFetch(uOcclusion, tap, 0).r;
        weight += w;
    }
    outOcclusion = sum / weight;
}
