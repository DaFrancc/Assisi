#version 450

// Tone map: linear HDR radiance -> display-encoded colour. The one place in the
// chain that leaves linear space, so everything upstream of it (the MSAA resolve,
// and any effect added later) operates on radiance, and everything downstream
// (FXAA, editor chrome) operates on perceptual values.
//
// Reinhard plus a 2.2 gamma, which is what the mesh shader used to apply inline
// per fragment. The operator is deliberately unchanged here: moving it is one
// change, replacing it is another.

layout(location = 0) in  vec2 vTexCoords;
layout(location = 0) out vec4 outColor;

// Texture_SRV/Sampler are separate descriptors in NVRHI's Vulkan backend, offset
// by VulkanBindingOffsets: shaderResource at +0, sampler at +128 (see fxaa.frag).
layout(binding = 0)   uniform texture2D uScene;
layout(binding = 128) uniform sampler   uSceneSampler;

layout(push_constant) uniform PushConstants
{
    // Non-zero copies the input through untouched. Two callers need that: the
    // material debug views, whose scene target holds channel values rather than
    // radiance, and the final copy of an already-mapped image to the swapchain.
    uint passthrough;
} pc;

void main()
{
    const vec4 scene = texture(sampler2D(uScene, uSceneSampler), vTexCoords);
    if (pc.passthrough != 0u)
    {
        outColor = scene;
        return;
    }

    vec3 color = scene.rgb / (scene.rgb + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));
    outColor = vec4(color, scene.a);
}
