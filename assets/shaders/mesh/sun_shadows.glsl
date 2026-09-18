// The sun's cascaded shadow: one array slice per cascade, each an orthographic
// map fitted to a slice of the camera's view distance.
//
// Three biases keep a surface from shadowing itself:
//   * The rasterizer pushes each caster back by its own depth slope as the map
//     is drawn (slope-scaled bias).
//   * The lookup moves the receiver along its geometric normal by the cascade's
//     texel size times sin(incidence), which clears a texel of surface at any
//     angle and stays bounded as the light goes edge-on, where tan would not.
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

float ShadowTap(vec2 uv, uint cascade, float reference)
{
    return texture(sampler2DArrayShadow(uShadowCascades, uShadowSampler), vec4(uv, float(cascade), reference));
}

float VogelTap(uint i, vec2 uv, uint cascade, float reference, float stepUv, float phi)
{
    return ShadowTap(uv + VogelOffset(i, phi, kVogelRadiusSteps) * stepUv, cascade, reference);
}

// The depth of the nearest occluder the map recorded in front of @p reference at
// @p uv's own texel, or @p reference where there is none. Only the fragment's own
// texel: its neighbours on a grazing receiver are the receiver itself.
float NearestBlockerDepth(vec2 uv, uint cascade, float reference)
{
    ivec3 size = textureSize(uShadowCascades, 0);
    ivec2 texel = clamp(ivec2(clamp(uv, vec2(0.0), vec2(1.0)) * vec2(size.xy)), ivec2(0), size.xy - 1);
    return min(reference, texelFetch(uShadowCascades, ivec3(texel, int(cascade)), 0).r);
}

// How (u, v, depth) in @p cascade's map change over a world-space step.
vec3 ShadowCoordDelta(uint cascade, vec3 dWorld)
{
    vec3 d = mat3(uFrame.shadowViewProjection[cascade]) * dWorld;
    return vec3(d.xy * vec2(0.5, -0.5), d.z);
}

// Mean depth gap to the blockers around @p uv, measured against the receiver's
// plane at each texel; zero where nothing is in front.
float CascadeBlockerGap(uint cascade, vec2 uv, float reference, vec2 slope)
{
    float searchUv  = PcssPenumbraUv(uFrame.shadowPcss.x, reference, uFrame.shadowPcss.y);
    float threshold = kPcssBlockerMinDepthTexels * uFrame.shadowPcss.z;
    vec2  size      = vec2(textureSize(uShadowCascades, 0).xy);
    float phi       = InterleavedGradientNoise(gl_FragCoord.xy) * kTwoPi;

    float gapSum   = 0.0;
    float blockers = 0.0;
    for (uint i = 0u; i < kVogelTaps; ++i)
    {
        vec2 tapUv = uv + VogelOffset(i, phi, searchUv);
        if (any(lessThan(tapUv, vec2(0.0))) || any(greaterThanEqual(tapUv, vec2(1.0))))
        {
            continue;
        }
        ivec2 texel  = ivec2(tapUv * size);
        vec2  centre = (vec2(texel) + 0.5) / size;
        float stored = texelFetch(uShadowCascades, ivec3(texel, int(cascade)), 0).r;
        float gap    = reference + dot(centre - uv, slope) - stored;
        if (gap > threshold)
        {
            gapSum += gap;
            blockers += 1.0;
        }
    }
    return blockers > 0.0 ? gapSum / blockers : 0.0;
}

// The Vogel kernel at @p stepUv, each tap compared against the receiver's plane.
float FilterCascadePcss(uint cascade, vec2 uv, float reference, vec2 slope, float stepUv)
{
    float phi = InterleavedGradientNoise(gl_FragCoord.xy) * kTwoPi;
    float sum = 0.0;
    for (uint i = 0u; i < 4u; ++i)
    {
        vec2 offset = VogelOffset(kVogelProbe[i], phi, kVogelRadiusSteps) * stepUv;
        sum += ShadowTap(uv + offset, cascade, reference + dot(offset, slope));
    }
    if (sum == 0.0)
    {
        return 0.0;
    }
    if (sum == 4.0)
    {
        return 1.0;
    }
    for (uint i = 0u; i < 12u; ++i)
    {
        vec2 offset = VogelOffset(kVogelRest[i], phi, kVogelRadiusSteps) * stepUv;
        sum += ShadowTap(uv + offset, cascade, reference + dot(offset, slope));
    }
    return sum / float(kVogelTaps);
}

// The sun through @p cascade, contact-hardened. @p uv and @p reference are
// already biased.
float SampleCascadePcss(uint cascade, vec2 uv, float reference, float NdotL)
{
    // A texel of depth is a texel of UV in an orthographic cascade.
    vec2  slope = ReceiverPlaneSlope(ShadowCoordDelta(cascade, gWorldPosDx), ShadowCoordDelta(cascade, gWorldPosDy),
                                     1.0);
    float gap   = CascadeBlockerGap(cascade, uv, reference, slope);
    if (gap == 0.0)
    {
        return 1.0;
    }
    // The world-space cap on the penumbra, stretched by 1 / NdotL on the surface.
    float worldCapUv = uFrame.shadowPcss.w * max(NdotL, kEps) /
                       (kVogelRadiusSteps * uFrame.shadowCascade[cascade].w);
    float stepUv     = min(PcssPenumbraUv(uFrame.shadowPcss.x, gap, uFrame.shadowPcss.y) / kVogelRadiusSteps,
                           worldCapUv);
    return FilterCascadePcss(cascade, uv, reference, slope, stepUv);
}

