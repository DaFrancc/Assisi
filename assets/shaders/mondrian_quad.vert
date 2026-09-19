#version 450
#extension GL_GOOGLE_include_directive : require

// Mondrian's quads, instanced, with no vertex buffer: the six corners of each
// quad's two triangles come from gl_VertexIndex, and the quad itself from the
// instance buffer at gl_InstanceIndex, which includes the draw's first instance.

#include "mondrian/quad.glsl"

layout(location = 0) out vec2 vPixel;          // window pixels
layout(location = 1) out vec2 vUv;
layout(location = 2) flat out uint vInstance;

// Unit-square corners for the quad's two triangles. The pipeline culls nothing,
// so their winding does not matter.
const vec2 kCorners[6] = vec2[](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
                                vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));

void main()
{
    const uint instance = uint(gl_InstanceIndex);
    const vec4 rect     = InstanceSlot(instance, kSlotRect);
    const vec4 uv       = InstanceSlot(instance, kSlotUv);
    const vec2 corner   = kCorners[gl_VertexIndex];

    vPixel    = rect.xy + corner * rect.zw;
    vUv       = uv.xy + corner * uv.zw;
    vInstance = instance;

    const vec2 ndc = vPixel / pass.viewport * 2.0 - 1.0;
    // NVRHI flips the Vulkan viewport to D3D's convention, so clip-space y
    // points up while pixel y points down.
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
}
