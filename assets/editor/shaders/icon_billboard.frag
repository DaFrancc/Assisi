#version 450

// Editor entity-icon billboard, fragment stage. Samples the supplied icon and
// composites it (alpha-blended by the pipeline). Fully transparent texels are
// discarded so overlapping billboards don't leave rectangular halos.

layout(location = 0) in  vec2 vTexCoords;
layout(location = 0) out vec4 outColor;

// Texture_SRV/Sampler are separate descriptors in NVRHI's Vulkan backend, offset
// by VulkanBindingOffsets: shaderResource at +0, sampler at +128 (see fxaa.frag).
layout(binding = 0)   uniform texture2D uIcon;
layout(binding = 128) uniform sampler   uIconSampler;

void main()
{
    const vec4 texel = texture(sampler2D(uIcon, uIconSampler), vTexCoords);
    if (texel.a < 0.01)
    {
        discard;
    }
    outColor = texel;
}
