#version 450
#extension GL_EXT_nonuniform_qualifier : require
// textureSize and texelFetch on a texture with no sampler bound to it, which is
// how the margin debug view reads the shadow map's stored depth: every other
// read of that texture goes through a comparison sampler and returns a fraction
// rather than the depth itself.
#extension GL_EXT_samplerless_texture_functions : require

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
    vec4  openPbrParams;             // x = baseWeight, y = specularWeight, z = baseDiffuseRoughness, w = alphaCutoff (0 unless masked)
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
    // The indirect term, as the provider that answered this frame left it (see
    // Render::IndirectConstants and IndirectRadiance below).
    vec4  indirectSky;        // rgb = radiance onto a surface facing straight up, w unused
    vec4  indirectGround;     // rgb = the same for one facing straight down, w unused
    // Sun shadows (see Render::FrameConstants and ShadowCascades.hpp).
    uvec4 shadowCounts;       // x = cascade count (0 = no shadows), y = shadowed dir light, z = filter, w = cascade view
    // Local-light shadows (see Render::FrameConstants and LocalShadowPass.hpp).
    // x = ShadowFilter for the atlas, y = 1 while any local light holds a tile.
    // The per-view biases and tap step ride in the view table, not here: a
    // demoted tile is biased differently from a full-size one, and a frame
    // constant could not say so.
    uvec4 localShadowCounts;
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

// shadowView is the light's first row in the shadow view table below, or
// kNoShadowView when it holds no atlas tile this frame. A point light's six
// faces are that row and the five after it.

struct PointLight
{
    vec4  positionRadius;
    vec4  colorIntensity;
    uvec4 shadowView;
};

struct SpotLight
{
    vec4  positionRadius;
    vec4  directionInner;
    vec4  colorIntensity;
    float outerCutoff;
    uint  shadowView;
    float _p0, _p1;
};

// Must match Render::kNoShadowView.
const uint kNoShadowView = 0xFFFFFFFFu;

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

// ---- Shadow view table (mirrors Render::ShadowViewGpu) --------------------
// One row per shadow map view of the frame, whichever kind of map produced it.
// The sun's cascades ride in the frame constants instead, because there are at
// most eight of them and they are read by every shadowed fragment; a local
// light's view is read only where that light reaches, and there may be a hundred
// and sixty of them.
struct ShadowViewRow
{
    mat4 viewProjection;
    vec4 uvScaleOffset; // xy = scale, zw = offset: the tile's rectangle of the atlas
    vec4 params;        // x = depth bias, y = normal offset, z = tap step, w = array slice
    vec4 clampUv;       // xy = min, zw = max UV a lookup may reach
};

layout(std430, binding = 8) readonly buffer ShadowViews { ShadowViewRow shadowViews[]; };

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
    float alpha;            // base colour alpha; read only by the ASSISI_ALPHA_MASK build
    float alphaCutoff;      // alpha below this kills the fragment; 0 unless the material is masked
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
    s.alpha = base.a;
    s.alphaCutoff = mat.openPbrParams.w;

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

    // A back face only rasterizes at all under a double-sided pipeline, and its
    // normal still points the way the front face does — away from whoever is
    // looking at it. Flipping it here is what makes an interior read as a lit
    // surface instead of a black shell. Unconditional because a single-sided
    // pipeline has no back faces for it to catch.
    vec3 N = normalize(vNormal);
    if (!gl_FrontFacing)
    {
        N = -N;
    }
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
// Three biases, in the order they do work. The rasterizer offsets each caster by
// its own depth slope while the map is drawn, which is the only one that can see
// the polygon being recorded. The sample side then moves the lookup along the
// surface normal by the sine of the light's incidence, which is what covers a
// texel's worth of surface at any angle without running away at the grazing end.
// A small constant covers what is left: the depth format's quantisation, and a
// receiver curved across the kernel.
//
// Both sample-side terms are scaled CPU-side by the cascade's world-per-texel,
// so one setting holds across cascades whose texels differ by an order of
// magnitude. Neither is asked to cover slope — that is the rasterizer's job, and
// a constant large enough to do it is a constant large enough to detach every
// shadow from its contact edge.

layout(binding = 7) uniform texture2DArray uShadowCascades;
layout(binding = 129) uniform samplerShadow uShadowSampler;
// Every spot and point face of the frame, in one texture. Sampled through the
// same comparison sampler the cascades use — the filtering is identical, only
// the rectangle differs.
layout(binding = 9) uniform texture2D uShadowAtlas;

// Must match Render::ShadowDebugView.
const uint kShadowDebugCascades = 1u;
const uint kShadowDebugMargin   = 2u;
const uint kShadowDebugTaps     = 3u;

// Must match Render::ShadowFilter.
const uint kShadowFilterPoint = 0u;
const uint kShadowFilterPcf3  = 1u;
const uint kShadowFilterPcf5  = 2u;
const uint kShadowFilterVogel = 3u;

