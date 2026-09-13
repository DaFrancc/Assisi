// The OpenPBR base layer over a Cook-Torrance lobe, and local-light attenuation.

// GGX integrals by (n.v, roughness), from Render::BuildBrdfLut: rg = the split
// sum's scale and bias, b = the per-light lobe's directional albedo at F0 = 1.
layout(binding = 11)  uniform texture2D uBrdfTable;
layout(binding = 130) uniform sampler   uClampSampler;

// ---- Cook-Torrance ----------------------------------------------------------
//
// Algebraically the textbook NDF * G * F / (4 NdotV NdotL), rearranged so the
// per-light loop issues as few divides, square roots and pows as possible:
//
//   * Smith-GGX's numerators cancel the specular denominator. With
//     k = (roughness + 1)^2 / 8,
//         G / (4 NdotV NdotL) = 1 / (4 (NdotV (1-k) + k) (NdotL (1-k) + k))
//     because the NdotV and NdotL in each Schlick-GGX numerator divide out. Three
//     divides become one, and the NdotV half is the same for every light.
//   * Schlick's pow(x, 5) is x2 * x2 * x: the compiler lowers pow() to a log and
//     an exp rather than expanding a constant exponent.
//
// What does not change per light (k, a2, the view half of Smith, the diffuse
// base) is built once per fragment in BrdfContext.
//
// ---- OpenPBR base layer -----------------------------------------------------
//
// Three additions to plain Schlick and Lambert:
//
//   * F0 from the IOR (specular_ior, _weight, _color) rather than a fixed 0.04.
//     The defaults are exact: ior 1.5 gives ((1.5 - 1) / (1.5 + 1))^2 = 0.04.
//   * F82-tint conductor Fresnel (Kutz et al.): Schlick minus one lobe peaking
//     near 82 degrees, scaled so specular_color is the reflectance there relative
//     to Schlick. specular_color 1 zeroes it, leaving plain Schlick.
//   * Multi-scatter energy compensation. Single-scatter GGX drops the energy that
//     would bounce again between microfacets, which darkens rough metals; the lobe
//     is scaled by 1 + F0 (1/Ess - 1), where Ess is its directional albedo from
//     the BRDF table. Closed-form fits of Ess are too far off at full roughness.
//
// EON (energy-preserving Oren-Nayar, Portsmouth/Kutz/Hill 2024) replaces Lambert
// when a material sets base_diffuse_roughness; it is skipped at the default 0.

// Fujii's Oren-Nayar (FON) is Lambert plus a term that brightens a rough surface
// toward the light and viewer. For diffuse roughness r, its lobe is normalised by
// A = 1 / (1 + kFonC1 r), and its albedo averaged over the hemisphere is
// A (1 + kFonC2 r). EON adds a multi-scatter lobe sized from both, so a rough
// surface keeps the energy single-scatter FON loses.
const float kFonC1 = 0.5 - 2.0 / (3.0 * PI);
const float kFonC2 = 2.0 / 3.0 - 28.0 / (15.0 * PI);
const float kEps = 1.0e-7;

// FON's albedo seen from angle cosine @p mu at roughness @p r, as the EON
// paper's polynomial fit.
float FonAlbedo(float mu, float r)
{
    float m = 1.0 - mu;
    float GoverPi = m * (0.0571085289 + m * (0.491881867 + m * (-0.332181442 + m * 0.0714429953)));
    return (1.0 + r * GoverPi) / (1.0 + kFonC1 * r);
}

// Everything the per-light lobe needs that does not depend on the light.
struct BrdfContext
{
    vec3  diffuseBase; // albedo * (1 - metallic) / PI  (Lambert; unused when eon)
    vec3  F0;
    float a2;          // (roughness^2)^2
    float oneMinusK;   // 1 - k
    float k;           // Smith k = (roughness + 1)^2 / 8
    float visV;        // NdotV * (1-k) + k, the view half of Smith
    float NdotV;
    vec3  f82;         // F82-tint coefficient; zero for dielectrics and untinted metals
    vec3  energyComp;  // multi-scatter compensation for the per-light lobe
    vec2  envBrdf;     // the split sum's (A, B) at this n.v and roughness
    bool  eon;
    float eonR;
    vec3  eonSingle;   // rho / PI
    vec3  eonMulti;    // the view half of the multi-scatter lobe
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

    // F82 tint at the reference angle mu = 1/7.
    const float mu   = 1.0 / 7.0;
    const float om   = 1.0 - mu;
    const float om2  = om * om;
    const float om5  = om2 * om2 * om;
    const float om6  = om5 * om;
    vec3 schlickAtMu = F0 + (1.0 - F0) * om5;
    c.f82 = s.metallic * (1.0 - s.specColor) * schlickAtMu * (1.0 / (mu * om6));

    vec4 table   = textureLod(sampler2D(uBrdfTable, uClampSampler), vec2(NdotV, s.roughness), 0.0);
    c.envBrdf    = table.rg;
    c.energyComp = vec3(1.0) + F0 * (1.0 / max(table.b, kEps) - 1.0);

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

// The dielectric F0 OpenPBR's specular parameters give; vec3(0.04) at the
// defaults.
vec3 DielectricF0(float ior, float weight, vec3 tint)
{
    float r0 = (ior - 1.0) / (ior + 1.0);
    return min(weight * tint * (r0 * r0), vec3(1.0));
}

// One light's reflected radiance.
vec3 CookTorrance(BrdfContext c, vec3 N, vec3 V, vec3 L, vec3 radiance)
{
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL == 0.0)
        return vec3(0.0);

    vec3  H     = normalize(V + L);
    float NdotH = max(dot(N, H), 0.0);
    float d     = NdotH * NdotH * (c.a2 - 1.0) + 1.0;
    float visL  = NdotL * c.oneMinusK + c.k;

    // Roughness is at least 0.04, so both visibility terms are at least k > 0.
    float spec = (c.a2 * 0.25) / (PI * d * d * c.visV * visL);

    float VdotH = max(dot(H, V), 0.0);
    float fc  = clamp(1.0 - VdotH, 0.0, 1.0);
    float fc2 = fc * fc;
    float fc5 = fc2 * fc2 * fc;
    // Schlick minus the F82 lobe.
    vec3  F   = c.F0 + (1.0 - c.F0) * fc5 - c.f82 * (VdotH * fc5 * fc);

    vec3 diffuse = c.diffuseBase;
    if (c.eon)
    {
        float s       = dot(L, V) - NdotL * c.NdotV;
        float sovertF = s > 0.0 ? s / max(NdotL, c.NdotV) : s;
        diffuse = c.eonSingle * (1.0 + c.eonR * sovertF) +
                  c.eonMulti * max(kEps, 1.0 - FonAlbedo(NdotL, c.eonR));
    }

    return (diffuse * (1.0 - F) + F * spec * c.energyComp) * radiance * NdotL;
}

// Windowed inverse-square attenuation: zero at and past the radius. Takes the
// squared distance and squared radius.
float AttenuationSq(float d2, float radiusSq)
{
    float rr    = d2 / radiusSq;
    float numer = max(1.0 - rr * rr, 0.0);
    return (numer * numer) / (d2 + 1.0);
}
