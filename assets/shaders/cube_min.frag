#version 450

layout(location = 0) in vec3  vWorldPos;
layout(location = 1) in vec3  vNormal;
layout(location = 2) in vec2  vTexCoord;
layout(location = 3) in float vViewZ;

layout(location = 0) out vec4 outColor;

// ---- Material textures ------------------------------------------------
// NVRHI's Vulkan backend keeps SRVs and samplers as separate descriptors
// (HLSL t-register/s-register split), offset by VulkanBindingOffsets:
// shaderResource at +0, sampler at +128. See MeshPass::Initialize.
layout(binding = 0)   uniform texture2D uAlbedoTexture;
layout(binding = 128) uniform sampler   uAlbedoSampler;

// ---- Per-frame camera + cluster-grid parameters ------------------------
// ConstantBuffer bindings are offset by +256 (VulkanBindingOffsets::constantBuffer).
layout(binding = 256) uniform FrameConstants
{
    mat4  view;
    uvec4 gridDim;            // xyz used, w unused
    vec4  screenSizeNearFar;  // xy = screen size, z = nearZ, w = farZ
    uvec4 lightCounts;        // x = directional light count, yzw unused
} uFrame;

// ---- Clustered light buffers (must match Render::ClusterGrid GPU structs) --
// StructuredBuffer_SRV shares the shaderResource (+0) space with Texture_SRV,
// so these start at slot 1 (slot 0 is the albedo texture) — see MeshPass.cpp.

struct PointLight { vec4 positionRadius; vec4 colorIntensity; };

struct SpotLight
{
    vec4  positionRadius;
    vec4  directionInner;
    vec4  colorIntensity;
    float outerCutoff;
    float _p0, _p1, _p2;
};

struct DirLight { vec4 directionIntensity; vec4 colorPad; };

struct LightGrid
{
    uint pointOffset;
    uint pointCount;
    uint spotOffset;
    uint spotCount;
};

layout(std430, binding = 1) readonly buffer PointLights    { PointLight pointLights[];   };
layout(std430, binding = 2) readonly buffer SpotLights     { SpotLight  spotLights[];    };
layout(std430, binding = 3) readonly buffer DirLights      { DirLight   dirLights[];     };
layout(std430, binding = 4) readonly buffer LightIndexList { uint       lightIndexList[]; };
layout(std430, binding = 5) readonly buffer LightGrids     { LightGrid  lightGrids[];    };

// Must match Render::ClusterGrid::kMaxLightIndices.
const uint kSpotIndexBase = 65536u;

// No material system for metallic/roughness maps yet (see
// docs/nvrhi-migration-todo.md) — a fixed, moderately rough dielectric looks
// reasonable for every entity in the meantime.
const float kMetallic  = 0.0;
const float kRoughness = 0.6;
const float kAmbient   = 0.03;

const float PI = 3.14159265359;

// ---- Cook-Torrance BRDF -------------------------------------------------

float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a      = roughness * roughness;
    float a2     = a * a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom  = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 CookTorrance(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, vec3 F0)
{
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL == 0.0)
        return vec3(0.0);

    vec3  H        = normalize(V + L);
    float NDF      = DistributionGGX(N, H, kRoughness);
    float G        = GeometrySmith(N, V, L, kRoughness);
    vec3  F        = FresnelSchlick(max(dot(H, V), 0.0), F0);
    vec3  specular = (NDF * G * F) / (4.0 * max(dot(N, V), 0.0) * NdotL + 0.0001);
    vec3  kD       = (1.0 - F) * (1.0 - kMetallic);
    return (kD * albedo / PI + specular) * radiance * NdotL;
}

// Windowed inverse-square attenuation (Frostbite) — 0 at dist >= radius, physically plausible inside.
float Attenuation(float dist, float radius)
{
    float ratio  = dist / radius;
    float ratio4 = ratio * ratio * ratio * ratio;
    float numer  = max(1.0 - ratio4, 0.0);
    return (numer * numer) / (dist * dist + 1.0);
}

// ---- Cluster index -------------------------------------------------------

