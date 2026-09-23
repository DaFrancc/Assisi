#version 460
#extension GL_EXT_samplerless_texture_functions : require

// Writes a cascade's still depth back into the slice the shader reads, over
// whatever the viewport covers: the texels movers were drawn into last time, or
// the whole slice after the still layer is rebaked. Drawn through the tile
// reset's triangle with depth test Always.
//
// A draw rather than a copy because the slice stays a depth target throughout:
// a copy moves both images to transfer layouts, which a GPU that compresses
// depth pays for across the whole array, every frame something moved.

layout(binding = 0) uniform texture2DArray uStill;

layout(push_constant) uniform PushConstants
{
    uint cascade;
    uint padding0;
    uint padding1;
    uint padding2;
} uPush;

void main()
{
    gl_FragDepth = texelFetch(uStill, ivec3(ivec2(gl_FragCoord.xy), int(uPush.cascade)), 0).r;
}
