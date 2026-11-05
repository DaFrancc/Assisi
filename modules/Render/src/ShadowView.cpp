/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/ShadowView.hpp>
#include <Assisi/Math/Float.hpp>

#include <algorithm>
#include <cmath>

namespace Assisi::Render
{
namespace
{
/// The near plane of a local light's frustum, as a fraction of its range.
///
/// A fraction rather than a constant because a spot lighting a desk and one
/// lighting a hangar differ by two orders of magnitude in reach, and a fixed
/// near plane spends the whole depth format on whichever of the two it was
/// chosen for. Holding the ratio fixed instead gives every light the same depth
/// resolution as a fraction of its own reach.
///
/// It is also a dead zone, and that is what sets the value. Nothing nearer than
/// this is recorded, and a point light's six near planes bound a *cube* of this
/// half-extent around it — so a caster inside that cube casts nothing at all. At
/// a twentieth of the range that cube is a metre across for an ordinary lamp,
/// which swallows the objects sitting closest to it: the shade around a bulb,
/// the table under a lamp, the wall a torch is held against.
///
/// A hundredth puts it at 20 cm for the same lamp and holds the near-to-far
/// ratio at 100, which D16 resolves to about a centimetre at the far edge of the
/// light's reach — where the shadow is dimmest and the bias is already widest.
/// That is the right way round: precision spent where the shadow is read, not on
/// a margin nothing occupies.
constexpr float kLocalNearFraction = 0.01f;

/// The floor under that, for a light authored with a range of nearly nothing.
/// A zero near plane makes the projection singular.
constexpr float kLocalMinNear = 0.01f;

/// The widest and narrowest full field of view a local light's frustum may take.
/// The ceiling is short of 180 because tan(fov/2) diverges there.
constexpr float kMinLocalFovDegrees = 1.0f;
constexpr float kMaxLocalFovDegrees = 175.0f;

float SafeRange(float range)
{
    return Math::IsFinite(range) && range > 0.f ? range : 1.f;
}

/// An up axis that is not parallel to @p forward, so lookAt has a basis to build.
glm::vec3 UpFor(const glm::vec3 &forward)
{
    // +Y everywhere except looking along it, where the usual choice is
    // degenerate and the basis collapses.
    return std::abs(forward.y) > 0.999f ? glm::vec3(0.f, 0.f, 1.f) : glm::vec3(0.f, 1.f, 0.f);
}

/// The view for one local light frustum, with the tile and biases filled in.
///
/// @p forward and @p fovDegrees are the caller's per-kind reading of the pose's
/// aim — a spot's own cone, or the axis of one face of a point light's cube —
/// which is why they are arguments rather than read from @p pose here.
ShadowView LocalShadowView(const LocalShadowLightPose &pose, const glm::vec3 &forward, float fovDegrees,
                           const ShadowAtlasTile &tile, const LocalShadowSettings &settings)
{
    const float safeRange = SafeRange(pose.range);
    const float nearPlane = std::max(safeRange * kLocalNearFraction, kLocalMinNear);
    const float fov = std::clamp(fovDegrees, kMinLocalFovDegrees, kMaxLocalFovDegrees);
    const float tanHalfFov = std::tan(glm::radians(fov) * 0.5f);

    ShadowView view;
    view.viewProjection = glm::perspective(glm::radians(fov), 1.f, nearPlane, safeRange) *
                          glm::lookAt(pose.position, pose.position + forward, UpFor(forward));
    view.rect = tile.rect;
    view.targetResolution = tile.atlasResolution;
    // The still layer. The pass raises this to 1 on a tile whose moving layer
    // is live this frame.
    view.arraySlice = 0;
    // Both are coefficients the shader scales by the receiver's own distance
    // from the light, not figures fixed at the far plane. See their declarations.
    view.depthBias = LocalDepthBiasNdcTimesDistance(tile.rect.width, nearPlane, safeRange, tanHalfFov, settings);
    view.normalOffset = LocalNormalOffsetPerDistance(tile.rect.width, tanHalfFov, settings);
    view.filterTapStepUv = LocalFilterTapStepUv(tile.atlasResolution);
    view.clampUv = ShadowViewClampUv(view, settings.filter);

    // The penumbra is quoted in the tile's UV and the lookup samples the
    // atlas's, so it shrinks by the tile's share of the atlas.
    const LocalShadowSettings safe = Sanitized(settings);
    const float tileShare = ShadowViewUvScaleOffset(view).x;
    view.pcssPenumbraUvPerDepth =
        LocalPenumbraUvPerDepth(safe.sourceRadius, tanHalfFov, nearPlane, safeRange) * tileShare;
    view.pcssTexelDepthTimesDistance = LocalTexelDepthTimesDistance(tile.rect.width, tanHalfFov, nearPlane, safeRange);
    view.pcssMaxReachUv = kPcssMaxReachTexels * view.filterTapStepUv;
    return view;
}

/// Past this, a normal lies too near the X axis for their cross product to be a
/// well-conditioned tangent, and the Y axis is used instead. Must match
/// shadow_filters.glsl's kTangentAxisLimit.
constexpr float kTangentAxisLimit = 0.9f;

/// How far along the receiver its plane is measured, as a fraction of its
/// distance from the light. A projection maps a plane to a plane, so the answer
/// is the same at any distance; a fraction of the receiver's own keeps both
/// points in front of a local light and their depths far enough apart for a
/// float to tell. Must match local_shadows.glsl's kReceiverSlopeProbe.
constexpr float kReceiverSlopeProbe = 1.f / 16.f;

/// A unit vector along the surface @p normal faces. Any one will do: the plane
/// is fixed by it and its cross product with @p normal.
glm::vec3 ReceiverTangent(const glm::vec3 &normal)
{
    const glm::vec3 axis = std::abs(normal.x) < kTangentAxisLimit ? glm::vec3(1.f, 0.f, 0.f) : glm::vec3(0.f, 1.f, 0.f);
    return glm::normalize(glm::cross(normal, axis));
}

/// Where @p world lands in @p view's target: UV in xy, comparison depth in z.
/// The same as mesh.frag's ShadowCoord and LocalShadowCoord.
glm::vec3 TargetCoord(const ShadowView &view, const glm::vec3 &world)
{
    const glm::vec4 clip = view.viewProjection * glm::vec4(world, 1.f);
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    // The map is drawn through a flipped viewport, so its first row is ndc.y = +1.
    const glm::vec2 viewUv = glm::vec2(ndc) * glm::vec2(0.5f, -0.5f) + 0.5f;
    const glm::vec4 scaleOffset = ShadowViewUvScaleOffset(view);
    return glm::vec3(viewUv * glm::vec2(scaleOffset) + glm::vec2(scaleOffset.z, scaleOffset.w), ndc.z);
}
} // namespace

glm::vec4 ShadowViewUvScaleOffset(const ShadowView &view)
{
    if (view.targetResolution == 0)
    {
        return glm::vec4(1.f, 1.f, 0.f, 0.f);
    }
    const float target = static_cast<float>(view.targetResolution);
    return glm::vec4(static_cast<float>(view.rect.width) / target, static_cast<float>(view.rect.height) / target,
                     static_cast<float>(view.rect.x) / target, static_cast<float>(view.rect.y) / target);
}

nvrhi::Viewport ShadowViewViewport(const ShadowView &view)
{
    const auto x = static_cast<float>(view.rect.x);
    const auto y = static_cast<float>(view.rect.y);
    return nvrhi::Viewport(x, x + static_cast<float>(view.rect.width), y, y + static_cast<float>(view.rect.height), 0.f,
                           1.f);
}

glm::vec2 ReceiverPlaneSlope(const ShadowView &view, const glm::vec3 &position, const glm::vec3 &normal,
                             float depthPerUv)
{
    // Two steps along the surface, as far as the receiver is from the light
    // times kReceiverSlopeProbe. A cascade's w is 1, so its steps are that
    // fraction of a world unit.
    const float probe = (view.viewProjection * glm::vec4(position, 1.f)).w * kReceiverSlopeProbe;
    const glm::vec3 first = ReceiverTangent(normal);
    const glm::vec3 second = glm::cross(normal, first);
    const glm::vec3 here = TargetCoord(view, position);
    const glm::vec3 alongFirst = TargetCoord(view, position + first * probe) - here;
    const glm::vec3 alongSecond = TargetCoord(view, position + second * probe) - here;

    // The (du, dv) of both steps against their change in depth, solved for the
    // depth's gradient.
    const float det = alongFirst.x * alongSecond.y - alongFirst.y * alongSecond.x;
    if (det == 0.f)
    {
        return glm::vec2(0.f);
    }
    const glm::vec2 slope = glm::vec2(alongFirst.z * alongSecond.y - alongFirst.y * alongSecond.z,
                                      alongFirst.x * alongSecond.z - alongFirst.z * alongSecond.x) /
                            det;
    const float limit = kMaxReceiverSlopeTexels * depthPerUv;
    const float length = glm::length(slope);
    return length > limit ? slope * (limit / length) : slope;
}

float ShadowComparedDepth(float reference, const glm::vec2 &offset, const glm::vec2 &slope)
{
    return reference + std::min(glm::dot(offset, slope), 0.f);
}

ShadowViewGpu PackShadowView(const ShadowView &view)
{
    ShadowViewGpu packed;
    packed.viewProjection = view.viewProjection;
    packed.uvScaleOffset = ShadowViewUvScaleOffset(view);
    packed.params =
        glm::vec4(view.depthBias, view.normalOffset, view.filterTapStepUv, static_cast<float>(view.arraySlice));
    packed.clampUv = view.clampUv;
    packed.pcss = glm::vec4(view.pcssPenumbraUvPerDepth, view.pcssTexelDepthTimesDistance, view.pcssMaxReachUv, 0.f);
    return packed;
}

ShadowView CascadeShadowView(const ShadowCascade &cascade, std::uint32_t slice, const SunShadowSettings &settings)
{
    const SunShadowSettings safe = Sanitized(settings);

    ShadowView view;
    view.viewProjection = cascade.viewProjection;
    view.rect = ShadowViewRect{.x = 0, .y = 0, .width = safe.resolution, .height = safe.resolution};
    view.targetResolution = safe.resolution;
    view.arraySlice = slice;
    // A cascade fits an ortho box, which is what lets the cull keep a caster
    // upstream of the near plane for the depth pass to flatten onto it.
    view.orthographic = true;
    view.depthBias = CascadeDepthBiasNdc(cascade, safe);
    view.normalOffset = CascadeNormalOffsetWorld(cascade, safe);
    view.filterTapStepUv = FilterTapStepUv(safe);
    // A cascade owns its whole slice, so there is nothing next to it to read
    // into and the clamp is the whole target.
    view.clampUv = glm::vec4(0.f, 0.f, 1.f, 1.f);
    return view;
}

glm::vec3 PointLightFaceDirection(std::uint32_t face)
{
    switch (face)
    {
    case kPointLightFacePositiveX:
        return glm::vec3(1.f, 0.f, 0.f);
    case kPointLightFaceNegativeX:
        return glm::vec3(-1.f, 0.f, 0.f);
    case kPointLightFacePositiveY:
        return glm::vec3(0.f, 1.f, 0.f);
    case kPointLightFaceNegativeY:
        return glm::vec3(0.f, -1.f, 0.f);
    case kPointLightFacePositiveZ:
        return glm::vec3(0.f, 0.f, 1.f);
    default:
        return glm::vec3(0.f, 0.f, -1.f);
    }
}

std::uint32_t PointLightFaceOf(const glm::vec3 &direction)
{
    // The axis the direction leans on hardest, then its sign. The comparisons
    // are inclusive so a direction exactly on a seam lands on one face rather
    // than falling through to the last — and either face of a seam holds the
    // geometry, because the faces are drawn overlapping.
    const glm::vec3 magnitude = glm::abs(direction);
    if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z)
    {
        return direction.x >= 0.f ? kPointLightFacePositiveX : kPointLightFaceNegativeX;
    }
    if (magnitude.y >= magnitude.z)
    {
        return direction.y >= 0.f ? kPointLightFacePositiveY : kPointLightFaceNegativeY;
    }
    return direction.z >= 0.f ? kPointLightFacePositiveZ : kPointLightFaceNegativeZ;
}

