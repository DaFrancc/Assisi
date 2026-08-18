#version 450

// Sun shadow map, depth-only. There is no fragment stage: the pipeline writes
// depth and nothing else, which is what makes a cascade cheap enough to render
// four of.

layout(location = 0) in vec3 inPosition;

// Per-instance world matrices, one record per drawn submesh instance, indexed by
// gl_InstanceIndex (each draw sets startInstanceLocation). All four cascades
// share one buffer — each cascade's commands index its own range — so a frame
// uploads once however many cascades it draws. Must match
// Render::ShadowPass::InstanceData (std430: a bare mat4, 64-byte stride).
struct ShadowInstance
{
    mat4 model;
};

layout(std430, binding = 0) readonly buffer ShadowInstances
{
    ShadowInstance instances[];
};

// The cascade being drawn. A push constant rather than a constant buffer
// because it changes between the four draws of a single frame, and this is the
// only thing that does.
layout(push_constant) uniform PushConstants
{
    mat4 lightViewProjection;
} pc;

void main()
{
    gl_Position = pc.lightViewProjection * (instances[gl_InstanceIndex].model * vec4(inPosition, 1.0));
}
