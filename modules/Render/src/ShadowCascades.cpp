/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/ShadowCascades.hpp>

#include <algorithm>
#include <cmath>

namespace Assisi::Render
{
namespace
{
/// A near plane of zero would make the logarithmic split term `pow(far/0, t)`,
/// which is inf, and every split after it NaN. Cameras validate their near
/// plane, so this is belt and braces rather than an expected path.
constexpr float kMinNearZ = 1.0e-4f;

/// Below this the slice has no depth and the sphere fit's `t` divides by zero.
constexpr float kMinSliceLength = 1.0e-6f;

/// glm::normalize of a zero vector is NaN, and a zero light direction reaches
/// here straight from a hand-edited level file.
constexpr glm::vec3 kFallbackLightDirection{0.f, -1.f, 0.f};

glm::vec3 SafeLightDirection(const glm::vec3 &direction)
{
    const float lengthSq = glm::dot(direction, direction);
    return lengthSq > 0.f && std::isfinite(lengthSq) ? direction / std::sqrt(lengthSq) : kFallbackLightDirection;
}
} // namespace

float PracticalSplitDistance(float nearZ, float farZ, std::uint32_t index, std::uint32_t count, float lambda)
{
    const float safeNear = std::max(nearZ, kMinNearZ);
    const float safeFar = std::max(farZ, safeNear);
    if (count == 0u || index == 0u)
    {
        return safeNear;
    }
    if (index >= count)
    {
        return safeFar;
    }

    const float t = static_cast<float>(index) / static_cast<float>(count);
    const float uniform = safeNear + (safeFar - safeNear) * t;
    const float logarithmic = safeNear * std::pow(safeFar / safeNear, t);
    const float weight = std::clamp(lambda, kMinSplitLambda, kMaxSplitLambda);
    return weight * logarithmic + (1.f - weight) * uniform;
}

Geometry::BoundingSphere FrustumSliceSphere(const glm::mat4 &inverseCameraView, float tanHalfFovY, float aspectRatio,
                                            float nearView, float farView)
{
    // The camera looks down its own -Z, so the inverse view's third column
    // points backwards; its fourth is the eye.
    const glm::vec3 forward = -glm::vec3(inverseCameraView[2]);
    const glm::vec3 eye = glm::vec3(inverseCameraView[3]);

    // Radius of the disc a frustum cap subtends at distance d: the half-height
    // and half-width are d * tanHalfFovY and that times the aspect, so the
    // corner is d * tanHalfFovY * sqrt(1 + aspect^2) from the axis. Depends on
    // nothing about the camera's orientation, which is the whole invariant.
    const float cornerScale = tanHalfFovY * std::sqrt(1.f + aspectRatio * aspectRatio);
    const float nearRadius = nearView * cornerScale;
    const float farRadius = farView * cornerScale;

    const glm::vec3 nearCenter = eye + forward * nearView;
    const float length = farView - nearView;
    if (length <= kMinSliceLength)
    {
        return Geometry::BoundingSphere{.center = nearCenter, .radius = std::max(nearRadius, farRadius)};
    }

    // The centre sits on the view axis where the two caps' corners are
    // equidistant: t^2 + rn^2 == (L - t)^2 + rf^2.
    float t = (length * length + farRadius * farRadius - nearRadius * nearRadius) / (2.f * length);

    // Outside the slice, one cap's corners enclose the other's and the sphere
    // is that cap's circumscribed one. Clamping rather than solving again keeps
    // the branch to a compare, at the cost of a sphere that is enclosing but no
    // longer minimal — which only widens texels, never drops geometry.
    t = std::clamp(t, 0.f, length);

    const glm::vec3 center = nearCenter + forward * t;
    const float radius = std::sqrt(std::max(t * t + nearRadius * nearRadius,
                                            (length - t) * (length - t) + farRadius * farRadius));
    return Geometry::BoundingSphere{.center = center, .radius = radius};
}

glm::mat4 LightRotation(const glm::vec3 &lightDirection)
{
    const glm::vec3 direction = SafeLightDirection(lightDirection);
    // A sun straight overhead is the common case, and it is exactly where a +Y
    // up axis is parallel to the light and lookAt degenerates.
    const glm::vec3 up = std::abs(direction.y) > 0.99f ? glm::vec3(0.f, 0.f, 1.f) : glm::vec3(0.f, 1.f, 0.f);
    return glm::lookAt(glm::vec3(0.f), direction, up);
}

namespace
{
/// How close to a lattice line counts as being on it, in texels.
///
/// The rotation is exact but the round trip through it is not: a point snapped
/// to a whole texel comes back a few ulps to either side of one. Landing a
/// hair below sends floor() to the texel beneath, so the snap moves a point
/// that was already on the lattice — and the cascade lurches a texel between
/// two frames whose input differed by nothing.
///
/// A thousandth of a texel is far below anything the shadow can resolve and far
/// above the round trip's error over any coordinate a level reaches.
constexpr float kSnapTexelTolerance = 1e-3f;

/// @brief @p quotient rounded down to a whole texel, snapping to the nearest
/// lattice line when it is within tolerance of one.
[[nodiscard]] float FloorTexel(float quotient)
{
    const float nearest = std::round(quotient);
    return std::abs(quotient - nearest) < kSnapTexelTolerance ? nearest : std::floor(quotient);
}
} // namespace

glm::vec3 SnapToTexelGrid(const glm::vec3 &center, const glm::mat4 &lightRotation, float worldUnitsPerTexel)
{
    if (!(worldUnitsPerTexel > 0.f) || !std::isfinite(worldUnitsPerTexel))
    {
        return center;
    }

    glm::vec3 lightSpace = glm::vec3(lightRotation * glm::vec4(center, 1.f));
    lightSpace.x = FloorTexel(lightSpace.x / worldUnitsPerTexel) * worldUnitsPerTexel;
    lightSpace.y = FloorTexel(lightSpace.y / worldUnitsPerTexel) * worldUnitsPerTexel;

    // The rotation has no translation, so its inverse is its transpose exactly.
    // What error there is comes from the two multiplies, which is what the
    // tolerance above absorbs.
    return glm::vec3(glm::transpose(lightRotation) * glm::vec4(lightSpace, 1.f));
}

CascadeFit FitCascades(const CascadeFitParams &params)
{
    CascadeFit fit;

    const SunShadowSettings settings = Sanitized(params.settings);
    if (!settings.enabled)
    {
        return fit;
    }

    const float nearZ = std::max(params.nearZ, kMinNearZ);
    // The shadow distance caps the camera's far plane rather than replacing it:
    // a camera that sees less than the shadow distance shadows only what it sees.
    const float farZ = std::min(params.farZ, settings.maxDistance);
    if (!(farZ > nearZ))
    {
        return fit;
    }

    const glm::mat4 inverseCameraView = glm::inverse(params.cameraView);
    const glm::mat4 lightRotation = LightRotation(params.lightDirection);
    const glm::vec3 lightDirection = SafeLightDirection(params.lightDirection);

    // The distribution is measured from here rather than from the camera's near
    // plane, so a logarithmic split has a sane ratio to work with instead of one
    // that spends its first two cascades inside arm's reach. Clamped under half
    // the shadow distance so a short one still gets a distribution rather than a
    // single degenerate step.
    const float distributionNear = std::clamp(kSplitDistributionNear, nearZ, farZ * 0.5f);

    for (std::uint32_t i = 0; i < settings.cascadeCount; ++i)
    {
        // Cascade zero covers from the camera, whatever the distribution says:
        // moving where the splits are placed must not leave close geometry with
        // no cascade to be shadowed by.
        const float splitNear =
            i == 0 ? nearZ
                   : PracticalSplitDistance(distributionNear, farZ, i, settings.cascadeCount, settings.splitLambda);
        const float splitFar =
            PracticalSplitDistance(distributionNear, farZ, i + 1u, settings.cascadeCount, settings.splitLambda);

        const Geometry::BoundingSphere sphere =
            FrustumSliceSphere(inverseCameraView, params.tanHalfFovY, params.aspectRatio, splitNear, splitFar);

        // The box is 2r across and `resolution` texels wide, so this is what one
        // texel covers — the scale every bias in this cascade is quoted in.
        const float worldUnitsPerTexel = 2.f * sphere.radius / static_cast<float>(settings.resolution);
        const glm::vec3 center = SnapToTexelGrid(sphere.center, lightRotation, worldUnitsPerTexel);

        // Where the slice sits along the light, and how far upstream of it a
        // caster can still be. Pulling the near plane back to the casters is
        // what keeps geometry behind the camera from being clipped out of the
        // map — nvrhi only honours a disabled depth clip where the device has
        // EXT_depth_clip_enable, so clamping cannot be relied on to do it.
        const float centerAlongLight = glm::dot(center, lightDirection);
        const float sliceNearAlongLight = centerAlongLight - sphere.radius;
        const float nearAlongLight =
            params.casterNearAlongLight.has_value() && std::isfinite(*params.casterNearAlongLight)
                ? std::min(sliceNearAlongLight, *params.casterNearAlongLight)
                : sliceNearAlongLight;
        const float depthRange = (centerAlongLight + sphere.radius) - nearAlongLight;

        // The eye moves only along the light, so its light-space XY still match
        // the snapped centre's and the texel lattice survives the pull-back.
        const glm::vec3 eye = center - lightDirection * (centerAlongLight - nearAlongLight);
        const glm::mat4 lightView = lightRotation * glm::translate(glm::mat4(1.f), -eye);
        const glm::mat4 lightProjection =
            glm::ortho(-sphere.radius, sphere.radius, -sphere.radius, sphere.radius, 0.f, depthRange);

        fit.cascades[i] = ShadowCascade{.viewProjection = lightProjection * lightView,
                                        .center = center,
                                        .radius = sphere.radius,
                                        .splitNearView = splitNear,
                                        .splitFarView = splitFar,
                                        .worldUnitsPerTexel = worldUnitsPerTexel,
                                        .depthRange = depthRange};
    }

    fit.count = settings.cascadeCount;
    return fit;
}

float CascadeDepthBiasNdc(const ShadowCascade &cascade, const SunShadowSettings &settings)
{
    if (!(cascade.depthRange > 0.f))
    {
        return 0.f;
    }
    const float worldBias = settings.depthBiasTexels * cascade.worldUnitsPerTexel;
    return worldBias / cascade.depthRange;
}

float FilterRadiusTaps(ShadowFilter filter)
{
    switch (filter)
    {
    case ShadowFilter::Point:
        // No taps of its own — the hardware's bilinear comparison is the whole
        // of the footprint, and CascadePenumbraWorld adds that separately.
        return 0.f;
    case ShadowFilter::Pcf5x5:
        return 2.f;
    case ShadowFilter::Vogel:
        return 2.5f;
    case ShadowFilter::Pcf3x3:
    default:
        return 1.f;
    }
}

float FilterTapStepUv(const SunShadowSettings &settings)
{
    const std::uint32_t resolution = std::max(Sanitized(settings).resolution, 1u);
    // Never finer than a texel: below the reference size there is nothing
    // between one texel and the next to sample, so the step is the texel.
    return 1.f / static_cast<float>(std::min(resolution, kFilterReferenceResolution));
}

float ShadowTexelSizeUv(const SunShadowSettings &settings)
{
    return 1.f / static_cast<float>(std::max(Sanitized(settings).resolution, 1u));
}

float CascadeTexelScreenPixels(const ShadowCascade &cascade, float viewDistance, float screenHeight,
                               float tanHalfFovY)
{
    // A world length L at distance d spans L / (2 d tan(fov/2)) of the screen's
    // height, so one texel covers that fraction times the height in pixels.
    const float denominator = 2.f * viewDistance * tanHalfFovY;
    if (!(denominator > 0.f))
    {
        return 0.f;
    }
    return cascade.worldUnitsPerTexel * screenHeight / denominator;
}

float CascadePenumbraWorld(const ShadowCascade &cascade, const SunShadowSettings &settings)
{
    const SunShadowSettings safe = Sanitized(settings);
    const float boxWidth = 2.f * cascade.radius;
    const float kernelUv = FilterRadiusTaps(safe.filter) * FilterTapStepUv(safe);
    // The hardware compares against four texels and blends, so every filter —
    // the one-tap one included — is soft to half a texel before its own kernel
    // adds anything.
    const float bilinearUv = 0.5f / static_cast<float>(std::max(safe.resolution, 1u));
    return (kernelUv + bilinearUv) * boxWidth;
}

float CascadeNormalOffsetWorld(const ShadowCascade &cascade, const SunShadowSettings &settings)
{
    return settings.normalOffsetTexels * cascade.worldUnitsPerTexel;
}

float CascadeReceiverBiasClampNdc(const ShadowCascade &cascade, const SunShadowSettings &settings)
{
    if (!(cascade.depthRange > 0.f))
    {
        return 0.f;
    }
    // What the outermost tap of the kernel is owed by a receiver at the steepest
    // slope the gradient is trusted at. A plane needs no more than this, so
    // anything past it is a gradient taken across a silhouette rather than along
    // a surface.
    return kMaxReceiverPlaneSlope * CascadePenumbraWorld(cascade, settings) / cascade.depthRange;
}

} // namespace Assisi::Render
