// The sun's cascaded shadow: one array slice per cascade, each an orthographic
// map fitted to a slice of the camera's view distance.
//
// Each texel the filter reads is compared against the receiver's own plane at
// that texel (see shadow_filters.glsl), which is what keeps a surface the sun
// grazes from shadowing itself across the kernel. Three biases cover what the
// plane cannot:
//   * The rasterizer pushes each caster back by its own depth slope as the map
//     is drawn (slope-scaled bias), against the caster's depth being rounded
//     where it is rasterized.
//   * The lookup moves the receiver along its geometric normal by the cascade's
//     texel size times sin(incidence), against curved surfaces, whose plane is
//     only right at the point it was taken.
//   * A small constant depth bias covers the depth format's quantisation.
// Every bias also pulls the shadow off its caster by its own size, so each is
// kept to about a texel.

layout(binding = 7) uniform texture2DArray uShadowCascades;

// Where @p worldPos lands in @p cascade's map: UV in xy, comparison depth in z.
// The cascade projection is orthographic, so this is affine.
vec3 ShadowCoord(uint cascade, vec3 worldPos)
{
    vec4 clip = uFrame.shadowViewProjection[cascade] * vec4(worldPos, 1.0);
    vec3 ndc  = clip.xyz / clip.w;
    // The map is drawn through a flipped viewport, so its first row is ndc.y = +1.
    return vec3(ndc.xy * vec2(0.5, -0.5) + 0.5, ndc.z);
}

// How (u, v, depth) in @p cascade's map change over a world-space step.
vec3 ShadowCoordDelta(uint cascade, vec3 dWorld)
{
    vec3 d = mat3(uFrame.shadowViewProjection[cascade]) * dWorld;
    return vec3(d.xy * vec2(0.5, -0.5), d.z);
}

// dz/du and dz/dv of the plane facing @p N in @p cascade's map. The projection
// is affine, so the plane's gradient is the same everywhere in the map. Must
// match Render::ReceiverPlaneSlope.
vec2 CascadeReceiverSlope(uint cascade, vec3 N)
{
    vec3 first = ReceiverTangent(N);
    // A texel of depth is a texel of UV in an orthographic cascade.
    return ReceiverPlaneSlope(ShadowCoordDelta(cascade, first), ShadowCoordDelta(cascade, cross(N, first)), 1.0);
}

// How lit a tap at @p tapUv is for @p receiver.
float CascadeTap(uint cascade, vec2 tapUv, Receiver receiver)
{
    vec2      texels = vec2(textureSize(uShadowCascades, 0).xy);
    TexelQuad quad   = QuadAt(tapUv, texels);
    vec4      stored =
        textureGather(sampler2DArray(uShadowCascades, uShadowSampler), vec3(quad.gatherUv, float(cascade)), 0);
    return CompareTexels(stored, quad, texels, tapUv, receiver);
}

// How far in front of the receiver the map records an occluder at its own
// texel; zero where there is none. Only the fragment's own texel, which is what
// the kernel is narrowed for.
float OwnTexelBlockerGap(uint cascade, Receiver receiver)
{
    ivec3 size   = textureSize(uShadowCascades, 0);
    ivec2 texel  = clamp(ivec2(clamp(receiver.uv, vec2(0.0), vec2(1.0)) * vec2(size.xy)), ivec2(0), size.xy - 1);
    vec2  centre = (vec2(texel) + 0.5) / vec2(size.xy);
    float stored = texelFetch(uShadowCascades, ivec3(texel, int(cascade)), 0).r;
    return max(ComparedDepth(receiver, centre - receiver.uv) - stored, 0.0);
}

// Mean depth gap to the blockers around @p receiver, each texel measured
// against the depth ComparedDepth gives it; zero where nothing is in front.
float CascadeBlockerGap(uint cascade, Receiver receiver)
{
    float searchUv  = PcssPenumbraUv(uFrame.shadowPcss.x, receiver.reference, uFrame.shadowPcss.y);
    float threshold = kPcssBlockerMinDepthTexels * uFrame.shadowPcss.z;
    vec2  size      = vec2(textureSize(uShadowCascades, 0).xy);
    vec2  turn      = VogelTurn();

    float gapSum   = 0.0;
    float blockers = 0.0;
    for (uint i = 0u; i < kVogelTaps; ++i)
    {
        vec2 tapUv = receiver.uv + VogelOffset(i, turn, searchUv);
        if (any(lessThan(tapUv, vec2(0.0))) || any(greaterThanEqual(tapUv, vec2(1.0))))
        {
            continue;
        }
        ivec2 texel  = ivec2(tapUv * size);
        vec2  centre = (vec2(texel) + 0.5) / size;
        float stored = texelFetch(uShadowCascades, ivec3(texel, int(cascade)), 0).r;
        float gap    = ComparedDepth(receiver, centre - receiver.uv) - stored;
        if (gap > threshold)
        {
            gapSum += gap;
            blockers += 1.0;
        }
    }
    return blockers > 0.0 ? gapSum / blockers : 0.0;
}

