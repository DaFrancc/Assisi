#version 450

// One level of a texture camera's picture, half the size of the level above
// it: one bilinear tap at the corner four pixels above share is their average.
// See Render::PictureMips.

layout(location = 0) in  vec2 vTexCoords;
layout(location = 0) out vec4 outColor;

// The level above, alone; Texture_SRV and Sampler are offset by
// VulkanBindingOffsets: shaderResource at +0, sampler at +128.
layout(binding = 0)   uniform texture2D uAbove;
layout(binding = 128) uniform sampler   uAboveSampler;

void main()
{
    outColor = textureLod(sampler2D(uAbove, uAboveSampler), vTexCoords, 0.0);
}
