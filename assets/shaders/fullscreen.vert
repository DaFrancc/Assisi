#version 450

// Fullscreen triangle via gl_VertexIndex — no vertex buffer needed. Draw with
// vertexCount = 3 and no bound vertex/index buffers.

layout(location = 0) out vec2 vTexCoords;

void main()
{
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2); // (0,0),(2,0),(0,2)

    // NVRHI's negative-height viewport flips Y (screen top = NDC y=+1 — see
    // cluster_build.comp's screenToViewNear for the same flip), so texcoord.y
    // must be derived as (1 - pos.y), not pos.y directly, or the resolved/
    // FXAA'd image comes out upside down relative to the offscreen texture it
    // samples (which was itself rendered through the same flipped viewport).
    vTexCoords  = vec2(pos.x, 1.0 - pos.y);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
