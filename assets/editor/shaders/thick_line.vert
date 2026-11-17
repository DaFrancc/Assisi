#version 450

// Overlay lines wider than a pixel, vertex stage (see Editor::LinePass). Each
// segment is two triangles; every corner carries both ends of its segment, and
// is pushed sideways across the segment as it lands on screen, so the line is
// the same number of pixels wide near and far. The pixel stage is line.frag.

layout(location = 0) in vec4  inColor;
layout(location = 1) in vec3  inPosition;  // this corner's end
layout(location = 2) in vec3  inOther;     // the segment's other end
layout(location = 3) in float inSide;      // -1 or +1: which side it is pushed to
layout(location = 4) in float inHalfWidth; // how far, in pixels

layout(push_constant) uniform PushConstants
{
    mat4 viewProjection;
    vec4 viewport; // xy = framebuffer size in pixels
} pc;

layout(location = 0) out vec4 vColor;

// Clip-space w at or below this is the eye plane and behind it, where the
// perspective divide stops meaning anything.
const float kMinClipW = 1e-4;

void main()
{
    vec4 here  = pc.viewProjection * vec4(inPosition, 1.0);
    vec4 there = pc.viewProjection * vec4(inOther, 1.0);
    vColor     = inColor;

    // Both ends behind the eye: nothing of the segment is on screen. A w below
    // zero puts the corner outside every clip plane, so the triangle is dropped.
    if (here.w < kMinClipW && there.w < kMinClipW)
    {
        gl_Position = vec4(0.0, 0.0, 0.0, -1.0);
        return;
    }

    // An end behind the eye is pulled along the segment to just in front of it.
    // Dividing by its own w would mirror it to the far side of the screen and
    // turn the segment's direction around.
    if (here.w < kMinClipW)
    {
        here = mix(here, there, (kMinClipW - here.w) / (there.w - here.w));
    }
    if (there.w < kMinClipW)
    {
        there = mix(there, here, (kMinClipW - there.w) / (here.w - there.w));
    }

    // The segment's direction in pixels, so the push is square to it on screen
    // whatever the viewport's shape.
    vec2  along  = (there.xy / there.w - here.xy / here.w) * pc.viewport.xy;
    float pixels = length(along);
    vec2  across = pixels > 0.0 ? vec2(-along.y, along.x) / pixels : vec2(0.0);

    // Pixels to clip space: the viewport spans two units of NDC, and the offset
    // is scaled by w so the divide brings it back to the same pixels.
    here.xy += across * (inSide * inHalfWidth) * 2.0 / pc.viewport.xy * here.w;
    gl_Position = here;
}
