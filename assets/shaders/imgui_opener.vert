#version 450

// This pipeline is never actually drawn with — see DebugUI's "opener pipeline"
// comment for why it exists (guarantees the dynamic-rendering target is bound
// via NVRHI's own tracked setGraphicsState path every frame, even when the
// scene has nothing to draw, so ImGui always has something to render into).

void main()
{
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
