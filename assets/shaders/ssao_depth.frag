#version 450
#extension GL_EXT_samplerless_texture_functions : require

// Screen-space occlusion, step one: the scene's depth as a distance in metres,
// one sample a pixel. Render::SsaoLinearDepth is this on the CPU.
//
// Every later step reads this rather than the depth buffer itself: the depth
// buffer is multisampled under MSAA and holds a projected value either way,
// and turning it into a distance once here spares the occlusion step doing it
// for every sample it takes.
//
// ASSISI_MSAA_DEPTH builds the variant that reads a multisampled buffer. It
// takes sample zero rather than resolving: occlusion is a property of one
// surface, and an average across a silhouette is a depth belonging to nothing.

layout(location = 0) in vec2 vTexCoords;
layout(location = 0) out float outDistance;

#ifdef ASSISI_MSAA_DEPTH
layout(binding = 0) uniform texture2DMS uDepth;
#else
layout(binding = 0) uniform texture2D uDepth;
#endif

// Mirrors Render::SsaoConstants; see ssao.frag for the lanes this step skips.
layout(binding = 256) uniform SsaoConstants
{
    vec4  projection; // x = xScale, y = yScale, z = depthScale, w = depthBias
    vec4  viewport;
    vec4  params;
    vec4  blur;
    uvec4 counts;
    vec4  kernel[32];
} uSsao;

void main()
{
    // Sample zero of a multisampled buffer, mip zero of a plain one: both are
    // the third argument.
    float depth = texelFetch(uDepth, ivec2(gl_FragCoord.xy), 0).r;
    outDistance = uSsao.projection.z / (depth + uSsao.projection.w);
}
