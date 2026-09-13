// The material's textures and constants, and the surface they describe at this
// fragment.

// Every material texture, bindless, in descriptor set 1.
layout(set = 1, binding = 0) uniform texture2D uTextures[];
layout(binding = 128) uniform sampler uMaterialSampler;

vec4 sampleMaterialTex(uint slot, vec2 uv)
{
    return texture(sampler2D(uTextures[nonuniformEXT(slot)], uMaterialSampler), uv);
}

// Mirrors Render::MaterialConstants, member for member.
struct MaterialRow
{
    vec4  baseColorFactor;
    vec4  emissiveFactorNormalScale; // xyz = emissive, w = normalScale
    vec4  metalRoughOcclusion;       // x = metallic, y = roughness, z = occlusion strength, w = specular AA variance clamp
    vec4  specularColorIor;          // rgb = specularColor, w = specularIor
    vec4  openPbrParams;             // x = baseWeight, y = specularWeight, z = baseDiffuseRoughness, w = alphaCutoff (0 unless masked)
    uvec4 flags;                     // x = material flag bits below
    uvec4 texIndices;                // bindless slots: x=baseColor y=normal z=metalRough w=occlusion
    uvec4 texIndicesEmissive;        // x = emissive bindless slot
};

// Must match Render::MaterialFlagBits.
const uint kMatFlagHasNormalTexture        = 1u;
const uint kMatFlagEnergyPreservingDiffuse = 2u;
const uint kMatFlagSpecularAntiAliasing    = 4u;

layout(std430, binding = 0) readonly buffer Materials
{
    MaterialRow materials[];
};

// Variance of the pixel footprint the specular AA kernel assumes, in pixels
// squared.
const float kSpecAaScreenSpaceVariance = 0.5;

struct Surface
{
    vec3  albedo;    // base colour x baseWeight
    float metallic;
    float roughness;
    float occlusion; // 1 = unoccluded
    vec3  emissive;
    vec3  normal;    // world-space, normal-mapped if present
    vec3  specColor; // OpenPBR specular_color
    float specWeight;
    float specIor;
    float diffuseRoughness; // OpenPBR base_diffuse_roughness
    bool  eonDiffuse;       // material uses the EON diffuse lobe
    float alpha;            // base colour alpha; read only by the ASSISI_ALPHA_MASK build
    float alphaCutoff;      // alpha below this kills the fragment; 0 unless masked
};

Surface SampleMaterial()
{
    Surface s;
    MaterialRow mat = materials[vMaterialIndex];

    // The base colour texture is sRGB, so the sampler returns linear values.
    vec4 base = sampleMaterialTex(mat.texIndices.x, vTexCoord) * mat.baseColorFactor;
    s.albedo = base.rgb * mat.openPbrParams.x;
    s.alpha = base.a;
    s.alphaCutoff = mat.openPbrParams.w;

    // glTF packing: G = roughness, B = metallic.
    vec2 mr = sampleMaterialTex(mat.texIndices.z, vTexCoord).gb;
    s.roughness = clamp(mat.metalRoughOcclusion.y * mr.x, 0.04, 1.0);
    s.metallic  = clamp(mat.metalRoughOcclusion.x * mr.y, 0.0, 1.0);

    float ao = sampleMaterialTex(mat.texIndices.w, vTexCoord).r;
    s.occlusion = 1.0 + mat.metalRoughOcclusion.z * (ao - 1.0);

    s.emissive = sampleMaterialTex(mat.texIndicesEmissive.x, vTexCoord).rgb *
                 mat.emissiveFactorNormalScale.xyz;

    s.specColor = mat.specularColorIor.rgb;
    s.specWeight = mat.openPbrParams.y;
    s.specIor = mat.specularColorIor.w;
    s.diffuseRoughness = clamp(mat.openPbrParams.z, 0.0, 1.0);
    s.eonDiffuse = (mat.flags.x & kMatFlagEnergyPreservingDiffuse) != 0u;

    // A back face (double-sided pipelines only) faces the viewer with -N.
    vec3 N = normalize(vNormal);
    if (!gl_FrontFacing)
    {
        N = -N;
    }
    if ((mat.flags.x & kMatFlagHasNormalTexture) != 0u)
    {
        // Gram-Schmidt the tangent against N, with an arbitrary perpendicular
        // where the tangent is parallel to N.
        vec3 Traw = vTangent - dot(vTangent, N) * N;
        vec3 T    = dot(Traw, Traw) > 1e-8 ? normalize(Traw)
                                           : normalize(abs(N.y) < 0.99 ? cross(N, vec3(0.0, 1.0, 0.0))
                                                                       : cross(N, vec3(1.0, 0.0, 0.0)));
        vec3 B = cross(N, T) * vTangentSign;
        vec3 sampledNormal = sampleMaterialTex(mat.texIndices.y, vTexCoord).xyz * 2.0 - 1.0;
        sampledNormal.xy *= mat.emissiveFactorNormalScale.w;
        s.normal = normalize(mat3(T, B, N) * sampledNormal);
    }
    else
    {
        s.normal = N;
    }

    // Geometric specular antialiasing: widen roughness by the normal's variance
    // across the pixel, capped by the material's clamp. Derivatives are safe in
    // this branch because the flag is uniform across a quad.
    if ((mat.flags.x & kMatFlagSpecularAntiAliasing) != 0u)
    {
        vec3  dNdx     = dFdx(s.normal);
        vec3  dNdy     = dFdy(s.normal);
        float variance = kSpecAaScreenSpaceVariance * (dot(dNdx, dNdx) + dot(dNdy, dNdy));
        float kernel   = min(2.0 * variance, mat.metalRoughOcclusion.w);
        float a        = s.roughness * s.roughness;
        s.roughness    = sqrt(sqrt(clamp(a * a + kernel, 0.0, 1.0)));
    }

    return s;
}
