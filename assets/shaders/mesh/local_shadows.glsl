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

// Two layers at the same rectangles: the still casters' depth in layer 0, and
// the moving casters' in layer 1 for a tile whose view row marks it live
// (ShadowViewRow.params.w = 1). A texel is lit only if it is lit in both, so the
// still depth is never copied under the movers.
layout(binding = 9) uniform texture2DArray uShadowAtlas;

// The nearest a receiver is taken to be to its light, in world units, so the
// per-distance biases never divide by zero.
const float kMinAxisDepth = 1e-4;

// How far along the receiver its plane is measured, as a fraction of its
// distance from the light. A projection maps a plane to a plane, so the answer
// is the same at any distance; a fraction of the receiver's own keeps both
// points in front of the light and their depths far enough apart for a float
// to tell. Must match the one in ShadowView.cpp.
const float kReceiverSlopeProbe = 1.0 / 16.0;

// Layer 0 is the still depth; a tile whose moving layer is live also reads
// layer 1.
const float kStillLayer  = 0.0;
const float kMovingLayer = 1.0;

// Whether @p view's tile has movers to sample beside its still depth.
bool HasMovingLayer(ShadowViewRow view)
{
    return view.params.w > 0.5;
}

// The nearer of the two layers' stored depths at @p texel.
float LocalStoredDepth(ivec2 texel, bool moving)
{
    float stored = texelFetch(uShadowAtlas, ivec3(texel, 0), 0).r;
    return moving ? min(stored, texelFetch(uShadowAtlas, ivec3(texel, 1), 0).r) : stored;
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

// dz/du and dz/dv in the atlas of the plane through @p biasedPos facing @p N,
// from two steps along it. Must match Render::ReceiverPlaneSlope.
vec2 LocalReceiverSlope(ShadowViewRow view, vec3 biasedPos, vec3 N, float axisDepth)
{
    float distance = max(axisDepth, kMinAxisDepth);
    float probe    = distance * kReceiverSlopeProbe;
    vec3  first    = ReceiverTangent(N);
    vec3  here     = LocalShadowCoord(view, biasedPos);
    // A texel of perspective depth shrinks with distance, so the slope's limit
    // is measured at the receiver.
    float texelDepth = view.pcss.y / distance;
    return ReceiverPlaneSlope(LocalShadowCoord(view, biasedPos + first * probe) - here,
                              LocalShadowCoord(view, biasedPos + cross(N, first) * probe) - here,
                              texelDepth / view.params.z);
}

// How lit a tap @p offset from @p receiver's point is. The tap is clamped to
// the tile, and is compared from where it lands.
float LocalTap(ShadowViewRow view, vec2 offset, Receiver receiver)
{
    vec2      tapUv  = clamp(receiver.uv + offset, view.clampUv.xy, view.clampUv.zw);
    vec2      texels = vec2(textureSize(uShadowAtlas, 0).xy);
    TexelQuad quad   = QuadAt(tapUv, texels);
    vec4      stored =
        textureGather(sampler2DArray(uShadowAtlas, uShadowSampler), vec3(quad.gatherUv, kStillLayer), 0);
    if (HasMovingLayer(view))
    {
        stored = min(stored,
                     textureGather(sampler2DArray(uShadowAtlas, uShadowSampler), vec3(quad.gatherUv, kMovingLayer), 0));
    }
    return CompareTexels(stored, quad, texels, tapUv, receiver);
}

// The Vogel disk at @p stepUv inside one tile, with the probe early-out: most
// receivers are wholly lit or wholly shadowed, and there four well-spread taps
// agree and the other twelve would only agree with them. The rest of the disk
// runs where the probe straddles an edge.
float FilterLocalVogel(ShadowViewRow view, Receiver receiver, float stepUv)
{
    vec2  turn = VogelTurn();
    float sum  = 0.0;
    for (uint i = 0u; i < kVogelProbeTaps; ++i)
    {
        sum += LocalTap(view, VogelOffset(kVogelProbe[i], turn, kVogelRadiusSteps * stepUv), receiver);
    }
    if (ProbeAgrees(sum, kVogelProbeTaps))
    {
        return sum / float(kVogelProbeTaps);
    }
    for (uint i = 0u; i < kVogelTaps - kVogelProbeTaps; ++i)
    {
        sum += LocalTap(view, VogelOffset(kVogelRest[i], turn, kVogelRadiusSteps * stepUv), receiver);
    }
    return sum / float(kVogelTaps);
}

// The 3x3 or 5x5 grid of @p radius, a texel apart, inside one tile, probed
// first at its centre and corners.
float FilterLocalGrid(ShadowViewRow view, Receiver receiver, int radius)
{
    float step = view.params.z;
    float sum  = LocalTap(view, vec2(0.0), receiver);
    for (uint i = 0u; i < kGridProbeTaps - 1u; ++i)
    {
        sum += LocalTap(view, kGridCorners[i] * (float(radius) * step), receiver);
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
                sum += LocalTap(view, vec2(float(x), float(y)) * step, receiver);
            }
        }
    }
    return sum / PcfTapCount(radius);
}

