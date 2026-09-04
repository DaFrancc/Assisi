/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/ShadowView.hpp>

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
    return std::isfinite(range) && range > 0.f ? range : 1.f;
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
    // Every tile shares slice zero: the atlas is one texture, and the slice lane
    // exists for the cascade array that is not.
    view.arraySlice = 0;
    // Both are coefficients the shader scales by the receiver's own distance
    // from the light, not figures fixed at the far plane. See their declarations.
    view.depthBias = LocalDepthBiasNdcTimesDistance(tile.rect.width, nearPlane, safeRange, tanHalfFov, settings);
    view.normalOffset = LocalNormalOffsetPerDistance(tile.rect.width, tanHalfFov, settings);
    view.filterTapStepUv = LocalFilterTapStepUv(tile.atlasResolution);
    view.clampUv = ShadowViewClampUv(view, settings.filter);
    return view;
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

ShadowViewGpu PackShadowView(const ShadowView &view)
{
    ShadowViewGpu packed;
    packed.viewProjection = view.viewProjection;
    packed.uvScaleOffset = ShadowViewUvScaleOffset(view);
    packed.params =
        glm::vec4(view.depthBias, view.normalOffset, view.filterTapStepUv, static_cast<float>(view.arraySlice));
    packed.clampUv = view.clampUv;
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
    // A cascade fits an ortho box, which is what lets the depth pass pancake a
    // caster upstream of the near plane instead of clipping it.
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
    if (tileResolution == 0u || !std::isfinite(tanHalfFov))
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
    if (perDistance <= 0.f || !std::isfinite(farPlane) || farPlane <= nearPlane || nearPlane <= 0.f)
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
    // One texel's worth of depth at the far plane, which is where the depth
    // curve is flattest and a texel therefore buys the most NDC.
    //
    // Both terms carry a factor of the far plane and it cancels: what a texel
    // covers grows with the range, and what a world unit is worth in depth
    // shrinks with it by the same factor. That is only true because the near
    // plane is a fixed fraction of the far one — the fraction, not the range, is
    // what sets the curve.
    const float tanHalfFov = std::tan(glm::radians((90.f + kPointLightFaceOverlapDegrees) * 0.5f));
    const float ndcPerWorldTimesFar = kLocalNearFraction / (1.f - kLocalNearFraction);
    const float worldPerTexelOverFar = 2.f * tanHalfFov / static_cast<float>(tileResolution);
    return ndcPerWorldTimesFar * worldPerTexelOverFar;
}

glm::vec4 ShadowViewClampUv(const ShadowView &view, ShadowFilter filter)
{
    const glm::vec4 scaleOffset = ShadowViewUvScaleOffset(view);
    const glm::vec2 minUv(scaleOffset.z, scaleOffset.w);
    const glm::vec2 maxUv = minUv + glm::vec2(scaleOffset.x, scaleOffset.y);

    // The kernel's outermost tap, plus the half texel the hardware's own
    // bilinear comparison reaches on top of wherever a tap lands.
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
    const float outer = std::isfinite(pose.outerAngleDegrees)
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
    return LocalShadowView(pose, PointLightFaceDirection(face), 90.f + kPointLightFaceOverlapDegrees, tile, settings);
}

} // namespace Assisi::Render