// Half-widths of the two grid kernels, in taps: a 3x3 reaches one texel from its
// centre and a 5x5 reaches two. The filter's own name says the grid; these say
// the reach, which is what both the loop bounds and the tap count come from.
//
// Must match Render::kPcf3FilterRadiusTaps and kPcf5FilterRadiusTaps, and
// kVogelRadiusSteps below must match kVogelFilterRadiusTaps. The CPU sizes the
// penumbra cap and the atlas tile's clamp inset from those, so a kernel that
// reached further here than the CPU assumed would read past its own tile.
const int kPcf3Radius = 1;
const int kPcf5Radius = 2;

// Taps in a square kernel of half-width @p radius, as the divisor that averages
// them. Derived rather than written down beside each loop, so a radius and its
// divisor cannot drift apart.
float PcfTapCount(int radius)
{
    int side = 2 * radius + 1;
    return float(side * side);
}

// 16 rather than a dozen because the disk it has to fill is quoted against a
// reference resolution, so on a map above that size it spans twice as many
// texels and twelve taps start leaving holes in it.
const uint  kVogelTaps          = 16u;
const float kVogelRadiusSteps   = 2.5;
const float kGoldenAngle        = 2.39996323;

// The sixteen taps split into a probe and the rest. The probe's four are picked
// for coverage — spread around the disk, and spanning its radius from near the
// centre to the rim — so a kernel that straddles a shadow edge almost always
// lands at least one probe tap on each side of it. Together the two lists are
// exactly the taps 0..15, so summing both walks the same disk the single loop
// did.
const uint kVogelProbe[4] = uint[](0u, 4u, 11u, 15u);
const uint kVogelRest[12] = uint[](1u, 2u, 3u, 5u, 6u, 7u, 8u, 9u, 10u, 12u, 13u, 14u);

/// Where @p worldPos lands in @p cascade's map: UV in xy, and in z the depth the
/// comparison is made against.
///
/// The cascade projection is orthographic, so w is exactly 1 and this mapping is
/// affine. That is what makes the plane fit below exact for a flat receiver
/// rather than a first-order approximation of one.
vec3 ShadowCoord(uint cascade, vec3 worldPos)
{
    vec4 clip = uFrame.shadowViewProjection[cascade] * vec4(worldPos, 1.0);
    vec3 ndc  = clip.xyz / clip.w;
    // The backend draws with a flipped viewport, so the map's first texel row is
    // ndc.y == +1 — hence the negative y scale rather than the usual 0.5.
    return vec3(ndc.xy * vec2(0.5, -0.5) + 0.5, ndc.z);
}

float ShadowTap(vec2 uv, uint cascade, float reference)
{
    return texture(sampler2DArrayShadow(uShadowCascades, uShadowSampler), vec4(uv, float(cascade), reference));
}

// Rotates the Vogel disk per pixel so its taps read as noise rather than as
// repeated copies of the same rosette.
float InterleavedGradientNoise(vec2 position)
{
    return fract(52.9829189 * fract(dot(position, vec2(0.06711056, 0.00583715))));
}

/// Tap @p i of the kVogelTaps-point Vogel disk, rotated by @p phi and spaced by
/// @p stepUv per radius step. The position depends only on the index, so the
/// disk is the same whichever order its taps are visited in.
float VogelTap(uint i, vec2 uv, uint cascade, float reference, float stepUv, float phi)
{
    float r     = sqrt((float(i) + 0.5) / float(kVogelTaps)) * kVogelRadiusSteps;
    float theta = float(i) * kGoldenAngle + phi;
    return ShadowTap(uv + vec2(r * cos(theta), r * sin(theta)) * stepUv, cascade, reference);
}

/// The normal of the surface that was actually rasterized, facing the viewer.
///
/// The interpolated vertex normal, flipped for a back face exactly as the shaded
/// normal is. Both have to agree about which way a surface faces or they disagree
/// about whether the sun reaches it — and a double-sided shell is nothing but
/// back faces from inside, so the disagreement is the whole of what one sees
/// there. The one geometric normal in the shader, so a diagnostic cannot answer
/// for a surface the shading orients differently.
vec3 GeometricNormal()
{
    vec3 n = normalize(vNormal);
    return gl_FrontFacing ? n : -n;
}

