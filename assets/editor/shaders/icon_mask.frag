#version 450

// Entity-icon outline mask, fragment stage. Unlike the mesh mask (flat coverage
// over the whole silhouette), this samples the icon and writes coverage only
// where the icon is opaque — so the selection outline traces the icon's artwork,
// not its bounding quad. Pairs with icon_billboard.vert (which provides the UVs)
// and feeds the shared screen-space edge pass.

layout(location = 0) in  vec2 vTexCoords;
layout(location = 0) out vec4 outMask;

// Texture_SRV/Sampler are separate descriptors in NVRHI's Vulkan backend, offset
// by VulkanBindingOffsets: shaderResource at +0, sampler at +128 (see fxaa.frag).
layout(binding = 0)   uniform texture2D uIcon;
layout(binding = 128) uniform sampler   uIconSampler;

void main()
{
    const float alpha = texture(sampler2D(uIcon, uIconSampler), vTexCoords).a;
    if (alpha < 0.5)
    {
        discard; // transparent icon texels contribute no coverage
    }
    outMask = vec4(1.0);
}
