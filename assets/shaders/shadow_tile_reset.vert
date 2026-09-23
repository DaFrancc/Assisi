#version 450

// One triangle covering the viewport at the far plane. Drawn with depth test
// Always and no fragment stage, it sets every texel of the viewport's rectangle
// to 1.0: a depth clear confined to one tile of a shared atlas, which a clear
// command cannot be. Draw with vertexCount = 3 and no bound buffers.

void main()
{
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2); // (0,0),(2,0),(0,2)
    gl_Position = vec4(pos * 2.0 - 1.0, 1.0, 1.0);
}