/// The depth of the nearest thing the map recorded in front of @p reference
/// around @p uv, or @p reference itself where nothing is.
///
/// Read rather than compared: the comparison sampler answers whether something
/// is in front, and what is wanted here is how far in front.
///
/// The fragment's own texel and no other. A wider search reads the receiver
/// itself: a surface the light grazes recedes so fast in depth that its own
/// neighbouring texels lie in front of this fragment's reference, and they
/// qualify as occluders a hair away. Sizing a kernel from that collapses it to
/// one tap on exactly the surfaces whose self-shadowing the kernel was averaging
/// away, which prints as stripes. The one texel under the lookup cannot be the
/// receiver, because the receiver is what the reference came from.
float NearestBlockerDepth(vec2 uv, uint cascade, float reference)
{
    ivec3 size = textureSize(uShadowCascades, 0);
    ivec2 texel = clamp(ivec2(clamp(uv, vec2(0.0), vec2(1.0)) * vec2(size.xy)), ivec2(0), size.xy - 1);
    return min(reference, texelFetch(uShadowCascades, ivec3(texel, int(cascade)), 0).r);
}

/// Fraction of the sun reaching this fragment through @p cascade: 1 lit, 0 in shadow.
///
/// @p N is the geometric normal, not the shaded one. The offset below has to
/// move the lookup off the surface that is actually in the depth map, and a
/// normal map describes a surface no caster ever rasterized: biasing along it
/// tilts the lookup by whatever the texture says, which reads as lit speckle in
/// the pattern of the normal map inside an otherwise correct shadow.
float SampleCascade(uint cascade, vec3 worldPos, vec3 N, float NdotL)
{
    // Along the normal, scaled by the sine of the light's incidence.
    //
    // Sine because that is the geometry: a texel covers a fixed distance across
    // the map, and the depth the surface gains over it is that distance times
    // the tangent of the incidence — but the offset only has to clear the
    // surface, not match its depth, and moving along the normal covers the same
    // error with the sine. The difference matters at the edge: tangent runs away
    // to infinity as the light goes edge-on and needs clamping, sine tops out at
    // one, so the offset is bounded by construction and the worst it can ever do
    // is a filter radius of leak.
    //
    // Scaled by the filter's reach as well as the texel, because the tap that
    // has to clear the surface is the outermost one, not the centre.
    float sinTheta  = sqrt(max(0.0, 1.0 - NdotL * NdotL));
    vec3  biasedPos = worldPos + N * (uFrame.shadowCascade[cascade].z * sinTheta);

    vec3 coord = ShadowCoord(cascade, biasedPos);
    vec2 uv    = coord.xy;

    // Outside the cascade nothing was recorded, so nothing may be claimed. The
    // sampler's white border would answer "lit" anyway; rejecting here also skips
    // the taps.
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || coord.z > 1.0)
    {
        return 1.0;
    }

    // What is left for a constant to cover once the offset has moved the lookup
    // and the rasterizer has biased the caster by its own slope: the depth
    // format's quantisation, and a receiver curved across the kernel. Neither
    // grows with the angle to the light, which is why this stays small.
    float reference = coord.z - uFrame.shadowCascade[cascade].y;

    uint filterMode = uFrame.shadowCounts.z;

    // The centre tap needs no kernel width, so it answers before the blocker
    // search below spends a fetch sizing a kernel it would never use.
    if (filterMode == kShadowFilterPoint)
    {
        return ShadowTap(uv, cascade, reference);
    }

    // How wide to filter, asked of the geometry rather than assumed.
    //
    // The sun is not a point: it subtends about half a degree, so an occluder d
    // away throws a penumbra of roughly d/216, and that — not a texel, not a
    // constant — is how soft this fragment's shadow should be. The map knows d:
    // it is the distance between what the map recorded and the fragment itself.
    //
    // This is what closes the leak rather than narrowing it. A kernel escapes an
    // occluder's silhouette only if it reaches as far as the silhouette is away,
    // and a fragment tucked under an occluder is by definition close to it — the
    // inside of a box's ceiling is a hand's breadth from the wall beneath it, so
    // the honest penumbra there is under a millimetre and there is nothing left
    // to reach past. Every earlier width was a guess standing in for d: a texel
    // count, then a constant in metres, then that constant over the incidence.
    // Each narrowed the band, because each was smaller than the last, and none
    // could close it, because none of them knew how far the occluder was.
    //
    // Where nothing is recorded in front of the fragment there is no occluder to
    // measure, and the kernel stays at the cap: that is both the lit side of an
    // edge, which keeps its softness, and a surface shadowing itself, whose
    // acne the full kernel is what averages away.
    float texelStep  = uFrame.shadowParams.x;
    float capStep    = min(texelStep, uFrame.shadowParams.z * max(NdotL, kEps) / uFrame.shadowCascade[cascade].w);
    float blockerNdc = reference - NearestBlockerDepth(uv, cascade, reference);
    float step       = blockerNdc > 0.0 ? min(capStep, uFrame.shadowParams.w * blockerNdc) : capStep;

    if (filterMode == kShadowFilterVogel)
    {
        float phi = InterleavedGradientNoise(gl_FragCoord.xy) * 6.28318531;

        // The probe taps first. A tap that lands wholly lit or wholly shadowed
        // returns exactly 1 or 0 — the comparison sampler blends only across a
        // texel — so a unanimous probe means the kernel almost certainly does
        // not straddle an edge, and most of any frame it does not. The twelve
        // remaining taps run only where the probe disagrees, with the same
        // positions and the same per-pixel rotation, so a penumbra keeps its
        // noise pattern. The probe can answer wrongly only where the rest
        // would have disagreed with a unanimous four — the outermost fringe of
        // a penumbra — and the rotation scatters that per pixel rather than
        // letting it band.
        float sum = 0.0;
        for (uint i = 0u; i < 4u; ++i)
        {
            sum += VogelTap(kVogelProbe[i], uv, cascade, reference, step, phi);
        }
        if (sum == 0.0)
        {
            return 0.0;
        }
        if (sum == 4.0)
        {
            return 1.0;
        }
        for (uint i = 0u; i < 12u; ++i)
        {
            sum += VogelTap(kVogelRest[i], uv, cascade, reference, step, phi);
        }
        return sum / float(kVogelTaps);
    }

    // The two grid filters differ in nothing but their reach, so they are one
    // loop over it rather than two loops that have to be kept identical.
    int   radius = filterMode == kShadowFilterPcf5 ? kPcf5Radius : kPcf3Radius;
    float sum    = 0.0;
    for (int y = -radius; y <= radius; ++y)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            sum += ShadowTap(uv + vec2(float(x), float(y)) * step, cascade, reference);
        }
    }
    return sum / PcfTapCount(radius);
}


