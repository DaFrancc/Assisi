#version 450
#extension GL_EXT_samplerless_texture_functions : require

// The scene's depth as a distance in metres, one sample a pixel.
// Render::SsaoLinearDepth is this on the CPU.
//
// Every screen-space feature reads this rather than the depth buffer itself:
// the depth buffer is multisampled under MSAA and holds a projected value
// either way, and turning it into a distance once here spares each reader doing
// it for every sample it takes.
//
// ASSISI_MSAA_DEPTH builds the variant that reads a multisampled buffer. It
// takes sample zero rather than resolving: every reader asks about one surface,
// and an average across a silhouette is a depth belonging to nothing.

layout(location = 0) in vec2 vTexCoords;
layout(location = 0) out float outDistance;

#ifdef ASSISI_MSAA_DEPTH
layout(binding = 0) uniform texture2DMS uDepth;
#else
layout(binding = 0) uniform texture2D uDepth;
#endif

// Mirrors Render::SceneDistanceConstants.
layout(binding = 256) uniform SceneDistanceConstants
{
    vec4 projection; // x = xScale, y = yScale, z = depthScale, w = depthBias
} uDistance;

void main()
{
    // Sample zero of a multisampled buffer, mip zero of a plain one: both are
    // the third argument.
    float depth = texelFetch(uDepth, ivec2(gl_FragCoord.xy), 0).r;
    outDistance = uDistance.projection.z / (depth + uDistance.projection.w);
}