float LocalTexelsPerUnitDistance(std::uint32_t tileResolution, float tanHalfFov)
{
    if (tileResolution == 0u || !Math::IsFinite(tanHalfFov))
    {
        return 0.f;
    }
    // The frustum spans 2 * tan(fov/2) across per unit of depth, and the tile's
    // texels divide that. No distance appears: that is the caller's to supply,
    // and supplying it is the whole point.
    return 2.f * tanHalfFov / static_cast<float>(tileResolution);
}

float LocalDepthBiasNdcTimesDistance(std::uint32_t tileResolution, float nearPlane, float farPlane, float tanHalfFov,
                                     const LocalShadowSettings &settings)
{
    const LocalShadowSettings safe = Sanitized(settings);
    const float perDistance = LocalTexelsPerUnitDistance(tileResolution, tanHalfFov);
    if (perDistance <= 0.f || !Math::IsFinite(farPlane) || farPlane <= nearPlane || nearPlane <= 0.f)
    {
        return 0.f;
    }
    // At distance z a texel covers `perDistance * z` world units, and a world
    // unit there is worth `near * far / ((far - near) * z * z)` of [0, 1] depth.
    // Their product is this over z — so the shader's one divide is what turns a
    // coefficient into the bias at the distance the receiver actually sits.
    return safe.depthBiasTexels * perDistance * nearPlane * farPlane / (farPlane - nearPlane);
}

