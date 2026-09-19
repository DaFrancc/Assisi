#version 450

// See mondrian_quad.vert. The colour leaves premultiplied, to match the
// pipeline's One / InvSrcAlpha blend.

layout(push_constant) uniform QuadConstants
{
    vec4 rect;
    vec4 color;
    vec2 viewport;
} quad;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vec4(quad.color.rgb * quad.color.a, quad.color.a);
}
