// Spot and point light shadows. A spot's map and each of a point light's six
// cube faces is a perspective view into its own rectangle (tile) of one shared
// atlas.
//
// Every lookup compares each texel it reads against the receiver's own plane at
// that texel's centre, not against one depth for the whole kernel. A perspective
// projection maps a plane to a plane, so the receiver's tangent plane is exactly
// linear in the atlas's (u, v, depth), and a flat receiver compared this way
// matches the map's record of it at every texel of any kernel. That is what lets
// the biases stay a fraction of a texel instead of growing with the kernel, and
// the biases are what pull a shadow off its caster.
//
// The kernel is a tent (see TentTexelWeight), read in 2x2 blocks by gather. Its
// centre clamps to its tile: the sampler clamps only to the whole atlas, and a
// texel past the tile's edge is the neighbouring light's depth. Faces are drawn a
// few degrees wider than they cover, so the clamp only moves a lookup that was
// already in that margin.

layout(binding = 9) uniform texture2D uShadowAtlas;

// The nearest a receiver is taken to be to its light, in world units, so the
// per-distance biases never divide by zero.
const float kMinAxisDepth = 1e-4;

// Tent half-widths in texels. Must match Render::kLocal*HalfWidthTexels.
const float kLocalPointHalfWidth = 1.0;
const float kLocalPcf3HalfWidth  = 1.5;
const float kLocalPcf5HalfWidth  = 2.5;
const float kLocalVogelHalfWidth = 3.5;

// The least a lookup is biased by, in steps of the atlas's depth format: the
// stored depth is rounded to the nearest step.
const float kDepthBiasFloorSteps = 1.0;

// How far the two points that fix the receiver's plane sit from it, per unit of
// its distance from the light. The plane stays a plane through the projection,
// so any step is exact; this one keeps the differences well above float noise.
const float kPlaneProbeStepPerDistance = 0.01;

// Past this |N.y| the receiver's first tangent is built from +X rather than +Y.
const float kTangentUpLimit = 0.99;

// The steepest a receiver's plane is followed exactly, in texels of depth per
// texel: 45 degrees from facing the light. See PlaneDepthAt.
const float kTrustedSlope = 1.0;

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
    // The map is drawn through a flipped viewport, so its first row is ndc.y = +1.
    vec2 tileUv = ndc.xy * vec2(0.5, -0.5) + 0.5;
    return vec3(tileUv * view.uvScaleOffset.xy + view.uvScaleOffset.zw, ndc.z);
}

// The receiver's plane through @p here (LocalShadowCoord of @p worldPos): how its
// stored depth changes per atlas texel across and down. @p N is its geometric
// normal. Clamped where the plane turns edge-on to the light.
vec2 LocalReceiverSlope(ShadowViewRow view, vec3 worldPos, vec3 N, vec3 here, float axisDepth)
{
    vec3  up         = abs(N.y) < kTangentUpLimit ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    float probe      = axisDepth * kPlaneProbeStepPerDistance;
    vec3  t1         = normalize(cross(N, up)) * probe;
    vec3  t2         = cross(N, t1);
    vec3  a          = LocalShadowCoord(view, worldPos + t1) - here;
    vec3  b          = LocalShadowCoord(view, worldPos + t2) - here;
    float texel      = view.params.z;
    float texelDepth = view.pcss.y / max(axisDepth, kMinAxisDepth);
    return ReceiverPlaneSlope(a, b, texelDepth / texel) * texel;
}

// The receiver's plane, for the kernels below: its depth at the lookup's own
// texel position @p origin, already biased, its change per texel, and one texel
// of depth at the receiver.
struct ReceiverPlane
{
    vec2  origin;
    float depth;
    vec2  slope;
    float texelDepth;
};

// The plane's depth at the centre of texel (@p x, @p y), lowered by however much
// steeper than kTrustedSlope it climbs to get there. A receiver near edge-on to
// the light climbs texels of depth per texel, and extrapolating that past its own
// edge — into the floor under a lit wall, say — puts the plane behind surfaces
// that do not shade it, which darkens the wall in streaks where it meets them.
// Lowering it can only remove occluders, never make a receiver shadow itself.
float PlaneDepthAt(ReceiverPlane plane, float x, float y)
{
    vec2 d = vec2(x, y) + 0.5 - plane.origin;
    vec2 steep = max(abs(plane.slope) - kTrustedSlope * plane.texelDepth, 0.0);
    return plane.depth + dot(d, plane.slope) - dot(abs(d), steep);
}

// The texels (@p x, @p y) to (@p x + 1, @p y + 1), as a gather returns them:
// .w = (x, y), .z = (x + 1, y), .x = (x, y + 1), .y = (x + 1, y + 1).
vec4 GatherAtlas(float x, float y, vec2 atlasSize)
{
    return textureGather(sampler2D(uShadowAtlas, uClampSampler), (vec2(x, y) + 1.0) / atlasSize);
}