float LocalNormalOffsetPerDistance(std::uint32_t tileResolution, float tanHalfFov, const LocalShadowSettings &settings)
{
    const LocalShadowSettings safe = Sanitized(settings);
    return safe.normalOffsetTexels * LocalTexelsPerUnitDistance(tileResolution, tanHalfFov);
}

float LocalPenumbraUvPerDepth(float sourceRadius, float tanHalfFov, float nearPlane, float farPlane)
{
    if (!(sourceRadius > 0.f) || !(tanHalfFov > 0.f) || !Math::IsFinite(farPlane) || farPlane <= nearPlane ||
        nearPlane <= 0.f)
    {
        return 0.f;
    }
    // Stored depth is far (d - near) / ((far - near) d), so 1/dB - 1/dR is the
    // depth difference times (far - near) / (near far).
    return sourceRadius * (farPlane - nearPlane) / (2.f * tanHalfFov * nearPlane * farPlane);
}

float LocalTexelDepthTimesDistance(std::uint32_t tileResolution, float tanHalfFov, float nearPlane, float farPlane)
{
    const float perDistance = LocalTexelsPerUnitDistance(tileResolution, tanHalfFov);
    if (perDistance <= 0.f || !Math::IsFinite(farPlane) || farPlane <= nearPlane || nearPlane <= 0.f)
    {
        return 0.f;
    }
    // The footprint at distance z times what a world unit is worth in depth
    // there, as in LocalDepthBiasNdcTimesDistance: the product is this over z.
    return perDistance * nearPlane * farPlane / (farPlane - nearPlane);
}

