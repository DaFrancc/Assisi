// Shared by the sun's cascades and the local lights' atlas: the comparison
// sampler, the filter kernels and the receiver-plane arithmetic.

// Compares against four texels and returns the blend.
layout(binding = 129) uniform samplerShadow uShadowSampler;

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

// The disk at unit radius: tap i at radius sqrt((i + 0.5) / 16) and angle i
// times the golden angle (2.39996323 rad). Tabulated, because a per-tap sqrt,
// sin and cos in every filter of every light is transcendental work repeated
// per light for a pattern that never changes.
const vec2 kVogelDisk[kVogelTaps] = vec2[](
    vec2(0.17677670, 0.00000000),
    vec2(-0.22577219, 0.20682582),
    vec2(0.03455805, -0.39377118),
    vec2(0.28457122, 0.37117276),
    vec2(-0.52222319, -0.09237393),
    vec2(0.49469539, -0.31468471),
    vec2(-0.16546593, 0.61552500),
    vec2(-0.31556147, -0.60759440),
    vec2(0.68464216, 0.25003022),
    vec2(-0.71225609, 0.29400896),
    vec2(0.34335450, -0.73372862),
    vec2(0.25373024, 0.80893199),
    vec2(-0.76474589, -0.44318588),
    vec2(0.89713398, -0.19723239),
    vec2(-0.54750691, 0.77877223),
    vec2(-0.12648677, -0.97608970));

// The disk split into a probe of four well-spread taps and the rest; the rest
// runs only where the probe disagrees.
const uint kVogelProbe[4] = uint[](0u, 4u, 11u, 15u);
const uint kVogelRest[12] = uint[](1u, 2u, 3u, 5u, 6u, 7u, 8u, 9u, 10u, 12u, 13u, 14u);

const float kTwoPi = 6.28318531;

// A texel is a blocker only if it is at least this many texels of depth in front
// of the receiver's plane.
const float kPcssBlockerMinDepthTexels = 1.0;

// The steepest receiver slope accepted, in texels of depth per texel (about 84
// degrees).
const float kPcssMaxReceiverSlope = 10.0;

// The world position's change one pixel across and one down. Set at the top of
// main while control flow is uniform; zero while no contact-hardening path is on.
vec3 gWorldPosDx = vec3(0.0);
vec3 gWorldPosDy = vec3(0.0);

// Per-pixel rotation for the Vogel disk.
float InterleavedGradientNoise(vec2 position)
{
    return fract(52.9829189 * fract(dot(position, vec2(0.06711056, 0.00583715))));
}

// This pixel's rotation of the disk as (cos, sin). Set once at the top of main:
// every filter in the fragment rotates by the same angle.
vec2 gVogelRotation = vec2(1.0, 0.0);

vec2 VogelRotation()
{
    float phi = InterleavedGradientNoise(gl_FragCoord.xy) * kTwoPi;
    return vec2(cos(phi), sin(phi));
}

// Tap @p i of the Vogel disk of radius @p radius, rotated by this pixel's angle.
vec2 VogelOffset(uint i, float radius)
{
    vec2 d = kVogelDisk[i];
    return vec2(d.x * gVogelRotation.x - d.y * gVogelRotation.y, d.x * gVogelRotation.y + d.y * gVogelRotation.x) *
           radius;
}

// The rasterized surface's normal, flipped for a back face as the shaded normal
// is. Shadow lookups offset along this rather than the normal-mapped one.
vec3 GeometricNormal()
{
    vec3 n = normalize(vNormal);
    return gl_FrontFacing ? n : -n;
}

// ---- Contact hardening (percentage-closer soft shadows) ---------------------
//
// A fixed kernel gives every shadow the same softness. PCSS searches the map for
// blockers in front of the receiver, then sizes the kernel from their mean depth
// gap: a penumbra is (receiver - blocker) / blocker times the light's size, so
// the shadow is sharp where a caster touches its receiver and widens with height.
//
// Each tap compares against the receiver's own plane (receiver-plane depth bias)
// rather than a single depth. On a receiver tilted toward the light, a wide
// kernel covers ground that recedes in depth, and one reference depth would read
// the receiver's own texels as blockers.

// How far a contact-hardened kernel reaches for a blocker @p gapDepth in front
// of its receiver. Must match Render::PcssPenumbraUv.
float PcssPenumbraUv(float penumbraUvPerDepth, float gapDepth, float maxReachUv)
{
    return clamp(penumbraUvPerDepth * gapDepth, 0.0, maxReachUv);
}

// dz/du and dz/dv of the receiver in a map, from the change in (u, v, depth) one
// pixel across (@p dx) and down (@p dy). @p depthPerUv is one texel of depth
// over one texel of UV. Zero where the pixel's footprint is degenerate.
vec2 ReceiverPlaneSlope(vec3 dx, vec3 dy, float depthPerUv)
{
    float det = dx.x * dy.y - dx.y * dy.x;
    if (det == 0.0)
    {
        return vec2(0.0);
    }
    vec2  slope = vec2(dx.z * dy.y - dx.y * dy.z, dx.x * dy.z - dx.z * dy.x) / det;
    float limit = kPcssMaxReceiverSlope * depthPerUv;
    float len   = length(slope);
    return len > limit ? slope * (limit / len) : slope;
}
