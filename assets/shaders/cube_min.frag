#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec3  vWorldPos;
layout(location = 1) in vec3  vNormal;
layout(location = 2) in vec2  vTexCoord;
layout(location = 3) in float vViewZ;
layout(location = 4) in vec3  vTangent;
layout(location = 5) in float vTangentSign;

layout(location = 0) out vec4 outColor;

// ---- Material textures (bindless) -------------------------------------
// Every material texture lives in one bindless array in descriptor set 1; a
// material's channels are slots in it, indexed via MaterialConstants below.
// NVRHI keeps SRVs and samplers as separate descriptors (HLSL t/s split); the
// one shared sampler stays in set 0 at +128. See MeshPass.
layout(set = 1, binding = 0) uniform texture2D uTextures[];
layout(binding = 128) uniform sampler uMaterialSampler;

// Sample a bindless material texture by slot. nonuniformEXT is a no-op while the
// index is per-draw-uniform (one material per draw) and becomes correct once
// instanced batching makes it vary between fragments (stage E).
vec4 sampleMaterialTex(uint slot, vec2 uv)
{
    return texture(sampler2D(uTextures[nonuniformEXT(slot)], uMaterialSampler), uv);
}

// ---- Per-material constants (mirrors Render::MaterialConstants, 64 bytes) ---
// ConstantBuffer bindings are offset by +256; the per-frame block is slot 0
// (256), this per-material block is slot 1 (257).
layout(binding = 257) uniform MaterialConstants
{
    vec4  baseColorFactor;
    vec4  emissiveFactorNormalScale; // xyz = emissive, w = normalScale
    vec4  metalRoughOcclusion;       // x = metallic, y = roughness, z = occlusion strength, w = pad
    uvec4 flags;                     // bit0 = has normal texture; rest reserved
    uvec4 texIndices;                // bindless slots: x=baseColor y=normal z=metalRough w=occlusion
    uvec4 texIndicesEmissive;        // x = emissive bindless slot
} uMaterial;

// ---- Per-frame camera + cluster-grid parameters ------------------------
layout(binding = 256) uniform FrameConstants
{
    mat4  view;
    uvec4 gridDim;            // xyz used, w unused
    vec4  screenSizeNearFar;  // xy = screen size, z = nearZ, w = farZ
    uvec4 lightCounts;        // x = directional light count, y = debug view mode, zw unused
} uFrame;

// Debug view modes (must match Render::MaterialDebugView). 0 = normal lit render;
// the rest short-circuit to a single material channel for inspection.
const uint kDebugNone      = 0u;
const uint kDebugBaseColor = 1u;
const uint kDebugMetallic  = 2u;
const uint kDebugRoughness = 3u;
const uint kDebugNormal    = 4u;
const uint kDebugOcclusion = 5u;
const uint kDebugEmissive  = 6u;

// ---- Clustered light buffers (must match Render::ClusterGrid GPU structs) --
// StructuredBuffer_SRV shares the shaderResource (+0) space with Texture_SRV,
// so these occupy slots 1-5 (slot 0 is baseColor) — see MeshPass.cpp.

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

// Must match Render::ClusterGrid::kMaxLightIndices and cluster_cull.comp's MAX_LIGHT_INDICES.
const uint kSpotIndexBase = 262144u;

const float kAmbient = 0.03;
const float PI = 3.14159265359;

// ---- Material sample ---------------------------------------------------
// Everything that reads the material's textures + factors lives here, so the
// lighting code below is agnostic to how the surface was authored. A later
// bindless transition rewrites only the texture() fetches in this block.
struct Surface
{
    vec3  albedo;
    float metallic;
    float roughness;
    float occlusion; // 1 = unoccluded
    vec3  emissive;
    vec3  normal;    // world-space, normal-mapped if present
};