// The Vogel disk at @p stepUv, with the probe early-out: most receivers are
// wholly lit or wholly shadowed, and there four well-spread taps agree and the
// other twelve would only agree with them. The rest of the disk runs where the
// probe straddles an edge.
float FilterCascadeVogel(uint cascade, Receiver receiver, float stepUv)
{
    vec2  turn = VogelTurn();
    float sum  = 0.0;
    for (uint i = 0u; i < kVogelProbeTaps; ++i)
    {
        sum += CascadeTap(cascade, receiver.uv + VogelOffset(kVogelProbe[i], turn, kVogelRadiusSteps * stepUv),
                          receiver);
    }
    if (ProbeAgrees(sum, kVogelProbeTaps))
    {
        return sum / float(kVogelProbeTaps);
    }
    for (uint i = 0u; i < kVogelTaps - kVogelProbeTaps; ++i)
    {
        sum += CascadeTap(cascade, receiver.uv + VogelOffset(kVogelRest[i], turn, kVogelRadiusSteps * stepUv),
                          receiver);
    }
    return sum / float(kVogelTaps);
}

// The 3x3 or 5x5 grid of @p radius at @p step, probed first at its centre and
// corners.
float FilterCascadeGrid(uint cascade, Receiver receiver, float step, int radius)
{
    float sum = CascadeTap(cascade, receiver.uv, receiver);
    for (uint i = 0u; i < kGridProbeTaps - 1u; ++i)
    {
        sum += CascadeTap(cascade, receiver.uv + kGridCorners[i] * (float(radius) * step), receiver);
    }
    if (ProbeAgrees(sum, kGridProbeTaps))
    {
        return sum / float(kGridProbeTaps);
    }
    for (int y = -radius; y <= radius; ++y)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            if (!IsGridProbe(x, y, radius))
            {
                sum += CascadeTap(cascade, receiver.uv + vec2(float(x), float(y)) * step, receiver);
            }
        }
    }
    return sum / PcfTapCount(radius);
}

// The sun through @p cascade, contact-hardened.
float SampleCascadePcss(uint cascade, Receiver receiver, float NdotL)
{
    float gap = CascadeBlockerGap(cascade, receiver);
    if (gap == 0.0)
    {
        return 1.0;
    }
    // The world-space cap on the penumbra, stretched by 1 / NdotL on the surface.
    float worldCapUv = uFrame.shadowPcss.w * max(NdotL, kEps) /
                       (kVogelRadiusSteps * uFrame.shadowCascade[cascade].w);
    float stepUv     = min(PcssPenumbraUv(uFrame.shadowPcss.x, gap, uFrame.shadowPcss.y) / kVogelRadiusSteps,
                           worldCapUv);
    return FilterCascadeVogel(cascade, receiver, stepUv);
}

// Fraction of the sun reaching @p surface through @p cascade: 1 lit, 0 shadowed.
float SampleCascade(uint cascade, ShadowSurface surface)
{
    float NdotL     = surface.NdotL;
    float sinTheta  = sqrt(max(0.0, 1.0 - NdotL * NdotL));
    vec3  biasedPos = surface.position + surface.normal * (uFrame.shadowCascade[cascade].z * sinTheta);

    vec3 coord = ShadowCoord(cascade, biasedPos);
    vec2 uv    = coord.xy;

    // Outside the cascade: nothing recorded, so lit.
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || coord.z > 1.0)
    {
        return 1.0;
    }

    Receiver receiver =
        Receiver(uv, CascadeReceiverSlope(cascade, surface.plane), coord.z - uFrame.shadowCascade[cascade].y);

    if (uFrame.shadowPcss.x > 0.0)
    {
        return SampleCascadePcss(cascade, receiver, NdotL);
    }

    uint filterMode = uFrame.shadowCounts.z;
    if (filterMode == kShadowFilterPoint)
    {
        return CascadeTap(cascade, uv, receiver);
    }

    // The sun subtends about half a degree, so a blocker d away throws a penumbra
    // about d / 216 wide. The kernel narrows to that where the map records a
    // blocker in front of this fragment, which keeps it from reaching past a
    // nearby occluder's edge; with nothing in front it stays at the cap.
    float texelStep  = uFrame.shadowParams.x;
    float capStep    = min(texelStep, uFrame.shadowParams.z * max(NdotL, kEps) / uFrame.shadowCascade[cascade].w);
    float blockerNdc = OwnTexelBlockerGap(cascade, receiver);
    float step       = blockerNdc > 0.0 ? min(capStep, uFrame.shadowParams.w * blockerNdc) : capStep;

    if (filterMode == kShadowFilterVogel)
    {
        return FilterCascadeVogel(cascade, receiver, step);
    }
    return FilterCascadeGrid(cascade, receiver, step, filterMode == kShadowFilterPcf5 ? kPcf5Radius : kPcf3Radius);
}