/// What the map holds at this fragment's own lookup, against what the fragment
/// compares — the two numbers the comparison sampler comes between and never
/// shows. Both in the cascade's [0, 1] depth; the difference times its depth
/// range is metres, and a positive one is how far in front of the receiver the
/// recorded occluder sits.
///
/// The centre tap only, fetched rather than filtered: a filtered read answers
/// what the shadow looks like, and the question here is what the map contains.
struct ShadowProbe
{
    float stored;
    float reference;
    bool  outside;
};

ShadowProbe ProbeCascade(uint cascade, vec3 worldPos, vec3 N, float NdotL)
{
    ShadowProbe probe;
    float sinTheta  = sqrt(max(0.0, 1.0 - NdotL * NdotL));
    vec3  biasedPos = worldPos + N * (uFrame.shadowCascade[cascade].z * sinTheta);

    vec3 coord     = ShadowCoord(cascade, biasedPos);
    probe.outside  = any(lessThan(coord.xy, vec2(0.0))) || any(greaterThan(coord.xy, vec2(1.0))) || coord.z > 1.0;
    probe.reference = coord.z - uFrame.shadowCascade[cascade].y;

    ivec3 size = textureSize(uShadowCascades, 0);
    ivec2 texel = ivec2(clamp(coord.xy, vec2(0.0), vec2(1.0)) * vec2(size.xy));
    texel = clamp(texel, ivec2(0), size.xy - 1);
    probe.stored = texelFetch(uShadowCascades, ivec3(texel, int(cascade)), 0).r;
    return probe;
}

/// Which cascade covers this fragment. A count over frame constants — cheap
/// enough to run even where the shadow itself is never looked up, which is
/// what lets the debug view still colour a surface the sun cannot reach.
uint SelectCascade(float viewDepth)
{
    // The splits ascend (each cascade ends further out than the last), so the
    // covering cascade is the number of splits at or below viewDepth. Counting
    // rather than searching keeps the trip count uniform: no break, so a warp
    // straddling a seam does not diverge here.
    //
    // Only the first count-1 splits are counted, so anything past the last
    // split lands on the last cascade — the fallback: out there SampleCascade's
    // own bounds check answers "lit".
    uint cascadeCount = uFrame.shadowCounts.x;
    uint cascade      = 0u;
    for (uint i = 0u; i + 1u < cascadeCount; ++i)
    {
        cascade += viewDepth >= uFrame.shadowCascade[i].x ? 1u : 0u;
    }
    return cascade;
}

/// The sun's visibility at this fragment, blended across the cascade seam.
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
        float t = clamp((viewDepth - (splitFar - band)) / band, 0.0, 1.0);
        if (cascade + 1u < cascadeCount)
        {
            // Darker of the two, never brighter. The next cascade covers more
            // world with the same texels, so where the two disagree it is the
            // one that could not resolve what this one did — and the direction
            // of that failure is always toward light, because an occluder it
            // cannot represent is an occluder it does not record. A hollow
            // shape a few centimetres thick is the clear case: a cascade whose
            // texel is as wide as the walls has nowhere to put them, reports
            // the inside as lit, and blending toward that answer carries
            // daylight into a sealed box a third of a cascade before the
            // boundary that would have justified it.
            //
            // Only the disagreement is dropped. Where both cascades agree the
            // mix is the mix, which is what the band is for: the seam it hides
            // is a change in how sharply an edge resolves, not a change in
            // whether the light arrives.
            //
            // At zero the neighbour has nothing to say: min() can only keep or
            // lower the answer, and mixing zero toward zero is zero, so the
            // second cascade is not sampled at all. This is the one early-out
            // the min() direction permits — a fully lit fragment can still be
            // darkened by the neighbour, which is the hollow-shape case the
            // min() exists for, so it must still pay for the second sample.
            if (visibility > 0.0)
            {
                float next = SampleCascade(cascade + 1u, vWorldPos, N, NdotL);
                visibility = mix(visibility, min(visibility, next), t);
            }
        }
        else
        {
            // Past the last cascade there is no further view to disagree with —
            // the shadow simply stops being recorded, and fading to lit is that
            // range ending rather than two cascades differing.
            visibility = mix(visibility, 1.0, t);
        }
    }

    return visibility;
}