// Fraction of the sun reaching @p worldPos through @p cascade: 1 lit, 0 shadowed.
// @p N is the geometric normal.
float SampleCascade(uint cascade, vec3 worldPos, vec3 N, float NdotL)
{
    float sinTheta  = sqrt(max(0.0, 1.0 - NdotL * NdotL));
    vec3  biasedPos = worldPos + N * (uFrame.shadowCascade[cascade].z * sinTheta);

    vec3 coord = ShadowCoord(cascade, biasedPos);
    vec2 uv    = coord.xy;

    // Outside the cascade: nothing recorded, so lit.
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || coord.z > 1.0)
    {
        return 1.0;
    }

    float reference = coord.z - uFrame.shadowCascade[cascade].y;

    if (uFrame.shadowPcss.x > 0.0)
    {
        return SampleCascadePcss(cascade, uv, reference, NdotL);
    }

    uint filterMode = uFrame.shadowCounts.z;
    if (filterMode == kShadowFilterPoint)
    {
        return ShadowTap(uv, cascade, reference);
    }

    // The sun subtends about half a degree, so a blocker d away throws a penumbra
    // about d / 216 wide. The kernel narrows to that where the map records a
    // blocker in front of this fragment, which keeps it from reaching past a
    // nearby occluder's edge; with nothing in front it stays at the cap.
    float texelStep  = uFrame.shadowParams.x;
    float capStep    = min(texelStep, uFrame.shadowParams.z * max(NdotL, kEps) / uFrame.shadowCascade[cascade].w);
    float blockerNdc = reference - NearestBlockerDepth(uv, cascade, reference);
    float step       = blockerNdc > 0.0 ? min(capStep, uFrame.shadowParams.w * blockerNdc) : capStep;

    if (filterMode == kShadowFilterVogel)
    {
        float phi = InterleavedGradientNoise(gl_FragCoord.xy) * kTwoPi;

        // A tap returns exactly 0 or 1 unless it straddles an edge, so four
        // probe taps that agree almost always mean the whole kernel would too.
        float sum = 0.0;
        for (uint i = 0u; i < 4u; ++i)
        {
            sum += VogelTap(kVogelProbe[i], uv, cascade, reference, step, phi);
        }
        if (sum == 0.0)
        {
            return 0.0;
        }
        if (sum == 4.0)
        {
            return 1.0;
        }
        for (uint i = 0u; i < 12u; ++i)
        {
            sum += VogelTap(kVogelRest[i], uv, cascade, reference, step, phi);
        }
        return sum / float(kVogelTaps);
    }

    int   radius = filterMode == kShadowFilterPcf5 ? kPcf5Radius : kPcf3Radius;
    float sum    = 0.0;
    for (int y = -radius; y <= radius; ++y)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            sum += ShadowTap(uv + vec2(float(x), float(y)) * step, cascade, reference);
        }
    }
    return sum / PcfTapCount(radius);
}

// What the map holds at this fragment's own lookup and what the fragment
// compares against it, for the debug views. Both in [0, 1] depth.
struct ShadowProbe
{
    float stored;
    float reference;
    bool  outside;
};

ShadowProbe ProbeCascade(uint cascade, vec3 worldPos, vec3 N, float NdotL)
{
    ShadowProbe probe;
    float sinTheta  = sqrt(max(0.0, 1.0 - NdotL * NdotL));
    vec3  biasedPos = worldPos + N * (uFrame.shadowCascade[cascade].z * sinTheta);

    vec3 coord     = ShadowCoord(cascade, biasedPos);
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
float SunVisibility(vec3 N, vec3 L, uint cascade, float NdotL)
{
    uint  cascadeCount = uFrame.shadowCounts.x;
    float viewDepth    = abs(vViewZ);

    // Past the last cascade the blend band has already faded to lit.
    if (viewDepth >= uFrame.shadowCascade[cascadeCount - 1u].x)
    {
        return 1.0;
    }

    float visibility = SampleCascade(cascade, vWorldPos, N, NdotL);

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
                float next = SampleCascade(cascade + 1u, vWorldPos, N, NdotL);
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

// Flat tints for the cascade debug view, one per slice.
vec3 CascadeTint(uint cascade)
{
    if (cascade == 0u) return vec3(1.0, 0.45, 0.45);
    if (cascade == 1u) return vec3(0.45, 1.0, 0.45);
    if (cascade == 2u) return vec3(0.45, 0.6, 1.0);
    return vec3(1.0, 0.95, 0.45);
}
