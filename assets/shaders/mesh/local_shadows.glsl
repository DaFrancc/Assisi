// Spot and point light shadows. A spot's map and each of a point light's six
// cube faces is a perspective view into its own rectangle (tile) of one shared
// atlas.
//
// Every lookup clamps to its tile: the sampler clamps only to the whole atlas,
// and a tap past the tile's edge would read the neighbouring light's depth. The
// clamp rectangle is the tile inset by the kernel's reach; faces are drawn a few
// degrees wider than they need to be so the inset loses no coverage.
//
// A perspective texel covers more world the further it is from the light, so the
// biases arrive from the CPU per unit of distance and are scaled here by the
// receiver's distance along the view's axis.

layout(binding = 9) uniform texture2D uShadowAtlas;

// The nearest a receiver is taken to be to its light, in world units, so the
// per-distance biases never divide by zero.
const float kMinAxisDepth = 1e-4;

float LocalShadowTap(vec2 uv, vec4 clampUv, float reference)
{
    return texture(sampler2DShadow(uShadowAtlas, uShadowSampler),
                   vec3(clamp(uv, clampUv.xy, clampUv.zw), reference));
}

// A point light's faces, in the order its views are in the table. Must match
// Render::PointLightFace.
const uint kFacePositiveX = 0u;
const uint kFaceNegativeX = 1u;
const uint kFacePositiveY = 2u;
const uint kFaceNegativeY = 3u;
const uint kFacePositiveZ = 4u;
const uint kFaceNegativeZ = 5u;

// The face @p direction (from the light) falls in: its dominant axis. Must agree
// with Render::PointLightFaceOf, ties included.
uint PointLightFace(vec3 direction)
{
    vec3 m = abs(direction);
    if (m.x >= m.y && m.x >= m.z)
    {
        return direction.x >= 0.0 ? kFacePositiveX : kFaceNegativeX;
    }
    if (m.y >= m.z)
    {
        return direction.y >= 0.0 ? kFacePositiveY : kFaceNegativeY;
    }
    return direction.z >= 0.0 ? kFacePositiveZ : kFaceNegativeZ;
}

// Where @p worldPos lands in @p view's tile: atlas UV in xy, stored depth in z.
vec3 LocalShadowCoord(ShadowViewRow view, vec3 worldPos)
{
    vec4 clip   = view.viewProjection * vec4(worldPos, 1.0);
    vec3 ndc    = clip.xyz / clip.w;
    vec2 tileUv = ndc.xy * vec2(0.5, -0.5) + 0.5;
    return vec3(tileUv * view.uvScaleOffset.xy + view.uvScaleOffset.zw, ndc.z);
}

// CascadeBlockerGap's search, inside one tile. @p texelDepth is one texel of
// depth at the receiver's distance.
float LocalBlockerGap(ShadowViewRow view, vec2 uv, float reference, vec2 slope, float texelDepth)
{
    float searchUv  = PcssPenumbraUv(view.pcss.x, reference, view.pcss.z);
    float threshold = kPcssBlockerMinDepthTexels * texelDepth;
    ivec2 size      = textureSize(uShadowAtlas, 0);
    float phi       = InterleavedGradientNoise(gl_FragCoord.xy) * kTwoPi;

    float gapSum   = 0.0;
    float blockers = 0.0;
    for (uint i = 0u; i < kVogelTaps; ++i)
    {
        vec2  tapUv  = clamp(uv + VogelOffset(i, phi, searchUv), view.clampUv.xy, view.clampUv.zw);
        ivec2 texel  = min(ivec2(tapUv * vec2(size)), size - 1);
        vec2  centre = (vec2(texel) + 0.5) / vec2(size);
        float gap    = reference + dot(centre - uv, slope) - texelFetch(uShadowAtlas, texel, 0).r;
        if (gap > threshold)
        {
            gapSum += gap;
            blockers += 1.0;
        }
    }
    return blockers > 0.0 ? gapSum / blockers : 0.0;
}

// One kernel tap, compared against the receiver's plane where the tap landed
// after clamping.
float LocalPcssTap(ShadowViewRow view, vec2 uv, vec2 offset, float reference, vec2 slope)
{
    vec2 tapUv = clamp(uv + offset, view.clampUv.xy, view.clampUv.zw);
    return LocalShadowTap(tapUv, view.clampUv, reference + dot(tapUv - uv, slope));
}

// The Vogel kernel at @p stepUv inside one tile, with the probe early-out.
float FilterLocalPcss(ShadowViewRow view, vec2 uv, float reference, vec2 slope, float stepUv)
{
    float phi = InterleavedGradientNoise(gl_FragCoord.xy) * kTwoPi;
    float sum = 0.0;
    for (uint i = 0u; i < 4u; ++i)
    {
        sum += LocalPcssTap(view, uv, VogelOffset(kVogelProbe[i], phi, kVogelRadiusSteps) * stepUv, reference, slope);
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
        sum += LocalPcssTap(view, uv, VogelOffset(kVogelRest[i], phi, kVogelRadiusSteps) * stepUv, reference, slope);
    }
    return sum / float(kVogelTaps);
}

