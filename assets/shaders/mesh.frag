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

// ---- Material table (mirrors Render::MaterialConstants) ---------------------
// Stage D: materials no longer bind a per-draw constant buffer; every material's
// constants live in one row of a shared structured buffer, and each instance
// carries its row index (vMaterialIndex). This is t0 in MeshPass's layout —
// StructuredBuffer_SRV shares the shaderResource (+0) space with Texture_SRV.
// Same member list and order as the C++ struct, every member a vec4/uvec4 lane,
// so both sides share one layout with no padding — the two must change together.
struct MaterialRow
{
    vec4  baseColorFactor;
    vec4  emissiveFactorNormalScale; // xyz = emissive, w = normalScale
    vec4  metalRoughOcclusion;       // x = metallic, y = roughness, z = occlusion strength, w = specular AA variance clamp
    vec4  specularColorIor;          // rgb = specularColor, w = specularIor
    vec4  openPbrParams;             // x = baseWeight, y = specularWeight, z = baseDiffuseRoughness, w = reserved
    uvec4 flags;                     // x = material flag bits below; yzw reserved
    uvec4 texIndices;                // bindless slots: x=baseColor y=normal z=metalRough w=occlusion
    uvec4 texIndicesEmissive;        // x = emissive bindless slot
};

// Material flag bits (mirror Render::MaterialFlagBits).
const uint kMatFlagHasNormalTexture        = 1u;
const uint kMatFlagEnergyPreservingDiffuse = 2u;
const uint kMatFlagSpecularAntiAliasing    = 4u;

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
    vec4  ambient;            // rgb = linear colour, w = intensity
    // Sun shadows (see Render::FrameConstants and ShadowCascades.hpp).
    uvec4 shadowCounts;       // x = cascade count (0 = no shadows), y = shadowed dir light, z = filter, w = cascade view
    vec4  shadowParams;       // x = UV step between PCF taps, y = blend-band fraction
    // Per cascade: x = view-space distance it ends at, y = constant depth bias
    // in [0,1] depth, z = normal offset in world units.
    vec4  shadowCascade[8];
    mat4  shadowViewProjection[8];
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

const float PI = 3.14159265359;

// Variance of the pixel-reconstruction filter the specular AA kernel assumes,
// in pixels squared. Not a knob: it describes the rasterizer's footprint, which
// no material knows anything about. The per-material clamp is the knob.
const float kSpecAaScreenSpaceVariance = 0.5;

// ---- Material sample ---------------------------------------------------
// Everything that reads the material's textures + factors lives here, so the
// lighting code below is agnostic to how the surface was authored. A later
// bindless transition rewrites only the texture() fetches in this block.
struct Surface
{
    vec3  albedo;    // base colour x baseWeight (OpenPBR base_weight scales the whole base layer)
    float metallic;
    float roughness;
    float occlusion; // 1 = unoccluded
    vec3  emissive;
    vec3  normal;    // world-space, normal-mapped if present
    vec3  specColor; // OpenPBR specular_color
    float specWeight;
    float specIor;
    float diffuseRoughness; // OpenPBR base_diffuse_roughness
    bool  eonDiffuse;       // material opted into the EON diffuse lobe
};