// ---- Local-light shadows --------------------------------------------------
//
// A spot light's map and one face of a point light's cube are the same thing: a
// perspective view into a rectangle of one shared atlas. The rectangle is why
// every lookup here clamps — the sampler's own clamp is to the whole texture, so
// a filter tap that walked off a tile would read the depth of whatever light
// sits beside it and report that light's occluders as this one's. The clamp
// rectangle is the tile inset by the kernel's reach, and the faces are drawn
// wider than they need to be so that inset costs no coverage.
//
// The biases work exactly as the cascades' do, and arrive already scaled by the
// tile's own texel — so a light demoted to a quarter-size tile is biased for the
// map it got rather than the map the setting names.

float LocalShadowTap(vec2 uv, vec4 clampUv, float reference)
{
    return texture(sampler2DShadow(uShadowAtlas, uShadowSampler),
                   vec3(clamp(uv, clampUv.xy, clampUv.zw), reference));
}

// The six faces of a point light's cube, in the order the CPU wrote its views
// into the frame's table. Must match Render::PointLightFace — these numbers are
// the contract between the two, and a disagreement samples a neighbouring face's
// depth for every fragment rather than failing anywhere.
const uint kFacePositiveX = 0u;
const uint kFaceNegativeX = 1u;
const uint kFacePositiveY = 2u;
const uint kFaceNegativeY = 3u;
const uint kFacePositiveZ = 4u;
const uint kFaceNegativeZ = 5u;

/// Which of a point light's six faces @p direction falls in, where @p direction
/// points from the light toward what it is lighting.
///
/// Must agree with Render::PointLightFaceOf, comparison for comparison: the
/// inclusive tests are what put a direction exactly on a seam onto one face
/// rather than the last, and either face of a seam holds the geometry because
/// the faces are drawn overlapping.
uint PointLightFace(vec3 direction)
{
    vec3 m = abs(direction);
    if (m.x >= m.y && m.x >= m.z)
    {
        return direction.x >= 0.0 ? kFacePositiveX : kFaceNegativeX;
    }
    if (m.y >= m.z)
    {
        return direction.y >= 0.0 ? kFacePositiveY : kFaceNegativeY;
    }
    return direction.z >= 0.0 ? kFacePositiveZ : kFaceNegativeZ;
}

