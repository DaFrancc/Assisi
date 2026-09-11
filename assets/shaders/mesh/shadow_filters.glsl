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
const float kGoldenAngle        = 2.39996323;

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

// Tap @p i of the Vogel disk of radius @p radius, rotated by @p phi.
vec2 VogelOffset(uint i, float phi, float radius)
{
    float r     = sqrt((float(i) + 0.5) / float(kVogelTaps)) * radius;
    float theta = float(i) * kGoldenAngle + phi;
    return vec2(r * cos(theta), r * sin(theta));
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
