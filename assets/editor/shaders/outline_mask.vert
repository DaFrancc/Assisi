#version 450

// Selection-outline mask pass, vertex stage. Just places the selected mesh's
// vertices; the fragment stage writes a flat coverage value. Position only — the
// mask is pure silhouette coverage, so normals/UVs/tangents aren't needed.

layout(location = 0) in vec3 inPosition;

layout(push_constant) uniform PushConstants
{
    mat4 modelViewProjection;
} pc;

void main()
{
    gl_Position = pc.modelViewProjection * vec4(inPosition, 1.0);
}
