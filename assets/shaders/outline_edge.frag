#version 450

// Selection-outline edge-detect pass (Unreal-style). A fullscreen triangle (see
// fullscreen.vert) that reads the silhouette coverage mask and paints an orange
// border of uniform screen-space thickness just OUTSIDE the object's silhouette.
// Because it works from the screen mask rather than the mesh's edges, the outline
// is clean on any topology — a box gets crisp corners, unlike a normal-extruded
// hull. Depth isn't consulted, so the border is drawn on top of everything.

layout(location = 0) in  vec2 vTexCoords;
layout(location = 0) out vec4 outColor;

// Texture_SRV/Sampler are separate descriptors in NVRHI's Vulkan backend, offset
// by VulkanBindingOffsets: shaderResource at +0, sampler at +128 (see fxaa.frag).
layout(binding = 0)   uniform texture2D uMask;
layout(binding = 128) uniform sampler   uMaskSampler;

layout(push_constant) uniform PushConstants
{
    vec4 params; // xy = texel size (1/resolution), z = outline width in pixels, w unused
} pc;

const vec3 kOutlineColor = vec3(1.0, 0.45, 0.0);

float SampleMask(vec2 uv)
{
    return texture(sampler2D(uMask, uMaskSampler), uv).r;
}

void main()
{
    const vec2  texel   = pc.params.xy;
    const float widthPx = pc.params.z;

    // Inside the silhouette: leave the object's rendered surface untouched.
    if (SampleMask(vTexCoords) > 0.5)
    {
        outColor = vec4(0.0);
        return;
    }

    // Outside: this pixel is part of the outline if any covered (inside) texel lies
    // within widthPx of it. Sample a small rounded kernel scaled to that radius;
    // the silhouette interior is solid, so a coarse kernel never misses it.
    const int   kSteps = 2;                       // 2 => reach the full width in ~12 taps
    const float stride = widthPx / float(kSteps); // texels between taps
    float       inside = 0.0;
    for (int j = -kSteps; j <= kSteps; ++j)
    {
        for (int i = -kSteps; i <= kSteps; ++i)
        {
            if (i == 0 && j == 0)
            {
                continue;
            }
            if (i * i + j * j > kSteps * kSteps) // round the kernel to a disc
            {
                continue;
            }
            const vec2 uv = vTexCoords + (vec2(float(i), float(j)) * stride * texel);
            inside        = max(inside, SampleMask(uv));
        }
    }

    outColor = vec4(kOutlineColor, step(0.5, inside));
}
