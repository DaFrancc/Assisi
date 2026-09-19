#version 450

// One Mondrian quad, with no vertex buffer: the six corners of its two
// triangles come from gl_VertexIndex and are placed from the push constants.

layout(push_constant) uniform QuadConstants
{
    vec4 rect;     // x, y, width, height in pixels, origin top-left
    vec4 color;    // straight-alpha display colour
    vec2 viewport; // target width and height in pixels
} quad;

// Unit-square corners for the quad's two triangles. The pipeline culls nothing,
// so their winding does not matter.
const vec2 kCorners[6] = vec2[](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
                                vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));

void main()
{
    const vec2 pixel = quad.rect.xy + kCorners[gl_VertexIndex] * quad.rect.zw;
    const vec2 ndc   = pixel / quad.viewport * 2.0 - 1.0;
    // NVRHI flips the Vulkan viewport to D3D's convention, so clip-space y
    // points up while pixel y points down.
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
}