// CascadeBlockerGap's search, inside one tile. @p texelDepth is one texel of
// depth at the receiver's distance.
float LocalBlockerGap(ShadowViewRow view, Receiver receiver, float texelDepth)
{
    float searchUv  = PcssPenumbraUv(view.pcss.x, receiver.reference, view.pcss.z);
    float threshold = kPcssBlockerMinDepthTexels * texelDepth;
    ivec2 size      = textureSize(uShadowAtlas, 0).xy;
    vec2  turn      = VogelTurn();
    bool  moving    = HasMovingLayer(view);

    float gapSum   = 0.0;
    float blockers = 0.0;
    for (uint i = 0u; i < kVogelTaps; ++i)
    {
        vec2  tapUv  = clamp(receiver.uv + VogelOffset(i, turn, searchUv), view.clampUv.xy, view.clampUv.zw);
        ivec2 texel  = min(ivec2(tapUv * vec2(size)), size - 1);
        vec2  centre = (vec2(texel) + 0.5) / vec2(size);
        float gap    = ComparedDepth(receiver, centre - receiver.uv) - LocalStoredDepth(texel, moving);
        if (gap > threshold)
        {
            gapSum += gap;
            blockers += 1.0;
        }
    }
    return blockers > 0.0 ? gapSum / blockers : 0.0;
}

// A local light through @p view, contact-hardened.
float LocalVisibilityPcss(ShadowViewRow view, Receiver receiver, float axisDepth)
{
    // A texel of perspective depth shrinks with distance, so it is measured at
    // the receiver.
    float texelDepth = view.pcss.y / max(axisDepth, kMinAxisDepth);
    float gap        = LocalBlockerGap(view, receiver, texelDepth);
    if (gap == 0.0)
    {
        return 1.0;
    }
    float stepUv = PcssPenumbraUv(view.pcss.x, gap, view.pcss.z) / kVogelRadiusSteps;
    return FilterLocalVogel(view, receiver, stepUv);
}

// Fraction of a local light reaching @p surface through view @p viewIndex:
// 1 lit, 0 shadowed. @p axisDepth is the distance along the view's own axis,
// which is what a texel's footprint scales with.
float LocalVisibility(uint viewIndex, ShadowSurface surface, float axisDepth)
{
    ShadowViewRow view = shadowViews[viewIndex];

    float sinTheta  = sqrt(max(1.0 - surface.NdotL * surface.NdotL, 0.0));
    vec3  biasedPos = surface.position + surface.normal * (view.params.y * axisDepth * sinTheta);

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

    Receiver receiver = Receiver(uv, LocalReceiverSlope(view, biasedPos, surface.plane, axisDepth),
                                 ndc.z - view.params.x / max(axisDepth, kMinAxisDepth));

    if (uFrame.localShadowCounts.z == 1u)
    {
        return LocalVisibilityPcss(view, receiver, axisDepth);
    }

    uint filterMode = uFrame.localShadowCounts.x;
    if (filterMode == kShadowFilterPoint)
    {
        return LocalTap(view, vec2(0.0), receiver);
    }
    if (filterMode == kShadowFilterVogel)
    {
        return FilterLocalVogel(view, receiver, view.params.z);
    }
    return FilterLocalGrid(view, receiver, filterMode == kShadowFilterPcf5 ? kPcf5Radius : kPcf3Radius);
}

// A spot light's visibility; its view looks down the cone's axis.
float SpotVisibility(uint shadowView, vec3 spotDir, vec3 lightPos, ShadowSurface surface)
{
    if (shadowView == kNoShadowView)
    {
        return 1.0;
    }
    float axisDepth = max(dot(surface.position - lightPos, spotDir), 0.0);
    return LocalVisibility(shadowView, surface, axisDepth);
}

// A point light's visibility, through the face @p surface falls in. That face's
// axis is the dominant one, so the largest component of |d| is the depth along it.
float PointVisibility(uint firstView, vec3 lightPos, ShadowSurface surface)
{
    if (firstView == kNoShadowView)
    {
        return 1.0;
    }
    vec3  d         = surface.position - lightPos;
    float axisDepth = max(max(abs(d.x), abs(d.y)), abs(d.z));
    return LocalVisibility(firstView + PointLightFace(d), surface, axisDepth);
}