Surface SampleMaterial()
{
    Surface s;

    // baseColor is an sRGB texture, so the sampler already returns linear values
    // (filtered/mip-blended in linear space); multiply by the linear factor.
    vec4 base = sampleMaterialTex(uMaterial.texIndices.x, vTexCoord) * uMaterial.baseColorFactor;
    s.albedo = base.rgb;

    // glTF metallic-roughness packing: G = roughness, B = metallic. The texture
    // is linear data (empty channel = white = 1, leaving the factor untouched).
    vec2 mr = sampleMaterialTex(uMaterial.texIndices.z, vTexCoord).gb;
    s.roughness = clamp(uMaterial.metalRoughOcclusion.y * mr.x, 0.04, 1.0);
    s.metallic  = clamp(uMaterial.metalRoughOcclusion.x * mr.y, 0.0, 1.0);

    // Occlusion: R channel, lerped by strength (strength 0 = ignore the map).
    float ao = sampleMaterialTex(uMaterial.texIndices.w, vTexCoord).r;
    s.occlusion = 1.0 + uMaterial.metalRoughOcclusion.z * (ao - 1.0);

    s.emissive = sampleMaterialTex(uMaterial.texIndicesEmissive.x, vTexCoord).rgb *
                 uMaterial.emissiveFactorNormalScale.xyz;

    vec3 N = normalize(vNormal);
    if (uMaterial.flags.x != 0u) // has normal texture
    {
        // Re-orthonormalize the interpolated tangent against N (Gram-Schmidt),
        // then build the bitangent from the stored handedness.
        vec3 T = normalize(vTangent - dot(vTangent, N) * N);
        vec3 B = cross(N, T) * vTangentSign;
        vec3 sampledNormal = sampleMaterialTex(uMaterial.texIndices.y, vTexCoord).xyz * 2.0 - 1.0;
        sampledNormal.xy *= uMaterial.emissiveFactorNormalScale.w; // normalScale
        s.normal = normalize(mat3(T, B, N) * sampledNormal);
    }
    else
    {
        s.normal = N;
    }

    return s;
}

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

vec3 CookTorrance(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, vec3 F0, float roughness, float metallic)
{
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL == 0.0)
        return vec3(0.0);

    vec3  H        = normalize(V + L);
    float NDF      = DistributionGGX(N, H, roughness);
    float G        = GeometrySmith(N, V, L, roughness);
    vec3  F        = FresnelSchlick(max(dot(H, V), 0.0), F0);
    vec3  specular = (NDF * G * F) / (4.0 * max(dot(N, V), 0.0) * NdotL + 0.0001);
    vec3  kD       = (1.0 - F) * (1.0 - metallic);
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
    Surface surf = SampleMaterial();

    // Debug views short-circuit the lighting: output one channel straight, so the
    // material's inputs can be inspected in isolation. Colour channels (base /
    // emissive) are linear here, so gamma-encode them for display; the scalar
    // data channels show raw, and the normal is mapped to [0,1] as an RGB.
    uint debugMode = uFrame.lightCounts.y;
    if (debugMode != kDebugNone)
    {
        if (debugMode == kDebugBaseColor)
            outColor = vec4(pow(surf.albedo, vec3(1.0 / 2.2)), 1.0);
        else if (debugMode == kDebugMetallic)
            outColor = vec4(vec3(surf.metallic), 1.0);
        else if (debugMode == kDebugRoughness)
            outColor = vec4(vec3(surf.roughness), 1.0);
        else if (debugMode == kDebugNormal)
            outColor = vec4(surf.normal * 0.5 + 0.5, 1.0);
        else if (debugMode == kDebugOcclusion)
            outColor = vec4(vec3(surf.occlusion), 1.0);
        else if (debugMode == kDebugEmissive)
            outColor = vec4(pow(surf.emissive, vec3(1.0 / 2.2)), 1.0);
        else
            outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3  albedo    = surf.albedo;
    vec3  N         = surf.normal;
    float roughness = surf.roughness;
    float metallic  = surf.metallic;

    // Recover the camera's world-space position from the view matrix: for a
    // rigid view transform (View = R | t, t = -R * cameraPos), cameraPos =
    // -transpose(R) * t, i.e. -R^-1 * t since R is orthonormal.
    vec3 cameraPos = -transpose(mat3(uFrame.view)) * vec3(uFrame.view[3]);
    vec3 V = normalize(cameraPos - vWorldPos);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 Lo = vec3(0.0);

    uint dirLightCount = uFrame.lightCounts.x;
    for (uint i = 0u; i < dirLightCount; i++)
    {
        vec3 L        = normalize(-dirLights[i].directionIntensity.xyz);
        vec3 radiance = dirLights[i].colorPad.xyz * dirLights[i].directionIntensity.w;
        Lo += CookTorrance(N, V, L, radiance, albedo, F0, roughness, metallic);
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

        Lo += CookTorrance(N, V, L, radiance, albedo, F0, roughness, metallic);
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

        Lo += CookTorrance(N, V, L, radiance, albedo, F0, roughness, metallic);
    }

    // Ambient is scaled by the occlusion map (direct light already accounts for
    // visibility via N·L); emissive is added on top, unlit.
    vec3 color = kAmbient * albedo * surf.occlusion + Lo + surf.emissive;

    // Reinhard tone map + gamma correction
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));

    outColor = vec4(color, 1.0);
}