// What the map holds at this fragment's own lookup and what the fragment
// compares against it, for the debug views. Both in [0, 1] depth.
struct ShadowProbe
{
    vec2  uv;
    float stored;
    float reference;
    bool  outside;
};

ShadowProbe ProbeCascade(uint cascade, ShadowSurface surface)
{
    ShadowProbe probe;
    float sinTheta  = sqrt(max(0.0, 1.0 - surface.NdotL * surface.NdotL));
    vec3  biasedPos = surface.position + surface.normal * (uFrame.shadowCascade[cascade].z * sinTheta);

    vec3 coord     = ShadowCoord(cascade, biasedPos);
    probe.uv       = coord.xy;
    probe.outside  = any(lessThan(coord.xy, vec2(0.0))) || any(greaterThan(coord.xy, vec2(1.0))) || coord.z > 1.0;
    probe.reference = coord.z - uFrame.shadowCascade[cascade].y;

    ivec3 size = textureSize(uShadowCascades, 0);
    ivec2 texel = ivec2(clamp(coord.xy, vec2(0.0), vec2(1.0)) * vec2(size.xy));
    texel = clamp(texel, ivec2(0), size.xy - 1);
    probe.stored = texelFetch(uShadowCascades, ivec3(texel, int(cascade)), 0).r;
    return probe;
}

// The cascade covering @p viewDepth: the number of splits at or below it,
// counted without a break so the loop stays uniform.
uint SelectCascade(float viewDepth)
{
    uint cascadeCount = uFrame.shadowCounts.x;
    uint cascade      = 0u;
    for (uint i = 0u; i + 1u < cascadeCount; ++i)
    {
        cascade += viewDepth >= uFrame.shadowCascade[i].x ? 1u : 0u;
    }
    return cascade;
}

// The sun's visibility, blended over the end of each cascade into the next.
float SunVisibility(uint cascade, ShadowSurface surface)
{
    uint  cascadeCount = uFrame.shadowCounts.x;
    float viewDepth    = abs(vViewZ);

    // Past the last cascade the blend band has already faded to lit.
    if (viewDepth >= uFrame.shadowCascade[cascadeCount - 1u].x)
    {
        return 1.0;
    }

    float visibility = SampleCascade(cascade, surface);

    float splitFar  = uFrame.shadowCascade[cascade].x;
    float splitNear = cascade == 0u ? 0.0 : uFrame.shadowCascade[cascade - 1u].x;
    float band      = (splitFar - splitNear) * uFrame.shadowParams.y;
    if (band > 0.0 && viewDepth > splitFar - band)
    {
        float t = clamp((viewDepth - (splitFar - band)) / band, 0.0, 1.0);
        if (cascade + 1u < cascadeCount)
        {
            // Blend toward the darker of the two: the coarser cascade can only
            // miss occluders, never add them. Nothing to blend below zero.
            if (visibility > 0.0)
            {
                float next = SampleCascade(cascade + 1u, surface);
                visibility = mix(visibility, min(visibility, next), t);
            }
        }
        else
        {
            visibility = mix(visibility, 1.0, t);
        }
    }

    return visibility;
}

// Flat tints for the cascade debug view, one per slice, as many as the frame
// constants hold cascades: red, green, blue, yellow, magenta, cyan, orange, grey.
const vec3 kCascadeTints[8] = vec3[8](vec3(1.0, 0.45, 0.45), vec3(0.45, 1.0, 0.45), vec3(0.45, 0.6, 1.0),
                                      vec3(1.0, 0.95, 0.45), vec3(1.0, 0.45, 1.0), vec3(0.45, 1.0, 1.0),
                                      vec3(1.0, 0.7, 0.3), vec3(0.75, 0.75, 0.75));

vec3 CascadeTint(uint cascade)
{
    return kCascadeTints[min(cascade, uint(kCascadeTints.length()) - 1u)];
}
