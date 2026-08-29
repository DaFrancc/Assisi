#version 450
#extension GL_EXT_nonuniform_qualifier : require

// The alpha test, and nothing else. Paired with the ASSISI_ALPHA_MASK build of
// shadow_depth.vert, this is the whole fragment stage a cutout caster gets: no
// colour output, no lighting, no normal — a surviving fragment writes the depth
// the rasterizer already produced, and a killed one leaves the light through.
//
// Opaque casters never reach this shader. Their pipeline has no fragment stage
// at all, which is what keeps four cascades affordable.

layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in flat uint vMaterialIndex;

// Set 1 is the same bindless material-texture table the mesh pass samples, so
// the alpha tested here is the alpha the surface shows. A hole that moved
// between the two would read as a shading bug rather than a shadow one.
layout(set = 1, binding = 0) uniform texture2D uTextures[];
layout(binding = 128) uniform sampler uMaterialSampler;

// Mirrors Render::MaterialConstants, exactly as mesh.frag does — the same
// member list in the same order, every member a vec4/uvec4 lane. Only three
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

// t1: the instance records the vertex stage reads are t0.
layout(std430, binding = 1) readonly buffer Materials
{
    MaterialRow materials[];
};

void main()
{
    MaterialRow mat = materials[vMaterialIndex];
    // Explicit LOD 0, never an implicit mip. The derivatives here come from the
    // light's rasterization, not the camera's, and on a face near edge-on to the
    // light they explode — selecting a mip whose alpha is the texture's average.
    // A texture with a large hole averages below its own cutoff, so the whole
    // grazing face would discard and write no depth, opening a gap along exactly
    // the edges the shadow most needs.
    float alpha = textureLod(sampler2D(uTextures[nonuniformEXT(mat.texIndices.x)], uMaterialSampler),
                             vTexCoord, 0.0).a *
                  mat.baseColorFactor.a;
    if (alpha < mat.openPbrParams.w)
    {
        discard;
    }
}
