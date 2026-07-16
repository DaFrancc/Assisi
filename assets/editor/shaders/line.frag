#version 450

// Generic 3D line renderer, pixel stage (see Render::LinePass). Emits the
// interpolated per-vertex colour directly, matching how the selection outline
// writes its colour straight into the scene target (see outline_edge.frag).

layout(location = 0) in  vec4 vColor;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vColor;
}