/// Fraction of a local light reaching @p worldPos through view @p viewIndex:
/// 1 lit, 0 in shadow.
///
/// @p N is the geometric normal, for the same reason the cascade lookup wants
/// one: the depth map holds the geometry a caster rasterized, and offsetting
/// along a normal map's surface tilts the lookup by whatever the texture says.
float LocalVisibility(uint viewIndex, vec3 worldPos, vec3 N, float NdotL, float axisDepth)
{
    ShadowViewRow view = shadowViews[viewIndex];

    // Both biases are quoted per unit of distance from the light, because a
    // perspective texel is a solid angle: what it covers grows as you move away.
    // A cascade can bake its bias in because an orthographic texel does not.
    //
    // @p axisDepth is the distance along this view's own axis, not the straight
    // line to the light — that is what a texel's footprint actually scales with,
    // and at a face's corner the two differ by a factor of the square root of
    // three. The offset moves the lookup sideways, so an over-estimate here walks
    // it out from under its occluder.
    float sinTheta = sqrt(max(1.0 - NdotL * NdotL, 0.0));
    vec3  biasedPos = worldPos + N * (view.params.y * axisDepth * sinTheta);

    vec4 clip = view.viewProjection * vec4(biasedPos, 1.0);
    // Behind the light's near plane, or outside the frustum this face recorded.
    // Lit rather than shadowed: the map holds nothing about out here, and
    // guessing dark would print a hard rectangle at the edge of every face.
    if (clip.w <= 0.0)
    {
        return 1.0;
    }
    vec3 ndc = clip.xyz / clip.w;
    if (any(greaterThan(abs(ndc.xy), vec2(1.0))) || ndc.z > 1.0 || ndc.z < 0.0)
    {
        return 1.0;
    }

    // Into the tile, then into the atlas. The y scale is negative for the same
    // reason the cascade lookup's is: the backend draws with a flipped viewport,
    // so the map's first texel row is ndc.y == +1.
    vec2 tileUv = ndc.xy * vec2(0.5, -0.5) + 0.5;
    vec2 uv     = tileUv * view.uvScaleOffset.xy + view.uvScaleOffset.zw;

    // params.x is the bias times a distance, so this divide is what evaluates it
    // where the receiver actually is. Guarded because a receiver sitting on the
    // light would divide by zero, and the near plane has already excluded it from
    // the map anyway.
    float reference = ndc.z - view.params.x / max(axisDepth, 1e-4);
    float stepUv    = view.params.z;
    vec4  clampUv   = view.clampUv;

    uint filterMode = uFrame.localShadowCounts.x;
    if (filterMode == kShadowFilterPoint)
    {
        return LocalShadowTap(uv, clampUv, reference);
    }

    if (filterMode == kShadowFilterVogel)
    {
        float phi = InterleavedGradientNoise(gl_FragCoord.xy) * 6.28318531;
        float sum = 0.0;
        for (uint i = 0u; i < kVogelTaps; i++)
        {
            float r     = sqrt((float(i) + 0.5) / float(kVogelTaps)) * kVogelRadiusSteps;
            float theta = float(i) * kGoldenAngle + phi;
            sum += LocalShadowTap(uv + vec2(r * cos(theta), r * sin(theta)) * stepUv, clampUv, reference);
        }
        return sum / float(kVogelTaps);
    }

    int   radius = filterMode == kShadowFilterPcf5 ? kPcf5Radius : kPcf3Radius;
    float sum    = 0.0;
    for (int y = -radius; y <= radius; ++y)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            sum += LocalShadowTap(uv + vec2(float(x), float(y)) * stepUv, clampUv, reference);
        }
    }
    return sum / PcfTapCount(radius);
}

/// The same, for a spot light: one view, or no shadow at all.
///
/// Its view looks down the cone's own axis, so the depth along that axis is what
/// the fragment projects to — the same quantity the perspective divide produces.
float SpotVisibility(uint shadowView, vec3 spotDir, vec3 lightPos, vec3 worldPos, vec3 N, float NdotL)
{
    if (shadowView == kNoShadowView)
    {
        return 1.0;
    }
    float axisDepth = max(dot(worldPos - lightPos, spotDir), 0.0);
    return LocalVisibility(shadowView, worldPos, N, NdotL, axisDepth);
}

