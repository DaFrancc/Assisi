#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inTangent;

layout(location = 0) out vec3  vWorldPos;
layout(location = 1) out vec3  vNormal;
layout(location = 2) out vec2  vTexCoord;
layout(location = 3) out float vViewZ; // view-space Z (negative for geometry in front of camera)

// Per-draw data. modelViewProjection and model are both needed: the former
// for gl_Position, the latter for world-space lighting — 128 bytes total,
// the portable Vulkan minimum, so no more per-draw data can be added here
// without exceeding some GPUs' guaranteed push-constant budget. Per-frame
// data (camera, cluster grid) lives in FrameConstants below instead.
layout(push_constant) uniform PushConstants
{
    mat4 modelViewProjection;
    mat4 model;
} pc;

// NVRHI's Vulkan backend offsets ConstantBuffer bindings by +256
// (VulkanBindingOffsets::constantBuffer) — see MeshPass.cpp's matching comment.
layout(binding = 256) uniform FrameConstants
{
    mat4  view;
    uvec4 gridDim;
    vec4  screenSizeNearFar;
    uvec4 lightCounts;
} uFrame;

void main()
{
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);
    vWorldPos = worldPos.xyz;
    // Assumes uniform scale — no material system needs non-uniform scale yet.
    vNormal   = normalize(mat3(pc.model) * inNormal);
    vTexCoord = inTexCoord;
    vViewZ    = (uFrame.view * worldPos).z;

    gl_Position = pc.modelViewProjection * vec4(inPosition, 1.0);
}
