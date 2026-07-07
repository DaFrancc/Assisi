#version 450

// See imgui_opener.vert — this pipeline is never actually drawn with.

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vec4(0.0);
}