// A local light through @p view, contact-hardened. @p biasedPos, @p uv and
// @p reference are LocalVisibility's already-biased lookup.
float LocalVisibilityPcss(ShadowViewRow view, vec3 biasedPos, vec2 uv, float reference, float axisDepth)
{
    // A texel of perspective depth shrinks with distance, so it is measured at
    // the receiver.
    float texelDepth = view.pcss.y / max(axisDepth, kMinAxisDepth);
    vec3  here       = LocalShadowCoord(view, biasedPos);
    vec2  slope      = ReceiverPlaneSlope(LocalShadowCoord(view, biasedPos + gWorldPosDx) - here,
                                          LocalShadowCoord(view, biasedPos + gWorldPosDy) - here,
                                          texelDepth / view.params.z);
    float gap        = LocalBlockerGap(view, uv, reference, slope, texelDepth);
    if (gap == 0.0)
    {
        return 1.0;
    }
    float stepUv = PcssPenumbraUv(view.pcss.x, gap, view.pcss.z) / kVogelRadiusSteps;
    return FilterLocalPcss(view, uv, reference, slope, stepUv);
}

// Fraction of a local light reaching @p worldPos through view @p viewIndex:
// 1 lit, 0 shadowed. @p N is the geometric normal; @p axisDepth is the distance
// along the view's own axis, which is what a texel's footprint scales with.
float LocalVisibility(uint viewIndex, vec3 worldPos, vec3 N, float NdotL, float axisDepth)
{
    ShadowViewRow view = shadowViews[viewIndex];

    float sinTheta  = sqrt(max(1.0 - NdotL * NdotL, 0.0));
    vec3  biasedPos = worldPos + N * (view.params.y * axisDepth * sinTheta);

    // Behind the light or outside the frustum this view recorded: nothing is
    // known, so lit.
    vec4 clip = view.viewProjection * vec4(biasedPos, 1.0);
    if (clip.w <= 0.0)
    {
        return 1.0;
    }
    vec3 ndc = clip.xyz / clip.w;
    if (any(greaterThan(abs(ndc.xy), vec2(1.0))) || ndc.z > 1.0 || ndc.z < 0.0)
    {
        return 1.0;
    }

    // Into the tile, then into the atlas. The map is drawn through a flipped
    // viewport, so its first row is ndc.y = +1.
    vec2 tileUv = ndc.xy * vec2(0.5, -0.5) + 0.5;
    vec2 uv     = tileUv * view.uvScaleOffset.xy + view.uvScaleOffset.zw;

    float reference = ndc.z - view.params.x / max(axisDepth, kMinAxisDepth);
    float stepUv    = view.params.z;
    vec4  clampUv   = view.clampUv;

    if (uFrame.localShadowCounts.z == 1u)
    {
        return LocalVisibilityPcss(view, biasedPos, uv, reference, axisDepth);
    }

    uint filterMode = uFrame.localShadowCounts.x;
    if (filterMode == kShadowFilterPoint)
    {
        return LocalShadowTap(uv, clampUv, reference);
    }

    if (filterMode == kShadowFilterVogel)
    {
        float phi = InterleavedGradientNoise(gl_FragCoord.xy) * kTwoPi;
        float sum = 0.0;
        for (uint i = 0u; i < kVogelTaps; i++)
        {
            sum += LocalShadowTap(uv + VogelOffset(i, phi, kVogelRadiusSteps) * stepUv, clampUv, reference);
        }
        return sum / float(kVogelTaps);
    }

    int   radius = filterMode == kShadowFilterPcf5 ? kPcf5Radius : kPcf3Radius;
    float sum    = 0.0;
    for (int y = -radius; y <= radius; ++y)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            sum += LocalShadowTap(uv + vec2(float(x), float(y)) * stepUv, clampUv, reference);
        }
    }
    return sum / PcfTapCount(radius);
}

// A spot light's visibility; its view looks down the cone's axis.
float SpotVisibility(uint shadowView, vec3 spotDir, vec3 lightPos, vec3 worldPos, vec3 N, float NdotL)
{
    if (shadowView == kNoShadowView)
    {
        return 1.0;
    }
    float axisDepth = max(dot(worldPos - lightPos, spotDir), 0.0);
    return LocalVisibility(shadowView, worldPos, N, NdotL, axisDepth);
}

// A point light's visibility, through the face @p worldPos falls in. That face's
// axis is the dominant one, so the largest component of |d| is the depth along it.
float PointVisibility(uint firstView, vec3 lightPos, vec3 worldPos, vec3 N, float NdotL)
{
    if (firstView == kNoShadowView)
    {
        return 1.0;
    }
    vec3  d         = worldPos - lightPos;
    float axisDepth = max(max(abs(d.x), abs(d.y)), abs(d.z));
    return LocalVisibility(firstView + PointLightFace(d), worldPos, N, NdotL, axisDepth);
}
