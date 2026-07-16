#version 450

// Generic 3D line renderer, vertex stage (see Render::LinePass). Transforms a
// world-space line vertex by the camera's view-projection and forwards its colour
// unchanged. Used by the editor to draw collider wireframes, but carries no
// collider knowledge — it just places coloured line segments in the world.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;

layout(push_constant) uniform PushConstants
{
    mat4 viewProjection;
} pc;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = pc.viewProjection * vec4(inPosition, 1.0);
    vColor      = inColor;
}
