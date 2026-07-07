#version 450

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

// NVRHI's Vulkan backend keeps SRVs and samplers as separate descriptors
// (HLSL t-register/s-register split), offset by VulkanBindingOffsets:
// shaderResource at +0, sampler at +128. See MeshPass::Initialize.
layout(binding = 0)   uniform texture2D uAlbedoTexture;
layout(binding = 128) uniform sampler   uAlbedoSampler;

void main()
{
    const vec3 lightDir = normalize(vec3(0.4, 0.8, 0.5));
    const float ambient = 0.15;
    const float diffuse = max(dot(normalize(vNormal), lightDir), 0.0);

    vec3 albedo = texture(sampler2D(uAlbedoTexture, uAlbedoSampler), vTexCoord).rgb;

    outColor = vec4(albedo * (ambient + diffuse), 1.0);
}