Surface SampleMaterial()
{
    Surface s;

    // Fetch this instance's material row; the vertex shader passed its index.
    MaterialRow mat = materials[vMaterialIndex];

    // baseColor is an sRGB texture, so the sampler already returns linear values
    // (filtered/mip-blended in linear space); multiply by the linear factor.
    vec4 base = sampleMaterialTex(mat.texIndices.x, vTexCoord) * mat.baseColorFactor;
    s.albedo = base.rgb * mat.openPbrParams.x; // baseWeight

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

    s.specColor = mat.specularColorIor.rgb;
    s.specWeight = mat.openPbrParams.y;
    s.specIor = mat.specularColorIor.w;
    s.diffuseRoughness = clamp(mat.openPbrParams.z, 0.0, 1.0);
    s.eonDiffuse = (mat.flags.x & kMatFlagEnergyPreservingDiffuse) != 0u;

    vec3 N = normalize(vNormal);
    if ((mat.flags.x & kMatFlagHasNormalTexture) != 0u)
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

    // Geometric specular antialiasing (Kaplanyan / Tokuyoshi). Where the shading
    // normal swings by more than a pixel's worth across the quad, the specular
    // lobe it implies is narrower than the pixel that has to sample it, and the
    // highlight blinks in and out as the surface moves. Convolving the NDF with
    // the footprint's own normal distribution is the same fix a history buffer
    // would smear over, done in one fragment: variances add, so the filtered
    // lobe is sqrt(a^2 + kernel) in GGX alpha.
    //
    // Derivatives are legal inside this branch because the flag comes from
    // vMaterialIndex, which is per-instance, and a quad never spans two
    // instances — every fragment in it takes the same side. Keeping them inside
    // the branch is what leaves a material with the filter off paying nothing
    // but the flag test.
    if ((mat.flags.x & kMatFlagSpecularAntiAliasing) != 0u)
    {
        vec3  dNdx     = dFdx(s.normal);
        vec3  dNdy     = dFdy(s.normal);
        float variance = kSpecAaScreenSpaceVariance * (dot(dNdx, dNdx) + dot(dNdy, dNdy));
        // The clamp is what keeps a high-curvature surface from being driven to
        // fully rough, which reads as the material turning to chalk.
        float kernel   = min(2.0 * variance, mat.metalRoughOcclusion.w);
        float a        = s.roughness * s.roughness;
        // Widening only ever raises roughness, so the [0.04, 1] range above
        // still holds: the kernel is non-negative and the clamp caps the top.
        s.roughness    = sqrt(sqrt(clamp(a * a + kernel, 0.0, 1.0)));
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

// ---- OpenPBR base layer -------------------------------------------------
//
// Three upgrades over the plain Schlick/Lambert pair, all built per fragment so
// the per-light loop keeps its SFU budget:
//
//   * F0 from the IOR (OpenPBR specular_ior/_weight/_color) instead of a
//     hardcoded 0.04. The defaults are an exact identity: ior 1.5 gives
//     ((1.5-1)/(1.5+1))^2 = 0.04, weight and colour 1 leave it untouched.
//   * F82-tint conductor Fresnel (Kutz et al.). Schlick gains one subtractive
//     lobe peaking near 82 degrees, normalised so specular_color is the
//     reflectance there relative to Schlick. specular_color 1 zeroes the
//     coefficient, so a default metal is bit-for-bit the old Schlick.
//   * Multi-scatter energy compensation. A single-scatter GGX loses the energy
//     that would have bounced again between microfacets, which darkens rough
//     metals; the standard fix scales the lobe by 1 + F0 (1/Ess - 1), with Ess
//     from Karis's analytic environment-BRDF fit rather than a DFG texture.
//
// EON (energy-preserving Oren-Nayar, Portsmouth/Kutz/Hill 2024) replaces
// Lambert when a material sets base_diffuse_roughness. Its per-light half is a
// polynomial and one divide, and it is skipped entirely at the default of 0.
const float kFonC1 = 0.5 - 2.0 / (3.0 * PI);
const float kFonC2 = 2.0 / 3.0 - 28.0 / (15.0 * PI);
const float kEps = 1.0e-7;

// FON directional albedo, the paper's polynomial fit (<0.1% error).
float FonAlbedo(float mu, float r)
{
    float m = 1.0 - mu;
    float GoverPi = m * (0.0571085289 + m * (0.491881867 + m * (-0.332181442 + m * 0.0714429953)));
    return (1.0 + r * GoverPi) / (1.0 + kFonC1 * r);
}

struct BrdfContext
{
    vec3  diffuseBase; // albedo * (1 - metallic) / PI  (Lambert; unused when eon)
    vec3  F0;
    float a2;          // (roughness^2)^2
    float oneMinusK;   // 1 - k
    float k;
    float visV;        // NdotV * (1-k) + k  — the view half of Smith
    float NdotV;
    vec3  f82;         // F82-tint coefficient; zero for dielectrics and untinted metals
    vec3  energyComp;  // multi-scatter compensation for the specular lobe
    // EON diffuse (all zero/unused unless the material opted in).
    bool  eon;
    float eonR;
    vec3  eonSingle;   // rho / PI
    vec3  eonMulti;    // the view-side half of the multi-scatter lobe
};

BrdfContext MakeBrdfContext(vec3 N, vec3 V, Surface s, vec3 F0)
{
    BrdfContext c;
    float r     = s.roughness + 1.0;
    c.k         = (r * r) / 8.0;
    c.oneMinusK = 1.0 - c.k;

    float a = s.roughness * s.roughness;
    c.a2    = a * a;

    float NdotV = max(dot(N, V), 0.0);
    c.NdotV     = NdotV;
    c.visV      = NdotV * c.oneMinusK + c.k;

    vec3 rho      = s.albedo * (1.0 - s.metallic);
    c.diffuseBase = rho * (1.0 / PI);
    c.F0          = F0;

    // F82 tint. The reference angle is mu = 1/7; every power of it is a
    // compile-time constant, so this costs the compiler, not the GPU.
    const float mu   = 1.0 / 7.0;
    const float om   = 1.0 - mu;
    const float om2  = om * om;
    const float om5  = om2 * om2 * om;
    const float om6  = om5 * om;
    vec3 schlickAtMu = F0 + (1.0 - F0) * om5;
    c.f82 = s.metallic * (1.0 - s.specColor) * schlickAtMu * (1.0 / (mu * om6));

    // Ess, the single-scatter directional albedo, from Karis's mobile
    // environment-BRDF fit (the split-sum DFG terms without the texture).
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4  rr   = s.roughness * c0 + c1;
    float a004 = min(rr.x * rr.x, exp2(-9.28 * NdotV)) * rr.x + rr.y;
    vec2  AB   = vec2(-1.04, 1.04) * a004 + rr.zw; // the split-sum scale/bias
    float Ess  = AB.x + AB.y;                      // == the lobe's albedo at F0 = 1
    c.energyComp = vec3(1.0) + F0 * (1.0 / max(Ess, kEps) - 1.0);

    c.eon = s.eonDiffuse;
    if (c.eon)
    {
        float er   = s.diffuseRoughness;
        float AF   = 1.0 / (1.0 + kFonC1 * er);
        float avgE = AF * (1.0 + kFonC2 * er);
        vec3  rhoMs = (rho * rho) * avgE / max(vec3(1.0) - rho * (1.0 - avgE), vec3(kEps));
        float EFo   = FonAlbedo(NdotV, er);

        c.eonR      = er;
        c.eonSingle = rho * (AF / PI);
        c.eonMulti  = (rhoMs / PI) * max(kEps, 1.0 - EFo) / max(kEps, 1.0 - avgE);
    }
    else
    {
        c.eonR      = 0.0;
        c.eonSingle = vec3(0.0);
        c.eonMulti  = vec3(0.0);
    }
    return c;
}

// The dielectric F0 OpenPBR's specular parameters imply. At the defaults
// (ior 1.5, weight 1, colour white) this is exactly vec3(0.04) — the constant
// the shader used before, which is what keeps existing content unchanged.
vec3 DielectricF0(float ior, float weight, vec3 tint)
{
    float r0 = (ior - 1.0) / (ior + 1.0);
    return min(weight * tint * (r0 * r0), vec3(1.0));
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

    float VdotH = max(dot(H, V), 0.0);
    float fc  = clamp(1.0 - VdotH, 0.0, 1.0);
    float fc2 = fc * fc;
    float fc5 = fc2 * fc2 * fc;
    // Schlick, minus the F82 lobe. The subtracted term is multiplies only — no
    // SFU op — and its coefficient is zero unless the material is a tinted
    // metal, so a default material takes the same values it always did.
    vec3  F   = c.F0 + (1.0 - c.F0) * fc5 - c.f82 * (VdotH * fc5 * fc);

    vec3 diffuse = c.diffuseBase;
    if (c.eon)
    {
        // Fujii Oren-Nayar single-scatter plus the paper's multi-scatter lobe.
        float s       = dot(L, V) - NdotL * c.NdotV;
        float sovertF = s > 0.0 ? s / max(NdotL, c.NdotV) : s;
        diffuse = c.eonSingle * (1.0 + c.eonR * sovertF) +
                  c.eonMulti * max(kEps, 1.0 - FonAlbedo(NdotL, c.eonR));
    }

    return (diffuse * (1.0 - F) + F * spec * c.energyComp) * radiance * NdotL;
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

// ---- Sun shadows ---------------------------------------------------------
//
// One texture array, one slice per cascade, sampled through a depth-comparison
// sampler: the hardware tests the reference against four texels and returns the
// blended result, so even the one-tap filter comes back soft to a texel.
//
// Two biases, both scaled CPU-side by the cascade's world-per-texel so one
// setting holds across cascades whose texels differ by an order of magnitude.
// They do different jobs and neither replaces the other: the constant depth bias
// lies about how deep the surface is, which detaches contact shadows if it is
// large; the normal offset moves the lookup out of the surface instead, which is
// what actually fixes acne where the sun grazes.

layout(binding = 7) uniform texture2DArray uShadowCascades;
layout(binding = 129) uniform samplerShadow uShadowSampler;

// Must match Render::ShadowFilter.
const uint kShadowFilterPoint = 0u;
const uint kShadowFilterPcf3  = 1u;
const uint kShadowFilterPcf5  = 2u;
const uint kShadowFilterVogel = 3u;

// 16 rather than a dozen because the disk it has to fill is quoted against a
// reference resolution, so on a map above that size it spans twice as many
// texels and twelve taps start leaving holes in it.
const uint  kVogelTaps          = 16u;
const float kVogelRadiusSteps   = 2.5;
const float kGoldenAngle        = 2.39996323;

float ShadowFetch(vec2 uv, uint cascade, float reference)
{
    return texture(sampler2DArrayShadow(uShadowCascades, uShadowSampler), vec4(uv, float(cascade), reference));
}

// Rotates the Vogel disk per pixel so its 12 taps read as noise rather than as
// 12 repeated copies of the same rosette.
float InterleavedGradientNoise(vec2 position)
{
    return fract(52.9829189 * fract(dot(position, vec2(0.06711056, 0.00583715))));
}

/// Fraction of the sun reaching this fragment through @p cascade: 1 lit, 0 in shadow.
float SampleCascade(uint cascade, vec3 worldPos, vec3 N, float NdotL)
{
    // The offset is scaled by how far from head-on the light is. At normal
    // incidence a surface cannot shadow itself and needs none; at grazing
    // incidence a texel spans a long stretch of the surface and needs all of it.
    float slope = clamp(1.0 - NdotL, 0.0, 1.0);
    vec3  biasedPos = worldPos + N * (uFrame.shadowCascade[cascade].z * slope);

    vec4 lightClip = uFrame.shadowViewProjection[cascade] * vec4(biasedPos, 1.0);
    vec3 ndc = lightClip.xyz / lightClip.w;

    // The backend draws with a flipped viewport, so the map's first texel row is
    // ndc.y == +1 — hence the negative y scale rather than the usual 0.5.
    vec2  uv        = ndc.xy * vec2(0.5, -0.5) + 0.5;
    float reference = ndc.z - uFrame.shadowCascade[cascade].y;

    // Outside the cascade nothing was recorded, so nothing may be claimed. The
    // sampler's white border would answer "lit" anyway; rejecting here also skips
    // the taps.
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || reference > 1.0)
    {
        return 1.0;
    }

    // The UV distance between taps. Deliberately not one texel of this map: a
    // radius quoted in the cascade's own texels would shrink every time the
    // resolution went up, so raising the quality setting would harden the
    // shadow rather than improve it. See Render::FilterTapStepUv.
    float step = uFrame.shadowParams.x;
    uint  filterMode = uFrame.shadowCounts.z;

    if (filterMode == kShadowFilterPoint)
    {
        return ShadowFetch(uv, cascade, reference);
    }

    if (filterMode == kShadowFilterVogel)
    {
        // A dozen taps on a disk, rotated per pixel. Trades the grid's banding
        // for noise, which reads as a softer edge at a third of 5x5's taps.
        float phi = InterleavedGradientNoise(gl_FragCoord.xy) * 6.28318531;
        float sum = 0.0;
        for (uint i = 0u; i < kVogelTaps; ++i)
        {
            float r     = sqrt((float(i) + 0.5) / float(kVogelTaps)) * kVogelRadiusSteps;
            float theta = float(i) * kGoldenAngle + phi;
            sum += ShadowFetch(uv + vec2(r * cos(theta), r * sin(theta)) * step, cascade, reference);
        }
        return sum / float(kVogelTaps);
    }

    if (filterMode == kShadowFilterPcf5)
    {
        float sum = 0.0;
        for (int y = -2; y <= 2; ++y)
        {
            for (int x = -2; x <= 2; ++x)
            {
                sum += ShadowFetch(uv + vec2(float(x), float(y)) * step, cascade, reference);
            }
        }
        return sum * (1.0 / 25.0);
    }

    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            sum += ShadowFetch(uv + vec2(float(x), float(y)) * step, cascade, reference);
        }
    }
    return sum * (1.0 / 9.0);
}