// The fraction of a tent of half-width @p w texels, centred at texel position
// @p centre, that sees past the map to the light.
float LocalTentVisibility(ReceiverPlane plane, vec2 centre, float w, vec2 atlasSize)
{
    vec2  first = floor(centre - w);
    vec2  last  = floor(centre + w);
    float sum   = 0.0;
    for (float y = first.y; y <= last.y; y += 2.0)
    {
        float wy0 = TentTexelWeight(y, centre.y, w);
        float wy1 = TentTexelWeight(y + 1.0, centre.y, w);
        for (float x = first.x; x <= last.x; x += 2.0)
        {
            float wx0    = TentTexelWeight(x, centre.x, w);
            float wx1    = TentTexelWeight(x + 1.0, centre.x, w);
            vec4  stored = GatherAtlas(x, y, atlasSize);
            sum += wx0 * wy0 * step(PlaneDepthAt(plane, x, y), stored.w);
            sum += wx1 * wy0 * step(PlaneDepthAt(plane, x + 1.0, y), stored.z);
            sum += wx0 * wy1 * step(PlaneDepthAt(plane, x, y + 1.0), stored.x);
            sum += wx1 * wy1 * step(PlaneDepthAt(plane, x + 1.0, y + 1.0), stored.y);
        }
    }
    return sum;
}

// The mean depth by which the map's texels within @p w of @p centre sit in front
// of the receiver's plane, counting only those that do and weighting each by the
// same tent, so a blocker at the rim fades in rather than switching the answer.
// Zero where nothing is in front.
float LocalBlockerGap(ReceiverPlane plane, vec2 centre, float w, vec2 atlasSize)
{
    vec2  first    = floor(centre - w);
    vec2  last     = floor(centre + w);
    float gapSum   = 0.0;
    float blockers = 0.0;
    for (float y = first.y; y <= last.y; y += 2.0)
    {
        vec2 wy = vec2(TentTexelWeight(y, centre.y, w), TentTexelWeight(y + 1.0, centre.y, w));
        for (float x = first.x; x <= last.x; x += 2.0)
        {
            vec2  wx     = vec2(TentTexelWeight(x, centre.x, w), TentTexelWeight(x + 1.0, centre.x, w));
            vec4  stored = GatherAtlas(x, y, atlasSize);
            vec4  gap    = vec4(PlaneDepthAt(plane, x, y + 1.0), PlaneDepthAt(plane, x + 1.0, y + 1.0),
                                PlaneDepthAt(plane, x + 1.0, y), PlaneDepthAt(plane, x, y)) -
                           stored;
            vec4  weight = vec4(wx.x * wy.y, wx.y * wy.y, wx.y * wy.x, wx.x * wy.x) *
                           vec4(greaterThan(gap, vec4(0.0)));
            gapSum += dot(weight, gap);
            blockers += dot(weight, vec4(1.0));
        }
    }
    return blockers > 0.0 ? gapSum / blockers : 0.0;
}

// The tent half-width the frame's fixed filter takes.
float LocalFilterHalfWidth(uint filterMode)
{
    if (filterMode == kShadowFilterPoint)
    {
        return kLocalPointHalfWidth;
    }
    if (filterMode == kShadowFilterPcf5)
    {
        return kLocalPcf5HalfWidth;
    }
    if (filterMode == kShadowFilterVogel)
    {
        return kLocalVogelHalfWidth;
    }
    return kLocalPcf3HalfWidth;
}

// Fraction of a local light reaching @p worldPos through view @p viewIndex:
// 1 lit, 0 shadowed. @p N is the geometric normal; @p axisDepth is the distance
// along the view's own axis, which is what a texel's footprint scales with.
float LocalVisibility(uint viewIndex, vec3 worldPos, vec3 N, float NdotL, float axisDepth)
{
    ShadowViewRow view = shadowViews[viewIndex];

    // Off the surface by a fraction of a texel toward the light's side, which
    // clears the texels a curved receiver's tangent plane parts from.
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

    vec3  here      = LocalShadowCoord(view, biasedPos);
    vec2  atlasSize = vec2(textureSize(uShadowAtlas, 0));
    uint  steps     = uFrame.localShadowCounts.w;
    float depthFloor = steps > 0u ? kDepthBiasFloorSteps / float(steps) : 0.0;
    float bias      = max(view.params.x / max(axisDepth, kMinAxisDepth), depthFloor);

    ReceiverPlane plane;
    plane.origin = here.xy * atlasSize;
    plane.depth  = here.z - bias;
    plane.slope  = LocalReceiverSlope(view, biasedPos, N, here, axisDepth);
    plane.texelDepth = view.pcss.y / max(axisDepth, kMinAxisDepth);

    vec2 centre = clamp(here.xy, view.clampUv.xy, view.clampUv.zw) * atlasSize;

    float filterWidth = LocalFilterHalfWidth(uFrame.localShadowCounts.x);
    if (uFrame.localShadowCounts.z == 1u)
    {
        // Contact hardening widens the selected filter, never narrows it: a
        // kernel narrowed toward one texel where a caster touches its receiver
        // shows each texel of the edge as a step. The search reaches as far as a
        // blocker at the light itself would spread the penumbra, and the kernel
        // is sized from what it finds.
        float perDepth = view.pcss.x * atlasSize.x;
        float maxWidth = max(view.pcss.z, filterWidth);
        float search   = clamp(perDepth * here.z, filterWidth, maxWidth);
        float gap      = LocalBlockerGap(plane, centre, search, atlasSize);
        if (gap == 0.0)
        {
            return 1.0;
        }
        filterWidth = clamp(perDepth * gap, filterWidth, maxWidth);
    }
    return LocalTentVisibility(plane, centre, filterWidth, atlasSize);
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
