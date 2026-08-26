#version 450

// Shadow map depth. The default build has no fragment stage at all: the
// pipeline writes depth and nothing else, which is what makes a cascade cheap
// enough to render four of.
//
// ASSISI_ALPHA_MASK builds the alpha-tested variant, which carries the texture
// coordinate and the material row down to shadow_depth.frag so a cutout casts
// its hole. It is a separate build rather than a branch because the attribute
// fetch and the varyings cost every caster otherwise, and geometry with no
// alpha in it must not pay for geometry that has some.

layout(location = 0) in vec3 inPosition;
#ifdef ASSISI_ALPHA_MASK
layout(location = 1) in vec2 inTexCoord;

layout(location = 0) out vec2 vTexCoord;
layout(location = 1) out flat uint vMaterialIndex;
#endif

// Per-instance records, one per drawn submesh instance, indexed by
// gl_InstanceIndex (each draw sets startInstanceLocation). All the frame's
// views share one buffer — each view's commands index its own range — so a
// frame uploads once however many views it draws. Must match
// Render::ShadowInstanceData (std430: mat4 + uint, 80-byte stride). The opaque
// build declares materialIndex and never reads it: one record shape for both
// pipelines is what lets them share the buffer.
struct ShadowInstance
{
    mat4 model;
    uint materialIndex;
};

layout(std430, binding = 0) readonly buffer ShadowInstances
{
    ShadowInstance instances[];
};

// The view being drawn. A push constant rather than a constant buffer because
// it changes between the draws of a single frame, and this is the only thing
// that does.
layout(push_constant) uniform PushConstants
{
    mat4 lightViewProjection;
} pc;

void main()
{
    ShadowInstance instance = instances[gl_InstanceIndex];
#ifdef ASSISI_ALPHA_MASK
    vTexCoord = inTexCoord;
    vMaterialIndex = instance.materialIndex;
#endif
    gl_Position = pc.lightViewProjection * (instance.model * vec4(inPosition, 1.0));
}