/// The sun's visibility at this fragment, blended across the cascade seam.
/// Reports which cascade it read in @p outCascade, for the debug view.
/// Which cascade covers this fragment. A compare chain over frame constants —
/// cheap enough to run even where the shadow itself is never looked up, which
/// is what lets the debug view still colour a surface the sun cannot reach.
uint SelectCascade(float viewDepth)
{
    uint cascadeCount = uFrame.shadowCounts.x;
    // The last cascade is the fallback: past its split there is nothing further
    // out, and SampleCascade's own bounds check answers "lit" there.
    for (uint i = 0u; i < cascadeCount; ++i)
    {
        if (viewDepth < uFrame.shadowCascade[i].x)
        {
            return i;
        }
    }
    return cascadeCount - 1u;
}

float SunVisibility(vec3 N, vec3 L, uint cascade, float NdotL)
{
    uint  cascadeCount = uFrame.shadowCounts.x;
    float viewDepth    = abs(vViewZ);

    // Past the outermost cascade the band below has already faded to full light,
    // so a lookup out here can only confirm it — at the cost of a matrix
    // multiply and a fetch on every pixel of a distant view.
    if (viewDepth >= uFrame.shadowCascade[cascadeCount - 1u].x)
    {
        return 1.0;
    }

    float visibility = SampleCascade(cascade, vWorldPos, N, NdotL);

    // Fade over the end of the cascade's range, into the next one where there is
    // one and into full light where there is not — so neither the seam between
    // two cascades nor the edge of the last one is a visible line.
    float splitFar  = uFrame.shadowCascade[cascade].x;
    float splitNear = cascade == 0u ? 0.0 : uFrame.shadowCascade[cascade - 1u].x;
    float band      = (splitFar - splitNear) * uFrame.shadowParams.y;
    if (band > 0.0 && viewDepth > splitFar - band)
    {
        float t    = clamp((viewDepth - (splitFar - band)) / band, 0.0, 1.0);
        float next = cascade + 1u < cascadeCount ? SampleCascade(cascade + 1u, vWorldPos, N, NdotL) : 1.0;
        visibility = mix(visibility, next, t);
    }
    return visibility;
}