float LocalFilterTapStepUv(std::uint32_t atlasResolution)
{
    return atlasResolution == 0u ? 0.f : 1.f / static_cast<float>(atlasResolution);
}

float LocalSlopeBiasClampNdc(std::uint32_t tileResolution)
{
    if (tileResolution == 0u)
    {
        return 0.f;
    }
    // One texel's worth of depth at the far plane. A texel's footprint grows with
    // distance while a world unit's worth of depth falls with its square, so a
    // texel spans the least depth out there — and a cap of one texel at the far
    // plane is under a texel everywhere nearer.
    //
    // Both terms carry a factor of the far plane and it cancels: what a texel
    // covers grows with the range, and what a world unit is worth in depth
    // shrinks with it by the same factor. That is only true because the near
    // plane is a fixed fraction of the far one — the fraction, not the range, is
    // what sets the curve.
    const float tanHalfFov = std::tan(glm::radians(kPointLightFaceFovDegrees * 0.5f));
    const float ndcPerWorldTimesFar = kLocalNearFraction / (1.f - kLocalNearFraction);
    const float worldPerTexelOverFar = 2.f * tanHalfFov / static_cast<float>(tileResolution);
    return ndcPerWorldTimesFar * worldPerTexelOverFar;
}

glm::vec4 ShadowViewClampUv(const ShadowView &view, ShadowFilter filter)
{
    const glm::vec4 scaleOffset = ShadowViewUvScaleOffset(view);
    const glm::vec2 minUv(scaleOffset.z, scaleOffset.w);
    const glm::vec2 maxUv = minUv + glm::vec2(scaleOffset.x, scaleOffset.y);

    // The kernel's outermost tap, plus the half texel a tap's bilinear blend
    // reaches on top of wherever it lands.
    const float inset = view.filterTapStepUv * (FilterRadiusTaps(filter) + 0.5f);
    const glm::vec2 room = (maxUv - minUv) * 0.5f;
    if (inset >= room.x || inset >= room.y)
    {
        // A tile smaller than the kernel has no interior left. Collapsing to its
        // centre keeps every tap inside this light's own depth, which is a
        // harder shadow rather than a wrong one.
        const glm::vec2 centre = (minUv + maxUv) * 0.5f;
        return glm::vec4(centre, centre);
    }
    return glm::vec4(minUv + inset, maxUv - inset);
}

ShadowView SpotShadowView(const LocalShadowLightPose &pose, const ShadowAtlasTile &tile,
                          const LocalShadowSettings &settings)
{
    // The cone's outer angle is a half-angle, so the full field of view is twice
    // it — widened for the same reason a point light's faces are, so the filter
    // at the cone's rim reads depth this light recorded rather than the tile
    // beside it.
    const float outer = Math::IsFinite(pose.outerAngleDegrees)
                            ? std::clamp(pose.outerAngleDegrees, 0.f, Math::kMaxConeHalfAngleDegrees)
                            : Math::kDefaultSpotOuterAngleDegrees;
    const float fov = outer * 2.f + kPointLightFaceOverlapDegrees;

    const float lengthSq = glm::dot(pose.direction, pose.direction);
    const glm::vec3 forward = lengthSq > 0.f ? pose.direction / std::sqrt(lengthSq) : glm::vec3(0.f, -1.f, 0.f);
    return LocalShadowView(pose, forward, fov, tile, settings);
}

ShadowView PointFaceShadowView(const LocalShadowLightPose &pose, std::uint32_t face, const ShadowAtlasTile &tile,
                               const LocalShadowSettings &settings)
{
    return LocalShadowView(pose, PointLightFaceDirection(face), kPointLightFaceFovDegrees, tile, settings);
}

} // namespace Assisi::Render