/// The same, for a point light: the face the fragment falls in, of the six this
/// light's first view begins.
float PointVisibility(uint firstView, vec3 lightPos, vec3 worldPos, vec3 N, float NdotL)
{
    if (firstView == kNoShadowView)
    {
        return 1.0;
    }
    vec3 d = worldPos - lightPos;
    // The chosen face is the major axis by construction, so the largest component
    // of |d| is the depth along that face's own axis — no branch on the face, and
    // exact rather than the straight-line distance, which at a face's corner
    // over-states it by the square root of three.
    float axisDepth = max(max(abs(d.x), abs(d.y)), abs(d.z));
    return LocalVisibility(firstView + PointLightFace(d), worldPos, N, NdotL, axisDepth);
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

// ---- Indirect lighting ---------------------------------------------------

// Radiance arriving from everything that is not a light, for a surface at
// @worldPos facing @N. The GPU half of the indirect-lighting seam: this is the
// only expression in the shader that answers it, and swapping the provider that
// filled uFrame.indirectSky/Ground swaps the answer without any other line here
// changing. Mirrors Render::EvaluateIndirect.
//
// The position is unread by a hemisphere, whose answer is the same everywhere,
// and is the parameter a baked or probe-based provider needs — the question is
// "here, facing this way" whether or not today's answer uses both halves of it.
vec3 IndirectRadiance(vec3 N, vec3 worldPos)
{
    // Saturated so a normal that is not quite unit length cannot extrapolate
    // past either hemisphere and hand back a negative radiance.
    float upward = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    return mix(uFrame.indirectGround.rgb, uFrame.indirectSky.rgb, upward);
}

// ---- Main -----------------------------------------------------------------

void main()
{
    Surface surf = SampleMaterial();

#ifdef ASSISI_ALPHA_MASK
    // The cutout test, and the only difference between this build of the shader
    // and the one the opaque pipeline uses. It lives behind the define because a
    // fragment shader that *can* discard loses early depth rejection for every
    // draw through its pipeline — including the opaque geometry that has no alpha
    // to test. Before the lighting and before the debug views: a killed fragment
    // is not part of the surface in either.
    if (surf.alpha < surf.alphaCutoff)
    {
        discard;
    }
#endif

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
            //
            // The shadow reads the geometric normal, not the shaded one. What is
            // in the depth map is the geometry a caster rasterized; a normal map
            // describes a surface that was never drawn into it, so biasing along
            // it tilts the lookup by whatever the texture says and lights
            // speckles in the pattern of the map inside a correct shadow. The
            // lighting below keeps the shaded normal — that part is not geometry.
            // Flipped for a back face, exactly as the shaded normal is. A
            // double-sided surface seen from behind is lit by the shaded normal
            // and would be tested for shadow by the unflipped one, so the two
            // disagree about which way the surface faces: the test fails, the
            // multiply below is skipped, and the fragment keeps the sun's full
            // radiance with no shadow applied at all. Every interior of a
            // double-sided shell is then lit as though nothing stood between it
            // and the sun — a cutout casts no pattern into its own inside, which
            // is the one place its pattern is supposed to land.
            vec3  Ng     = GeometricNormal();
            float NgdotL = dot(Ng, L);
            float NdotL  = dot(N, L);
            if (NdotL > 0.0 && NgdotL > 0.0)
            {
                radiance *= SunVisibility(Ng, L, shadowCascade, NgdotL);
            }
            else if (NdotL > 0.0)
            {
                // Lit by the shaded normal but facing away by the geometric one,
                // which a normal map can do at a grazing angle. Nothing in the
                // depth map describes that surface, so the honest answer is that
                // it is not lit rather than that it is lit and unshadowed.
                radiance = vec3(0.0);
            }
        }
        Lo += CookTorrance(brdf, N, V, L, radiance);
    }

    // Read once rather than per light: it is the same frame constant for every
    // light in both loops, and the loops are where a redundant uniform fetch is
    // paid for as many times as the cluster lists anything.
    uint localShadows = uFrame.localShadowCounts.y;

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

        // One inversesqrt covers both the direction and the attenuation, which
        // needs only squared distance. The max() guards a fragment sitting exactly
        // on the light, where toLight is zero.
        vec3  toLight = lPos - vWorldPos;
        float d2      = max(dot(toLight, toLight), 1e-8);

        // The cluster lists a light for every fragment of every froxel its
        // sphere touches, so a fragment can sit past the radius while its
        // cluster still names the light. Attenuation is exactly zero out there,
        // and the BRDF of zero radiance is zero — skip it.
        float att = AttenuationSq(d2, r * r);
        if (att == 0.0)
        {
            continue;
        }

        vec3  lCol     = pointLights[li].colorIntensity.xyz;
        float lInt     = pointLights[li].colorIntensity.w;
        vec3  L        = toLight * inversesqrt(d2);
        vec3  radiance = lCol * lInt * att;

        // The same two-normal rule the sun's lookup follows, and for the same
        // reasons: a surface facing away is already dark by its own geometry, so
        // sampling first would tell the BRDF what it is about to work out for
        // itself; and where a normal map lights a surface the geometry faces
        // away from, nothing in the depth map describes that surface, so the
        // honest answer is unlit rather than lit-and-unshadowed.
        if (localShadows == 1u)
        {
            vec3  Ng     = GeometricNormal();
            float NgdotL = dot(Ng, L);
            float NdotL  = dot(N, L);
            if (NdotL > 0.0 && NgdotL > 0.0)
            {
                radiance *= PointVisibility(pointLights[li].shadowView.x, lPos, vWorldPos, Ng, NgdotL);
            }
            else if (NdotL > 0.0)
            {
                radiance = vec3(0.0);
            }
        }

        Lo += CookTorrance(brdf, N, V, L, radiance);
    }

    for (uint i = 0u; i < spotCount; i++)
    {
        uint  li     = lightIndexList[kSpotIndexBase + spotOffset + i];
        vec3  lPos   = spotLights[li].positionRadius.xyz;
        float r      = spotLights[li].positionRadius.w;
        vec3  lDir   = spotLights[li].directionInner.xyz;
        float inner  = spotLights[li].directionInner.w;
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

        // The cluster clips the cone against froxels, not fragments, so most
        // fragments of a froxel the cone grazes are outside it — and outside
        // the cone or past the radius the radiance is exactly zero. Skip the
        // BRDF for both.
        float att = AttenuationSq(d2, r * r);
        if (att == 0.0 || cone == 0.0)
        {
            continue;
        }

        vec3  lCol     = spotLights[li].colorIntensity.xyz;
        float lInt     = spotLights[li].colorIntensity.w;
        vec3  radiance = lCol * lInt * att * cone;

        if (localShadows == 1u)
        {
            vec3  Ng     = GeometricNormal();
            float NgdotL = dot(Ng, L);
            float NdotL  = dot(N, L);
            if (NdotL > 0.0 && NgdotL > 0.0)
            {
                radiance *= SpotVisibility(spotLights[li].shadowView, lDir, lPos, vWorldPos, Ng, NgdotL);
            }
            else if (NdotL > 0.0)
            {
                radiance = vec3(0.0);
            }
        }

        Lo += CookTorrance(brdf, N, V, L, radiance);
    }

    // The indirect term is scaled by the occlusion map (direct light already
    // accounts for visibility via N·L); emissive is added on top, unlit. No
    // further factor of pi: the provider answers with the cosine-weighted mean
    // radiance, which is exactly what a Lambertian albedo multiplies.
    vec3 indirect = IndirectRadiance(N, vWorldPos);
    vec3 color = indirect * albedo * surf.occlusion + Lo + surf.emissive;

    // Cascade view: tint the lit result rather than replacing it, so a split
    // distance and a blend band are readable against the geometry that provoked
    // them. Goes through the tone map like any other radiance — these are bands
    // to look at, not numbers to read off the screen.
    if (uFrame.shadowCounts.w == kShadowDebugCascades && shadowCascadeCount > 0u)
    {
        color *= CascadeTint(shadowCascade);
    }

    // Margin view: how far in front of this fragment the map's recorded occluder
    // sits, in metres, replacing the shading rather than tinting it. The one
    // thing the comparison sampler cannot be asked — it returns a fraction, and
    // a fraction is the same whether the map holds the wrong occluder or the
    // right one at the wrong depth.
    //
    //   blue    the lookup left the cascade, so the map was never asked
    //   red     an occluder nearer the light than the fragment, brighter the
    //           deeper it sits in front — this is what being shadowed looks like
    //   green   nothing in front of the fragment: the map says it is lit
    //   black   the two agree to within a millimetre, which is the ambiguous case
    if (uFrame.shadowCounts.w == kShadowDebugMargin && shadowCascadeCount > 0u)
    {
        // The same lookup SampleCascade would make, so the number read here is
        // the number the comparison was given.
        vec3        Ng    = GeometricNormal();
        vec3        sunL  = -dirLights[uFrame.shadowCounts.y].directionIntensity.xyz;
        ShadowProbe probe = ProbeCascade(shadowCascade, vWorldPos, Ng, dot(Ng, sunL));

        const float kFullScaleMetres = 0.25;
        float margin = (probe.stored - probe.reference) * uFrame.shadowCascade[shadowCascade].w;
        float scaled = clamp(abs(margin) / kFullScaleMetres, 0.0, 1.0);

        color = probe.outside ? vec3(0.0, 0.0, 1.0)
                              : (margin >= 0.0 ? vec3(0.0, scaled, 0.0) : vec3(scaled, 0.0, 0.0));
    }

    // Tap view: the visibility that actually shades the pixel, against the two
    // narrower answers inside it. Every stage that can add light is on screen at
    // once, so a leak names the stage that produced it instead of only proving
    // the stage below it was innocent.
    //
    //   red     what SunVisibility returns — the number the shading uses
    //   green   what this cascade's own filtered lookup returns
    //   blue    what its centre tap alone returns
    //
    //   black   all three shadowed: correct, and the ordinary case in shadow
    //   white   all three lit: correct, and the ordinary case out of it
    //   red     the blend relit a fragment its own cascade shadowed — the next
    //           cascade's coarser texels read through what this one did not
    //   yellow  the kernel found light its centre did not: the footprint
    //           reaching past the silhouette of whatever shades this fragment
    if (uFrame.shadowCounts.w == kShadowDebugTaps && shadowCascadeCount > 0u)
    {
        vec3  Ng    = GeometricNormal();
        vec3  sunL  = -dirLights[uFrame.shadowCounts.y].directionIntensity.xyz;
        float NdotL = dot(Ng, sunL);

        // A surface turned away from the sun is never asked, so reporting an
        // answer for it invents one: the shading gate below decides the colour
        // before the lookup does, and the roof inside a box is dark because it
        // faces down, not because of anything in the map.
        if (NdotL <= 0.0)
        {
            outColor = vec4(0.08, 0.08, 0.12, 1.0);
            return;
        }

        ShadowProbe probe    = ProbeCascade(shadowCascade, vWorldPos, Ng, NdotL);
        float       blended  = SunVisibility(Ng, sunL, shadowCascade, NdotL);
        float       filtered = SampleCascade(shadowCascade, vWorldPos, Ng, NdotL);
        float       centre   = ShadowTap(ShadowCoord(shadowCascade,
                                                     vWorldPos + Ng * (uFrame.shadowCascade[shadowCascade].z *
                                                                       sqrt(max(0.0, 1.0 - NdotL * NdotL)))).xy,
                                         shadowCascade, probe.reference);

        color = vec3(blended, filtered, centre);
    }

    // Linear radiance, unbounded. The scene target is float and the tone map is
    // its own pass (tonemap.frag), so nothing here clamps or encodes: a value
    // over 1 is a real highlight until something downstream decides what it
    // looks like, and the MSAA resolve averages radiance rather than perception.
    outColor = vec4(color, 1.0);
}
