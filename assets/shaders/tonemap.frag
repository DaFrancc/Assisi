#version 450

// Tone map: linear HDR radiance -> display-encoded colour. The one place in the
// chain that leaves linear space, so everything upstream of it (the MSAA resolve,
// and any effect added later) operates on radiance, and everything downstream
// (FXAA, editor chrome) operates on perceptual values.
//
// The look is four stages: exposure, a tone curve, a grade, and the display
// encode. Every stage is a runtime knob — see Tonemap.hpp, which owns the
// operator's wire values and the ranges these constants are allowed to take.
//
// The swapchain is a UNORM format by deliberate choice (VulkanContext picks it
// over the _SRGB one), so this shader owns the encode. AgX therefore ends at its
// outset matrix with no linearising pow: its sigmoid output is already display
// encoded. Reinhard and ACES end in linear display-referred values and take an
// explicit gamma.

layout(location = 0) in  vec2 vTexCoords;
layout(location = 0) out vec4 outColor;

// Texture_SRV/Sampler are separate descriptors in NVRHI's Vulkan backend, offset
// by VulkanBindingOffsets: shaderResource at +0, sampler at +128 (see fxaa.frag).
layout(binding = 0)   uniform texture2D uScene;
layout(binding = 128) uniform sampler   uSceneSampler;

layout(push_constant) uniform PushConstants
{
    float exposureScale; // linear multiplier on radiance: exp2(stops)
    float contrast;      // exponent on the curve's output
    float saturation;    // scale on each channel's distance from luma
    uint  op;            // TonemapOperator
    // Non-zero copies the input through untouched. Two callers need that: the
    // material debug views, whose scene target holds channel values rather than
    // radiance, and the final copy of an already-mapped image to the swapchain.
    uint  passthrough;
} pc;

const uint kAgx      = 0u;
const uint kAces     = 1u;
const uint kReinhard = 2u;

const vec3 kLumaWeights = vec3(0.2126, 0.7152, 0.0722);

// --- AgX -------------------------------------------------------------------
//
// Rows of the inset matrix sum to 1, so it leaves a neutral grey alone while
// pulling every saturated colour toward the middle. That is what makes a bright
// colour bleach toward white as it climbs instead of rotating toward the nearest
// corner of the RGB cube, which is the failure a per-channel curve cannot avoid.
// The outset matrix is its inverse and gives back the saturation the inset took,
// after the sigmoid and the grade have both had their say.

const float kAgxMinEv = -12.47393;
const float kAgxMaxEv = 4.026069;

const mat3 kAgxInset = mat3(
    0.842479062253094,  0.0423282422610123, 0.0423756549057051,
    0.0784335999999992, 0.878468636469772,  0.0784336,
    0.0792237451477643, 0.0791661274605434, 0.879142973793104);

const mat3 kAgxOutset = mat3(
     1.19687900512017,   -0.0528968517574562, -0.0529716355144438,
    -0.0980208811401368,  1.15190312990417,   -0.0980434501171241,
    -0.0990297440797205, -0.0989611768448433,  1.15107367264116);

// Sixth-order fit of the AgX sigmoid over the normalised EV range. Cheaper than
// the piecewise original and within 4e-6 mean squared error of it.
vec3 AgxSigmoid(vec3 x)
{
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return 15.5 * x4 * x2
         - 40.14 * x4 * x
         + 31.96 * x4
         - 6.868 * x2 * x
         + 0.4298 * x2
         + 0.1191 * x
         - 0.00232;
}

// --- The other two ---------------------------------------------------------

vec3 AcesFit(vec3 x)
{
    // Narkowicz's fit of the ACES RRT/ODT. Reaches white at ~7.2x linear.
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 Reinhard(vec3 x)
{
    return x / (x + vec3(1.0));
}

vec3 EncodeGamma(vec3 x)
{
    return pow(max(x, vec3(0.0)), vec3(1.0 / 2.2));
}

// The grade, an ASC CDL power plus a saturation about the pixel's luma. AgX is
// neutral by design and reads flat without this; the defaults in Tonemap.hpp are
// the punchy look rather than the identity for that reason.
vec3 Grade(vec3 v)
{
    v = pow(max(v, vec3(0.0)), vec3(pc.contrast));
    float luma = dot(v, kLumaWeights);
    return max(luma + pc.saturation * (v - luma), vec3(0.0));
}

void main()
{
    const vec4 scene = texture(sampler2D(uScene, uSceneSampler), vTexCoords);
    if (pc.passthrough != 0u)
    {
        outColor = scene;
        return;
    }

    vec3 color = scene.rgb * pc.exposureScale;

    if (pc.op == kAgx)
    {
        color = kAgxInset * color;
        // log2(0) is -inf and the clamp is what absorbs it; without it the
        // sigmoid is evaluated on a NaN and black pixels come out arbitrary.
        color = clamp(log2(color), vec3(kAgxMinEv), vec3(kAgxMaxEv));
        color = (color - kAgxMinEv) / (kAgxMaxEv - kAgxMinEv);
        color = AgxSigmoid(color);
        // The grade sits inside AgX, before the outset: the look gets the
        // desaturated image and the outset restores saturation afterwards, which
        // is the order AgX is built around.
        color = Grade(color);
        color = kAgxOutset * color;
    }
    else
    {
        color = (pc.op == kAces) ? AcesFit(color) : Reinhard(color);
        color = Grade(EncodeGamma(color));
    }

    outColor = vec4(clamp(color, vec3(0.0), vec3(1.0)), scene.a);
}
