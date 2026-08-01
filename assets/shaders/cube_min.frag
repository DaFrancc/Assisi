#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec3  vWorldPos;
layout(location = 1) in vec3  vNormal;
layout(location = 2) in vec2  vTexCoord;
layout(location = 3) in float vViewZ;
layout(location = 4) in vec3  vTangent;
layout(location = 5) in float vTangentSign;
layout(location = 6) in flat uint vMaterialIndex; // row into the material table

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

// ---- Material table (mirrors Render::MaterialConstants, 96 bytes) -----------
// Stage D: materials no longer bind a per-draw constant buffer; every material's
// constants live in one row of a shared structured buffer, and each instance
// carries its row index (vMaterialIndex). This is t0 in MeshPass's layout —
// StructuredBuffer_SRV shares the shaderResource (+0) space with Texture_SRV.
struct MaterialRow
{
    vec4  baseColorFactor;
    vec4  emissiveFactorNormalScale; // xyz = emissive, w = normalScale
    vec4  metalRoughOcclusion;       // x = metallic, y = roughness, z = occlusion strength, w = pad
    uvec4 flags;                     // bit0 = has normal texture; rest reserved
    uvec4 texIndices;                // bindless slots: x=baseColor y=normal z=metalRough w=occlusion
    uvec4 texIndicesEmissive;        // x = emissive bindless slot
};

layout(std430, binding = 0) readonly buffer Materials
{
    MaterialRow materials[];
};

// ---- Per-frame camera + cluster-grid parameters ------------------------
// viewProjection leads to match the vertex shader / Render::FrameConstants; the
// fragment shader doesn't use it, but the block layout must be identical so the
// members past it land at the same offsets.
layout(binding = 256) uniform FrameConstants
{
    mat4  viewProjection;
    mat4  view;
    uvec4 gridDim;            // xyz used, w unused
    vec4  screenSizeNearFar;  // xy = screen size, z = nearZ, w = farZ
    uvec4 lightCounts;        // x = directional light count, y = debug view mode, zw unused
    vec4  cameraPosition;     // world-space camera position, w unused
    // Froxel lookup scale/bias (see Render::FrameConstants): xy = gridDim.xy /
    // screenSize, z = gridDim.z / log(farZ/nearZ), w = -z * log(nearZ).
    vec4  clusterScale;
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
// StructuredBuffer_SRV shares the shaderResource (+0) space with Texture_SRV, so
// these occupy slots 1-5 (slot 0 is the material table, slot 6 the instance
// buffer) — see MeshPass.cpp.

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

    // Fetch this instance's material row; the vertex shader passed its index.
    MaterialRow mat = materials[vMaterialIndex];

    // baseColor is an sRGB texture, so the sampler already returns linear values
    // (filtered/mip-blended in linear space); multiply by the linear factor.
    vec4 base = sampleMaterialTex(mat.texIndices.x, vTexCoord) * mat.baseColorFactor;
    s.albedo = base.rgb;

    // glTF metallic-roughness packing: G = roughness, B = metallic. The texture
    // is linear data (empty channel = white = 1, leaving the factor untouched).
    vec2 mr = sampleMaterialTex(mat.texIndices.z, vTexCoord).gb;
    s.roughness = clamp(mat.metalRoughOcclusion.y * mr.x, 0.04, 1.0);
    s.metallic  = clamp(mat.metalRoughOcclusion.x * mr.y, 0.0, 1.0);

    // Occlusion: R channel, lerped by strength (strength 0 = ignore the map).
    float ao = sampleMaterialTex(mat.texIndices.w, vTexCoord).r;
    s.occlusion = 1.0 + mat.metalRoughOcclusion.z * (ao - 1.0);

    s.emissive = sampleMaterialTex(mat.texIndicesEmissive.x, vTexCoord).rgb *
                 mat.emissiveFactorNormalScale.xyz;

    vec3 N = normalize(vNormal);
    if (mat.flags.x != 0u) // has normal texture
    {
        // Re-orthonormalize the interpolated tangent against N (Gram-Schmidt),
        // then build the bitangent from the stored handedness. The projection
        // collapses to zero when the tangent is parallel to N — which the
        // default tangent (1,0,0,1) is on any +/-X face — so fall back to an
        // arbitrary perpendicular rather than normalize(0) = NaN.
        vec3 Traw = vTangent - dot(vTangent, N) * N;
        vec3 T    = dot(Traw, Traw) > 1e-8 ? normalize(Traw)
                                           : normalize(abs(N.y) < 0.99 ? cross(N, vec3(0.0, 1.0, 0.0))
                                                                       : cross(N, vec3(1.0, 0.0, 0.0)));
        vec3 B = cross(N, T) * vTangentSign;
        vec3 sampledNormal = sampleMaterialTex(mat.texIndices.y, vTexCoord).xyz * 2.0 - 1.0;
        sampledNormal.xy *= mat.emissiveFactorNormalScale.w; // normalScale
        s.normal = normalize(mat3(T, B, N) * sampledNormal);
    }
    else
    {
        s.normal = N;
    }

    return s;
}

