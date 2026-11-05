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

// How far inside its own rectangle of the texture an image samples, in texels:
// half of one keeps a sample at the edge from blending in the texel beyond,
// which in a sheet belongs to another sprite.
const float kHalfTexel = 0.5;

// The shortest middle a nine-slice divides by, in pixels. Corners that meet
// leave no middle, and then no pixel samples it.
const float kMinSliceMiddle = 1.0e-4;

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

// The two insets packed in @p word, the low half first.
vec2 UnpackInsets(uint word)
{
    return vec2(float(word & kSliceInsetMask), float(word >> kSliceInsetBits));
}

// One axis of a nine-slice: how many texels into the sprite the pixel @p offset
// in from the quad's start samples. The quad is @p length long with corners
// @p quadInsets in from its two ends; the sprite is @p texels long with corners
// @p spriteInsets in from its ends. A corner shows its texels at its own size
// and the middle stretches.
float SliceTexel(float offset, float length, vec2 quadInsets, vec2 spriteInsets, float texels)
{
    if (offset < quadInsets.x)
    {
        return offset * (spriteInsets.x / quadInsets.x);
    }
    if (offset > length - quadInsets.y)
    {
        return texels - ((length - offset) * (spriteInsets.y / quadInsets.y));
    }
    // From the centre of the middle's first texel to the centre of its last:
    // a sample between a corner's texel and the middle's would carry the
    // corner's colour into the middle and stretch it there.
    const float middle = max(length - quadInsets.x - quadInsets.y, kMinSliceMiddle);
    const float first  = spriteInsets.x + kHalfTexel;
    const float last   = max(texels - spriteInsets.y - kHalfTexel, first);
    return first + ((offset - quadInsets.x) / middle * (last - first));
}

// How many texels one pixel moves across on one axis of a nine-slice, for the
// whole quad: the corners' rate where the axis has corners. One rate for every
// part picks one mip level for every part, where each part's own would switch
// levels at a corner's edge and draw it as a seam.
float SliceRate(float length, vec2 quadInsets, vec2 spriteInsets, float texels)
{
    const float corners = quadInsets.x + quadInsets.y;
    if (corners > 0.0)
    {
        return (spriteInsets.x + spriteInsets.y) / corners;
    }
    return texels / max(length, kMinSliceMiddle);
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

    // Where the texture is sampled, and how fast that moves per pixel each way:
    // as the vertex shader interpolated it, except on a nine-slice, which
    // works out its own piece by piece. The derivatives are taken here, before
    // any branch, since they are only defined where every pixel of a 2x2 quad
    // runs the same code.
    const vec4 uvRect = InstanceSlot(vInstance, kSlotUv);
    const vec2 texels = vec2(textureSize(sampler2D(uTexture, uSampler), 0));
    vec2 uv           = vUv;
    vec2 uvAcross     = dFdx(vUv);
    vec2 uvDown       = dFdy(vUv);
    if (kind == kKindNineSlice)
    {
        const uvec4 slice   = floatBitsToUint(InstanceSlot(vInstance, kSlotSlice));
        const vec2 quadFrom = UnpackInsets(slice.x);   // left, top
        const vec2 quadTo   = UnpackInsets(slice.y);   // right, bottom
        const vec2 cutFrom  = UnpackInsets(slice.z);
        const vec2 cutTo    = UnpackInsets(slice.w);
        const vec2 acrossQuad = vec2(quadFrom.x, quadTo.x);
        const vec2 acrossCut  = vec2(cutFrom.x, cutTo.x);
        const vec2 downQuad   = vec2(quadFrom.y, quadTo.y);
        const vec2 downCut    = vec2(cutFrom.y, cutTo.y);
        const vec2 sprite     = uvRect.zw * texels;
        const vec2 offset     = vPixel - rect.xy;
        const vec2 into       = vec2(SliceTexel(offset.x, rect.z, acrossQuad, acrossCut, sprite.x),
                                     SliceTexel(offset.y, rect.w, downQuad, downCut, sprite.y));
        uv       = uvRect.xy + (into / texels);
        uvAcross = vec2(SliceRate(rect.z, acrossQuad, acrossCut, sprite.x) / texels.x, 0.0);
        uvDown   = vec2(0.0, SliceRate(rect.w, downQuad, downCut, sprite.y) / texels.y);
    }
    if (kind == kKindImage || kind == kKindNineSlice)
    {
        const vec2 inset = kHalfTexel / texels;
        uv = min(max(uv, uvRect.xy + inset), uvRect.xy + uvRect.zw - inset);
    }

    // Sampled, and the field's gradient taken, outside any branch, for the
    // same reason.
    const vec4 texel       = textureGrad(sampler2D(uTexture, uSampler), uv, uvAcross, uvDown);
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
