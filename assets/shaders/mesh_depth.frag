#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

// The depth prepass's alpha test, and nothing else. Paired with mesh.vert built
// with ASSISI_DEPTH_ONLY and ASSISI_ALPHA_MASK, it is the whole fragment stage a
// cutout surface gets in the prepass; opaque surfaces have no fragment stage
// there at all.
//
// This is the only alpha test a cutout gets. The lit pass after it draws
// cutouts with an Equal depth test and no discard (see
// Render::AfterPrepassDepthTest), so the texels this keeps are exactly the ones
// shaded, and a hole, nearer than whatever shows through it, is never shaded.
// Multisampled, the samples this covers are the ones holding the cutout's
// depth, so the lit pass colours exactly those.

#include "mesh/cutout.glsl"

layout(location = 2) in vec2 vTexCoord;
layout(location = 6) in flat uint vMaterialIndex;

// Nothing is written to the colour target; alpha-to-coverage reads the alpha.
layout(location = 0) out vec4 outCoverage;

layout(set = 1, binding = 0) uniform texture2D uTextures[];
layout(binding = 128) uniform sampler uMaterialSampler;

// Mirrors Render::MaterialConstants, exactly as mesh.frag does. Only three
// fields are read, but the rest have to be declared or the offsets move.
struct MaterialRow
{
    vec4  baseColorFactor;
    vec4  emissiveFactorNormalScale;
    vec4  metalRoughOcclusion;
    vec4  specularColorIor;
    vec4  openPbrParams;             // w = alphaCutoff (0 unless masked)
    uvec4 flags;
    uvec4 texIndices;                // x = baseColor bindless slot
    uvec4 texIndicesEmissive;
};

layout(std430, binding = 0) readonly buffer Materials
{
    MaterialRow materials[];
};

void main()
{
    MaterialRow mat = materials[vMaterialIndex];
    float alpha = (texture(sampler2D(uTextures[nonuniformEXT(mat.texIndices.x)], uMaterialSampler), vTexCoord) *
                   mat.baseColorFactor).a;
    float coverage = CutoutCoverage(alpha, mat.openPbrParams.w);
    if (coverage < 0.0)
    {
        discard;
    }
    outCoverage = vec4(0.0, 0.0, 0.0, coverage);
}