// ---- Cook-Torrance BRDF -------------------------------------------------
//
// Algebraically identical to the textbook NDF * G * F / (4 NdotV NdotL) form it
// replaces, restructured so the per-light inner loop — which runs once per light
// per fragment, and is therefore the hot path of the whole renderer — issues as
// few SFU (divide / sqrt / pow) operations as possible. On Ampere those retire at
// a quarter of the FMA rate.
//
// Two rewrites do the work:
//
//   * Smith-GGX's height-correlated numerators cancel the specular denominator
//     exactly. With k = (roughness+1)^2 / 8,
//         G / (4 NdotV NdotL) = 1 / (4 (NdotV(1-k)+k) (NdotL(1-k)+k))
//     because the NdotV / NdotL in each GeometrySchlickGGX numerator divide out.
//     Three divides collapse into one, and the NdotV half is loop-invariant.
//   * Schlick's pow(x, 5.0) becomes x2*x2*x. The NVIDIA compiler lowers pow() to
//     LG2 + EX2 rather than expanding a constant integer exponent, so this trades
//     two SFU ops for two multiplies.
//
// Everything invariant across the light loop (k, a2, the view-side visibility
// term, the diffuse base) is hoisted into BrdfContext, built once per fragment.

struct BrdfContext
{
    vec3  diffuseBase; // albedo * (1 - metallic) / PI
    vec3  F0;
    float a2;          // (roughness^2)^2
    float oneMinusK;   // 1 - k
    float k;
    float visV;        // NdotV * (1-k) + k  — the view half of Smith
};

BrdfContext MakeBrdfContext(vec3 N, vec3 V, vec3 albedo, vec3 F0, float roughness, float metallic)
{
    BrdfContext c;
    float r     = roughness + 1.0;
    c.k         = (r * r) / 8.0;
    c.oneMinusK = 1.0 - c.k;

    float a = roughness * roughness;
    c.a2    = a * a;

    float NdotV = max(dot(N, V), 0.0);
    c.visV      = NdotV * c.oneMinusK + c.k;

    c.diffuseBase = albedo * ((1.0 - metallic) / PI);
    c.F0          = F0;
    return c;
}

vec3 CookTorrance(BrdfContext c, vec3 N, vec3 V, vec3 L, vec3 radiance)
{
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL == 0.0)
        return vec3(0.0);

    vec3  H     = normalize(V + L);
    float NdotH = max(dot(N, H), 0.0);
    float d     = NdotH * NdotH * (c.a2 - 1.0) + 1.0;
    float visL  = NdotL * c.oneMinusK + c.k;

    // The old form added 1e-4 to the denominator to avoid a divide by zero at
    // grazing angles. It is not needed here: roughness is clamped to [0.04, 1]
    // (SampleMaterial), so k >= 0.135 and both visibility terms are >= k > 0.
    float spec = (c.a2 * 0.25) / (PI * d * d * c.visV * visL);

    float fc  = clamp(1.0 - max(dot(H, V), 0.0), 0.0, 1.0);
    float fc2 = fc * fc;
    vec3  F   = c.F0 + (1.0 - c.F0) * (fc2 * fc2 * fc);

    return (c.diffuseBase * (1.0 - F) + F * spec) * radiance * NdotL;
}

