// Shared by the sun's cascades and the local lights' atlas: the sampler, the
// filter kernels and the receiver-plane arithmetic.
//
// Every texel a filter reads on the light's side of the receiver is compared
// against the receiver's own plane at that texel's centre, not against one depth
// for the whole lookup. On a surface the light grazes, those texels are the
// surface itself, nearer the light, and one depth reads them as a blocker:
// stripes of shadow across a lit surface. So a tap gathers its four texels raw
// and compares and blends them here, where a comparison sampler would hold all
// four to one depth.

// Reads texels raw for textureGather. Outside the map it returns depth 1, the
// far plane, so a texel past the edge is never a blocker.
layout(binding = 129) uniform sampler uShadowSampler;

// Must match Render::ShadowDebugView.
const uint kShadowDebugCascades = 1u;
const uint kShadowDebugMargin   = 2u;
const uint kShadowDebugTaps     = 3u;

// Must match Render::ShadowFilter.
const uint kShadowFilterPoint = 0u;
const uint kShadowFilterPcf3  = 1u;
const uint kShadowFilterPcf5  = 2u;
const uint kShadowFilterVogel = 3u;

// Grid kernel half-widths in taps. Must match Render::kPcf3FilterRadiusTaps and
// kPcf5FilterRadiusTaps; the CPU sizes the atlas clamp inset from them.
const int kPcf3Radius = 1;
const int kPcf5Radius = 2;

float PcfTapCount(int radius)
{
    int side = 2 * radius + 1;
    return float(side * side);
}

// The Vogel disk. kVogelRadiusSteps must match Render::kVogelFilterRadiusTaps.
const uint  kVogelTaps          = 16u;
const float kVogelRadiusSteps   = 2.5;
const float kGoldenAngle        = 2.39996323;

// The disk split into a probe of four well-spread taps and the rest; the rest
// runs only where the probe disagrees.
const uint kVogelProbeTaps = 4u;
const uint kVogelProbe[kVogelProbeTaps] = uint[](0u, 4u, 11u, 15u);
const uint kVogelRest[kVogelTaps - kVogelProbeTaps] = uint[](1u, 2u, 3u, 5u, 6u, 7u, 8u, 9u, 10u, 12u, 13u, 14u);

const float kTwoPi = 6.28318531;

// A texel is a blocker only if it is at least this many texels of depth in front
// of the receiver's plane.
const float kPcssBlockerMinDepthTexels = 1.0;

// The steepest receiver slope a lookup follows, in texels of depth per texel
// (about 89.94 degrees). Must match Render::kMaxReceiverSlopeTexels.
const float kMaxReceiverSlope = 1000.0;

// Past this, a normal lies too near the X axis for their cross product to be a
// well-conditioned tangent, and the Y axis is used instead. Must match the one
// in ShadowView.cpp.
const float kTangentAxisLimit = 0.9;

// Per-pixel rotation for the Vogel disk: Jimenez's interleaved gradient noise,
// whose constants are the function.
float InterleavedGradientNoise(vec2 position)
{
    return fract(52.9829189 * fract(dot(position, vec2(0.06711056, 0.00583715))));
}

// Tap @p i of the Vogel disk of radius @p radius, rotated by @p phi.
vec2 VogelOffset(uint i, float phi, float radius)
{
    float r     = sqrt((float(i) + 0.5) / float(kVogelTaps)) * radius;
    float theta = float(i) * kGoldenAngle + phi;
    return vec2(r * cos(theta), r * sin(theta));
}

// The rasterized surface's normal, flipped for a back face as the shaded normal
// is. Shadow lookups offset along this rather than the normal-mapped one, and
// take the receiver's plane from it.
vec3 GeometricNormal()
{
    vec3 n = normalize(vNormal);
    return gl_FrontFacing ? n : -n;
}

// A unit vector along the surface @p N faces. Any one will do: the plane is
// fixed by it and its cross product with @p N.
vec3 ReceiverTangent(vec3 N)
{
    vec3 axis = abs(N.x) < kTangentAxisLimit ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    return normalize(cross(N, axis));
}