uint ClusterIndex()
{
    uvec3 gridDim    = uFrame.gridDim.xyz;
    vec2  screenSize = uFrame.screenSizeNearFar.xy;
    float nearZ      = uFrame.screenSizeNearFar.z;
    float farZ       = uFrame.screenSizeNearFar.w;

    uint ix = clamp(uint(gl_FragCoord.x / screenSize.x * float(gridDim.x)), 0u, gridDim.x - 1u);
    uint iy = clamp(uint(gl_FragCoord.y / screenSize.y * float(gridDim.y)), 0u, gridDim.y - 1u);

    // Logarithmic depth slice (abs because vViewZ is negative for visible geometry)
    float viewDepth = abs(vViewZ);
    uint  iz = clamp(uint(log(viewDepth / nearZ) / log(farZ / nearZ) * float(gridDim.z)), 0u, gridDim.z - 1u);

    return ix + iy * gridDim.x + iz * gridDim.x * gridDim.y;
}

// ---- Main -----------------------------------------------------------------

void main()
{
    // The albedo texture is an sRGB format (SRGBA8_UNORM), so the sampler already
    // returns linear values — filtered and mip-blended in linear space, which is
    // why the sRGB->linear step no longer lives here in the shader.
    vec3 albedo = texture(sampler2D(uAlbedoTexture, uAlbedoSampler), vTexCoord).rgb;

    vec3 N = normalize(vNormal);

    // Recover the camera's world-space position from the view matrix: for a
    // rigid view transform (View = R | t, t = -R * cameraPos), cameraPos =
    // -transpose(R) * t, i.e. -R^-1 * t since R is orthonormal.
    vec3 cameraPos = -transpose(mat3(uFrame.view)) * vec3(uFrame.view[3]);
    vec3 V = normalize(cameraPos - vWorldPos);

    vec3 F0 = mix(vec3(0.04), albedo, kMetallic);
    vec3 Lo = vec3(0.0);

    uint dirLightCount = uFrame.lightCounts.x;
    for (uint i = 0u; i < dirLightCount; i++)
    {
        vec3 L        = normalize(-dirLights[i].directionIntensity.xyz);
        vec3 radiance = dirLights[i].colorPad.xyz * dirLights[i].directionIntensity.w;
        Lo += CookTorrance(N, V, L, radiance, albedo, F0);
    }

    uint clusterIdx  = ClusterIndex();
    uint pointOffset = lightGrids[clusterIdx].pointOffset;
    uint pointCount  = lightGrids[clusterIdx].pointCount;
    uint spotOffset  = lightGrids[clusterIdx].spotOffset;
    uint spotCount   = lightGrids[clusterIdx].spotCount;

    for (uint i = 0u; i < pointCount; i++)
    {
        uint  li   = lightIndexList[pointOffset + i];
        vec3  lPos = pointLights[li].positionRadius.xyz;
        float r    = pointLights[li].positionRadius.w;
        vec3  lCol = pointLights[li].colorIntensity.xyz;
        float lInt = pointLights[li].colorIntensity.w;

        vec3  toLight  = lPos - vWorldPos;
        float dist     = length(toLight);
        vec3  L        = toLight / dist;
        vec3  radiance = lCol * lInt * Attenuation(dist, r);

        Lo += CookTorrance(N, V, L, radiance, albedo, F0);
    }

    for (uint i = 0u; i < spotCount; i++)
    {
        uint  li     = lightIndexList[kSpotIndexBase + spotOffset + i];
        vec3  lPos   = spotLights[li].positionRadius.xyz;
        float r      = spotLights[li].positionRadius.w;
        vec3  lDir   = spotLights[li].directionInner.xyz;
        float inner  = spotLights[li].directionInner.w;
        vec3  lCol   = spotLights[li].colorIntensity.xyz;
        float lInt   = spotLights[li].colorIntensity.w;
        float outer  = spotLights[li].outerCutoff;

        vec3  toLight = lPos - vWorldPos;
        float dist    = length(toLight);
        vec3  L       = toLight / dist;

        float theta = dot(L, normalize(-lDir));
        float cone  = smoothstep(outer, inner, theta);
        vec3  radiance = lCol * lInt * Attenuation(dist, r) * cone;

        Lo += CookTorrance(N, V, L, radiance, albedo, F0);
    }

    vec3 color = kAmbient * albedo + Lo;

    // Reinhard tone map + gamma correction
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));

    outColor = vec4(color, 1.0);
}