// Windowed inverse-square attenuation (Frostbite) — 0 at dist >= radius,
// physically plausible inside. Takes squared distance because nothing here needs
// the distance itself: (dist/radius)^4 is (d2/r2)^2, and the inverse-square term
// wants d2 directly. That lets the caller use one inversesqrt for the light
// direction instead of a length() plus a divide.
float AttenuationSq(float d2, float radiusSq)
{
    float rr    = d2 / radiusSq;          // (dist/radius)^2
    float numer = max(1.0 - rr * rr, 0.0);
    return (numer * numer) / (d2 + 1.0);
}

// ---- Cluster index -------------------------------------------------------

uint ClusterIndex()
{
    uvec3 gridDim    = uFrame.gridDim.xyz;
    vec2  screenSize = uFrame.screenSizeNearFar.xy;
    float nearZ      = uFrame.screenSizeNearFar.z;
    float farZ       = uFrame.screenSizeNearFar.w;

    // Scale/bias form: gridDim/screenSize, 1/log(farZ/nearZ) and log(nearZ) are
    // all frame constants, so what was three divides and two logs per fragment is
    // now two multiplies and one log. Folded CPU-side into uFrame.clusterScale.
    uint ix = clamp(uint(gl_FragCoord.x * uFrame.clusterScale.x), 0u, gridDim.x - 1u);
    uint iy = clamp(uint(gl_FragCoord.y * uFrame.clusterScale.y), 0u, gridDim.y - 1u);

    // Logarithmic depth slice (abs because vViewZ is negative for visible geometry).
    // Fragments nearer than nearZ make the slice negative, and converting a
    // negative float to uint is undefined in GLSL — clamp before the cast, not after.
    float viewDepth = abs(vViewZ);
    float slice     = max(log(viewDepth) * uFrame.clusterScale.z + uFrame.clusterScale.w, 0.0);
    uint  iz        = clamp(uint(slice), 0u, gridDim.z - 1u);

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

    // Supplied by the CPU (Render::FrameConstants::cameraPosition). This used to
    // be recovered here as -transpose(mat3(view)) * view[3] — a mat3 transpose and
    // a matrix-vector product per fragment, for a value fixed for the whole frame.
    vec3 V = normalize(uFrame.cameraPosition.xyz - vWorldPos);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 Lo = vec3(0.0);

    // Everything the BRDF needs that does not vary per light, computed once.
    BrdfContext brdf = MakeBrdfContext(N, V, albedo, F0, roughness, metallic);

    uint dirLightCount = uFrame.lightCounts.x;
    for (uint i = 0u; i < dirLightCount; i++)
    {
        // LightingSystem::Update normalises every direction on upload
        // (SafeDirection), so this only needs the negation.
        vec3 L        = -dirLights[i].directionIntensity.xyz;
        vec3 radiance = dirLights[i].colorPad.xyz * dirLights[i].directionIntensity.w;
        Lo += CookTorrance(brdf, N, V, L, radiance);
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

        // One inversesqrt covers both the direction and the attenuation, which
        // needs only squared distance. The max() guards a fragment sitting exactly
        // on the light, where toLight is zero.
        vec3  toLight  = lPos - vWorldPos;
        float d2       = max(dot(toLight, toLight), 1e-8);
        vec3  L        = toLight * inversesqrt(d2);
        vec3  radiance = lCol * lInt * AttenuationSq(d2, r * r);

        Lo += CookTorrance(brdf, N, V, L, radiance);
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
        float d2      = max(dot(toLight, toLight), 1e-8);
        vec3  L       = toLight * inversesqrt(d2);

        // lDir arrives normalised — LightingSystem::WorldSpotDirection rotates it
        // into world space and renormalises (SafeDirection), which also absorbs
        // any scale in the parent's matrix. Re-normalising here cost an
        // inversesqrt per spot light per fragment for a per-light constant.
        float theta = dot(L, -lDir);
        // A cone authored with inner == outer makes smoothstep divide by zero.
        float cone  = smoothstep(outer, max(inner, outer + 1e-4), theta);
        vec3  radiance = lCol * lInt * AttenuationSq(d2, r * r) * cone;

        Lo += CookTorrance(brdf, N, V, L, radiance);
    }

    // Ambient is scaled by the occlusion map (direct light already accounts for
    // visibility via N·L); emissive is added on top, unlit.
    vec3 color = kAmbient * albedo * surf.occlusion + Lo + surf.emissive;

    // Reinhard tone map + gamma correction
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));

    outColor = vec4(color, 1.0);
}
