#version 450

// One level of a texture camera's picture (see Render::PictureLevels). The
// first is stretched from the part of a target the camera drew; each smaller
// one is half the size of the level above, its pixels one bilinear tap at the
// corner four pixels above share, which is their average.

layout(location = 0) in  vec2 vTexCoords;
layout(location = 0) out vec4 outColor;

// What is read: the drawn target or the level above, alone. Texture_SRV and
// Sampler are offset by VulkanBindingOffsets: shaderResource at +0, sampler
// at +128.
layout(binding = 0)   uniform texture2D uAbove;
layout(binding = 128) uniform sampler   uAboveSampler;

// Render::PictureLevels::Constants.
layout(push_constant) uniform PushConstants
{
    vec2 scale; // the part read, as a fraction of the texture
    vec2 most;  // the furthest coordinate read, inside that part
} pc;

void main()
{
    vec2 uv = min(vTexCoords * pc.scale, pc.most);
    outColor = textureLod(sampler2D(uAbove, uAboveSampler), uv, 0.0);
}
