#version 450
#extension GL_GOOGLE_include_directive : require

// Mondrian's quads: the shape, border and clip are signed distances in pixels,
// so every edge is anti-aliased on a 1-sample target. The output is
// premultiplied, to match the pipeline's One / InvSrcAlpha blend.

#include "mondrian/quad.glsl"

layout(location = 0) in vec2 vPixel;
layout(location = 1) in vec2 vUv;
layout(location = 2) flat in uint vInstance;

layout(set = 1, binding = 0) uniform texture2D uTexture;
layout(set = 1, binding = 128) uniform sampler uSampler;

layout(location = 0) out vec4 outColor;

// Width of the anti-aliasing ramp across an edge, in pixels. One pixel keeps a
// pixel-aligned edge fully crisp and a curved one smooth.
const float kAaWidth = 1.0;

// A distance at the pixel centre maps to coverage through the ramp's midpoint.
const float kCoverageMidpoint = 0.5;

// A glyph atlas stores a signed distance field: the outline at 128 of 255,
// inside above it.
const float kSdfOutline = 128.0 / 255.0;

// The smallest screen-space gradient the glyph edge is ramped over, so a texel
// with a flat field does not divide by zero.
const float kSdfMinGradient = 1.0 / 1024.0;

// 1 / sqrt(2): scales a distance measured along a diagonal to a true distance.
const float kInvSqrt2 = 0.70710678;

// The sRGB transfer function's constants, for decoding a display colour to
// the linear value an sRGB target encodes back to it.
const float kSrgbLinearThreshold = 0.04045;
const float kSrgbLinearScale     = 12.92;
const float kSrgbOffset          = 0.055;
const float kSrgbScale           = 1.055;
const float kSrgbGamma           = 2.4;

float Coverage(float distance)
{
    return clamp(kCoverageMidpoint - distance / kAaWidth, 0.0, 1.0);
}

// Signed distance from a point @p local, relative to the box centre, to a box
// of half size @p halfSize.
float BoxDistance(vec2 local, vec2 halfSize)
{
    const vec2 q = abs(local) - halfSize;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);
}

// Signed distance to the quad's shape: a box whose corner in @p local's
// quadrant is square, rounded or cut with radius @p radius.
float ShapeDistance(vec2 local, vec2 halfSize, float radius, uint style)
{
    if (style == kCornerRounded && radius > 0.0)
    {
        const vec2 q = abs(local) - halfSize + radius;
        return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
    }
    const float box = BoxDistance(local, halfSize);
    if (style == kCornerCut && radius > 0.0)
    {
        // The chamfer is the line cutting `radius` off each edge at the corner.
        const vec2 a      = abs(local);
        const float chamfer = (a.x + a.y - (halfSize.x + halfSize.y - radius)) * kInvSqrt2;
        return max(box, chamfer);
    }
    return box;
}

// The corner whose quadrant @p local lies in, in Assisi::Mondrian::Corner order.
uint QuadrantCorner(vec2 local)
{
    if (local.y < 0.0)
    {
        return local.x < 0.0 ? 0u : 1u;
    }
    return local.x < 0.0 ? 3u : 2u;
}

vec3 SrgbToLinear(vec3 display)
{
    const vec3 low  = display / kSrgbLinearScale;
    const vec3 high = pow((display + kSrgbOffset) / kSrgbScale, vec3(kSrgbGamma));
    return mix(high, low, lessThanEqual(display, vec3(kSrgbLinearThreshold)));
}

vec4 Premultiply(vec4 color)
{
    return vec4(color.rgb * color.a, color.a);
}

void main()
{
    const vec4 rect        = InstanceSlot(vInstance, kSlotRect);
    const vec4 clip        = InstanceSlot(vInstance, kSlotClip);
    const vec4 color       = InstanceSlot(vInstance, kSlotColor);
    const vec4 borderColor = InstanceSlot(vInstance, kSlotBorderColor);
    const vec4 radii       = InstanceSlot(vInstance, kSlotCornerRadius);
    const vec4 scalars     = InstanceSlot(vInstance, kSlotScalars);
    const float borderWidth = scalars.x;
    const uint cornerStyles = floatBitsToUint(scalars.y);
    const uint kind         = floatBitsToUint(scalars.z);

    const vec2 halfSize = rect.zw * 0.5;
    const vec2 local    = vPixel - (rect.xy + halfSize);
    const uint corner   = QuadrantCorner(local);
    const float radius  = min(radii[corner], min(halfSize.x, halfSize.y));
    const uint style    = (cornerStyles >> (corner * kCornerStyleBits)) & kCornerStyleMask;

    const float outer = ShapeDistance(local, halfSize, radius, style);

    // Sampled, and its gradient taken, outside any branch: derivatives are only
    // defined where every pixel of a 2x2 quad runs the same code.
    const vec4 texel       = texture(sampler2D(uTexture, uSampler), vUv);
    const float fieldWidth = max(fwidth(texel.r), kSdfMinGradient);

    vec4 fill = color;
    if (kind == kKindImage || kind == kKindNineSlice)
    {
        fill *= texel;
    }
    else if (kind == kKindGlyph)
    {
        // The edge ramps over one screen pixel of field whatever the glyph's
        // size, which is what keeps one atlas crisp small and large.
        fill.a *= clamp((texel.r - kSdfOutline) / fieldWidth + kCoverageMidpoint, 0.0, 1.0);
    }

    // The border is the band between the shape and the same shape moved in by
    // its width; the fill shows through the inner edge.
    vec4 premultiplied = Premultiply(fill);
    if (borderWidth > 0.0)
    {
        const float innerCoverage = Coverage(outer + borderWidth);
        premultiplied = mix(Premultiply(borderColor), premultiplied, innerCoverage);
    }

    const vec2 clipHalf  = clip.zw * 0.5;
    const float clipped  = Coverage(BoxDistance(vPixel - (clip.xy + clipHalf), clipHalf));
    vec4 result          = premultiplied * (Coverage(outer) * clipped);

    if (pass.encodeSrgb != 0u && result.a > 0.0)
    {
        result.rgb = SrgbToLinear(result.rgb / result.a) * result.a;
    }
    outColor = result;
}