/// Flat tints for the cascade debug view, one per slice.
vec3 CascadeTint(uint cascade)
{
    if (cascade == 0u) return vec3(1.0, 0.45, 0.45);
    if (cascade == 1u) return vec3(0.45, 1.0, 0.45);
    if (cascade == 2u) return vec3(0.45, 0.6, 1.0);
    return vec3(1.0, 0.95, 0.45);
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
    //
    // These are display values, not radiance — the point is to read the number
    // off the screen. The tone map copies the frame through untouched while a
    // debug view is on (PostProcess::SetTonemapPassthrough), so what is written
    // here is what is shown.
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

    vec3  albedo   = surf.albedo;
    vec3  N        = surf.normal;
    float metallic = surf.metallic;

    // Supplied by the CPU (Render::FrameConstants::cameraPosition). This used to
    // be recovered here as -transpose(mat3(view)) * view[3] — a mat3 transpose and
    // a matrix-vector product per fragment, for a value fixed for the whole frame.
    vec3 V = normalize(uFrame.cameraPosition.xyz - vWorldPos);

    // Dielectrics reflect what their IOR says; metals reflect their base colour.
    vec3 F0 = mix(DielectricF0(surf.specIor, surf.specWeight, surf.specColor), albedo, metallic);
    vec3 Lo = vec3(0.0);

    // Everything the BRDF needs that does not vary per light, computed once.
    BrdfContext brdf = MakeBrdfContext(N, V, surf, F0);

    // Only one directional light casts — the sun. Which one it is arrives as an
    // index into the same buffer this loop walks, so the two halves agree on what
    // index i means without either re-deriving the order.
    uint shadowCascadeCount = uFrame.shadowCounts.x;
    uint shadowLightIndex   = uFrame.shadowCounts.y;
    uint shadowCascade      = 0u;

    uint dirLightCount = uFrame.lightCounts.x;
    for (uint i = 0u; i < dirLightCount; i++)
    {
        // LightingSystem::Update normalises every direction on upload
        // (SafeDirection), so this only needs the negation.
        vec3 L        = -dirLights[i].directionIntensity.xyz;
        vec3 radiance = dirLights[i].colorPad.xyz * dirLights[i].directionIntensity.w;
        if (shadowCascadeCount > 0u && i == shadowLightIndex)
        {
            shadowCascade = SelectCascade(abs(vViewZ));

            // A surface turned away from the sun is already dark by its own
            // geometry: CookTorrance multiplies by N.L and throws the whole term
            // away a line later. Sampling the cascade first told it something it
            // already knew, at sixteen comparison fetches a fragment — and about
            // half the surfaces in any scene face away from a given light.
            float NdotL = dot(N, L);
            if (NdotL > 0.0)
            {
                radiance *= SunVisibility(N, L, shadowCascade, NdotL);
            }
        }
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
    vec3 ambient = uFrame.ambient.rgb * uFrame.ambient.w;
    vec3 color = ambient * albedo * surf.occlusion + Lo + surf.emissive;

    // Cascade view: tint the lit result rather than replacing it, so a split
    // distance and a blend band are readable against the geometry that provoked
    // them. Goes through the tone map like any other radiance — these are bands
    // to look at, not numbers to read off the screen.
    if (uFrame.shadowCounts.w != 0u && shadowCascadeCount > 0u)
    {
        color *= CascadeTint(shadowCascade);
    }

    // Linear radiance, unbounded. The scene target is float and the tone map is
    // its own pass (tonemap.frag), so nothing here clamps or encodes: a value
    // over 1 is a real highlight until something downstream decides what it
    // looks like, and the MSAA resolve averages radiance rather than perception.
    outColor = vec4(color, 1.0);
}
