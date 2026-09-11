#version 450

// Three builds of one stage. The plain one is the lit pass's whenever there is
// no depth prepass, and is what every frame drew with before there was one.
//
// ASSISI_DEPTH_ONLY builds the prepass: position out and nothing else, plus the
// texture coordinate and material row mesh_depth.frag alpha-tests against when
// ASSISI_ALPHA_MASK is also set. ASSISI_INVARIANT_POSITION builds the lit pass
// that draws after it.
//
// Both of those declare gl_Position invariant. The lit pass after a prepass
// tests its depth against the prepass's with LessOrEqual and writes none of its
// own, so a fragment survives only if the two stages computed its position to
// the bit. Two separately compiled stages are free to schedule the same
// arithmetic differently unless both say invariant; one that is off by an ulp
// leaves a crack of whatever the depth clear shows through. The plain build
// does not declare it, so the frame without a prepass is the frame it always was.
#if defined(ASSISI_DEPTH_ONLY) || defined(ASSISI_INVARIANT_POSITION)
invariant gl_Position;
#endif

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inTangent;

#ifndef ASSISI_DEPTH_ONLY
layout(location = 0) out vec3  vWorldPos;
layout(location = 1) out vec3  vNormal;
layout(location = 2) out vec2  vTexCoord;
layout(location = 3) out float vViewZ;       // view-space Z (negative for geometry in front of camera)
layout(location = 4) out vec3  vTangent;     // world-space tangent (for the normal-map TBN)
layout(location = 5) out float vTangentSign; // bitangent handedness (glTF TANGENT.w)
layout(location = 6) out flat uint vMaterialIndex; // row into the material table (per instance)
#elif defined(ASSISI_ALPHA_MASK)
// The same locations the lit build writes them at, so mesh_depth.frag reads them
// where mesh.frag does.
layout(location = 2) out vec2  vTexCoord;
layout(location = 6) out flat uint vMaterialIndex;
#endif

// Per-instance data (stage D): the world matrix and material id that used to be
// pushed as per-draw constants now live in a structured buffer, one record per
// drawn submesh. gl_InstanceIndex selects this draw's record (each draw sets
// startInstanceLocation); the model matrix never leaves the GPU. Must match
// Render::InstanceData (std430: mat4 then uint, 80-byte array stride).
struct InstanceData
{
    mat4 model;
    uint materialIndex;
};

// StructuredBuffer_SRV shares the shaderResource (+0) register space with
// Texture_SRV; this is t6 in MeshPass's binding layout (past the light buffers).
layout(std430, binding = 6) readonly buffer Instances
{
    InstanceData instances[];
};

// NVRHI's Vulkan backend offsets ConstantBuffer bindings by +256
// (VulkanBindingOffsets::constantBuffer) — see MeshPass.cpp's matching comment.
// viewProjection leads so clip position derives from each instance's world matrix.
layout(binding = 256) uniform FrameConstants
{
    mat4  viewProjection;
    mat4  view;
    uvec4 gridDim;
    vec4  screenSizeNearFar;
    uvec4 lightCounts;
    // Unused here, but the block must mirror Render::FrameConstants (and
    // mesh.frag's copy) member for member — a declaration that diverges in
    // the middle silently shifts every following offset.
    vec4  cameraPosition;
    vec4  clusterScale;
    vec4  indirectSky;
    vec4  indirectGround;
    vec4  indirectSpecular;
    uvec4 shadowCounts;
    uvec4 localShadowCounts;
    vec4  shadowParams;
    vec4  shadowPcss;
    vec4  shadowCascade[8];
    mat4  shadowViewProjection[8];
} uFrame;

void main()
{
    InstanceData inst = instances[gl_InstanceIndex];
    mat4 model = inst.model;
    vec4 worldPos = model * vec4(inPosition, 1.0);

#ifndef ASSISI_DEPTH_ONLY
    vMaterialIndex = inst.materialIndex;
    vWorldPos = worldPos.xyz;
    // Normals transform by the inverse-transpose of the model's upper-left 3x3,
    // so non-uniform scale doesn't skew them (for uniform scale/rotation this
    // reduces to mat3(model)). Computed per-vertex from the instance's model
    // matrix; inverse() of a 3x3 is cheap.
    mat3 normalMatrix = transpose(inverse(mat3(model)));
    vNormal   = normalize(normalMatrix * inNormal);
    // Tangents are directions along the surface, so they transform by the plain
    // model 3x3 (not the inverse-transpose) — the fragment shader re-orthonormalizes
    // against the normal, absorbing any residual skew. w carries handedness.
    vTangent     = mat3(model) * inTangent.xyz;
    vTangentSign = inTangent.w;
    vTexCoord = inTexCoord;
    vViewZ    = (uFrame.view * worldPos).z;
#elif defined(ASSISI_ALPHA_MASK)
    vMaterialIndex = inst.materialIndex;
    vTexCoord = inTexCoord;
#endif

    gl_Position = uFrame.viewProjection * worldPos;
}
