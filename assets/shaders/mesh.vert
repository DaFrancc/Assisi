#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inTangent;

layout(location = 0) out vec3  vWorldPos;
layout(location = 1) out vec3  vNormal;
layout(location = 2) out vec2  vTexCoord;
layout(location = 3) out float vViewZ;       // view-space Z (negative for geometry in front of camera)
layout(location = 4) out vec3  vTangent;     // world-space tangent (for the normal-map TBN)
layout(location = 5) out float vTangentSign; // bitangent handedness (glTF TANGENT.w)
layout(location = 6) out flat uint vMaterialIndex; // row into the material table (per instance)

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
    vec4  ambient;
    uvec4 shadowCounts;
    vec4  shadowParams;
    vec4  shadowCascade[8];
    mat4  shadowViewProjection[8];
} uFrame;

void main()
{
    InstanceData inst = instances[gl_InstanceIndex];
    mat4 model = inst.model;
    vMaterialIndex = inst.materialIndex;

    vec4 worldPos = model * vec4(inPosition, 1.0);
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

    gl_Position = uFrame.viewProjection * worldPos;
}