// dz/du and dz/dv of the receiver in a map, from the change in (u, v, depth)
// over two steps along it, @p along1 and @p along2. @p depthPerUv is one texel
// of depth over one texel of UV. Zero where the surface is edge-on to the map.
vec2 ReceiverPlaneSlope(vec3 along1, vec3 along2, float depthPerUv)
{
    float det = along1.x * along2.y - along1.y * along2.x;
    if (det == 0.0)
    {
        return vec2(0.0);
    }
    vec2  slope = vec2(along1.z * along2.y - along1.y * along2.z, along1.x * along2.z - along1.z * along2.x) / det;
    float limit = kMaxReceiverSlope * depthPerUv;
    float len   = length(slope);
    return len > limit ? slope * (limit / len) : slope;
}

// The four texels a bilinear tap at a point reads: where to gather them, and
// how far past the first one's centre the point lies, in texels.
struct TexelQuad
{
    vec2 gatherUv;
    vec2 fraction;
};

TexelQuad QuadAt(vec2 uv, vec2 texels)
{
    vec2 position = uv * texels - 0.5;
    vec2 first    = floor(position);
    // The corner the four share. Gathered anywhere else, a point a rounding
    // error from a texel centre could gather the next four along.
    return TexelQuad((first + 1.0) / texels, position - first);
}

// A lookup's receiver in one map: where its own point lands, its depth there,
// already biased, and how its plane deepens across the map.
struct Receiver
{
    vec2  uv;
    vec2  slope;
    float reference;
};

// The depth a texel centred @p offset from the receiver's own point is compared
// against: the plane where it runs toward the light, the receiver's own depth
// where it runs away. Must match Render::ShadowComparedDepth, which says why.
float ComparedDepth(Receiver receiver, vec2 offset)
{
    return receiver.reference + min(dot(offset, receiver.slope), 0.0);
}

// How lit a tap at @p tapUv is: the four texels gathered for @p quad, each lit
// where the depth it is compared against is in front of what it stores, blended
// bilinearly. @p texels is the map's size.
float CompareTexels(vec4 stored, TexelQuad quad, vec2 texels, vec2 tapUv, Receiver receiver)
{
    // From the receiver's point to the first texel's centre and the second's,
    // along each axis.
    vec2 tap   = tapUv - receiver.uv;
    vec2 back  = tap - quad.fraction / texels;
    vec2 ahead = tap + (1.0 - quad.fraction) / texels;
    // textureGather returns (i0, j1), (i1, j1), (i1, j0), (i0, j0).
    vec4 compared = vec4(ComparedDepth(receiver, vec2(back.x, ahead.y)), ComparedDepth(receiver, ahead),
                         ComparedDepth(receiver, vec2(ahead.x, back.y)), ComparedDepth(receiver, back));
    vec4 lit = vec4(lessThan(compared, stored));
    return mix(mix(lit.w, lit.z, quad.fraction.x), mix(lit.x, lit.y, quad.fraction.x), quad.fraction.y);
}

// ---- Contact hardening (percentage-closer soft shadows) ---------------------
//
// A fixed kernel gives every shadow the same softness. PCSS searches the map for
// blockers in front of the receiver, then sizes the kernel from their mean depth
// gap: a penumbra is (receiver - blocker) / blocker times the light's size, so
// the shadow is sharp where a caster touches its receiver and widens with height.
//
// The blocker search measures each texel against the receiver's plane too, so a
// grazing receiver never counts its own texels as blockers.

// How far a contact-hardened kernel reaches for a blocker @p gapDepth in front
// of its receiver. Must match Render::PcssPenumbraUv.
float PcssPenumbraUv(float penumbraUvPerDepth, float gapDepth, float maxReachUv)
{
    return clamp(penumbraUvPerDepth * gapDepth, 0.0, maxReachUv);
}
