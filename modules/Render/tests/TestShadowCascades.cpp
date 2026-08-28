/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/ShadowCascades.hpp>
#include <Assisi/Render/ShadowSettings.hpp>

#include <cmath>
#include <limits>
#include <vector>

using namespace Assisi::Render;

namespace
{
constexpr float kTanHalfFov = 0.5773502692f; // 60 degrees vertical
constexpr float kAspect = 16.f / 9.f;

/// A camera at `position` looking along `forward`. Built the way the runtime
/// builds one (glm::lookAt from a world transform), so the fit sees exactly the
/// matrix it sees in a frame.
glm::mat4 View(const glm::vec3 &position, const glm::vec3 &forward)
{
    return glm::lookAt(position, position + glm::normalize(forward), glm::vec3(0.f, 1.f, 0.f));
}

CascadeFitParams DefaultParams()
{
    CascadeFitParams params;
    params.cameraView = View(glm::vec3(0.f), glm::vec3(0.f, 0.f, -1.f));
    params.tanHalfFovY = kTanHalfFov;
    params.aspectRatio = kAspect;
    params.nearZ = 0.1f;
    params.farZ = 200.f;
    params.lightDirection = glm::normalize(glm::vec3(-0.4f, -1.f, -0.3f));
    return params;
}

/// The world position a cascade's clip space maps back to, for asserting that
/// a point lands where the matrix says it does.
glm::vec3 ToClip(const ShadowCascade &cascade, const glm::vec3 &world)
{
    const glm::vec4 clip = cascade.viewProjection * glm::vec4(world, 1.f);
    return glm::vec3(clip) / clip.w;
}
} // namespace

TEST_CASE("Practical splits bracket the camera range and stay ordered")
{
    constexpr std::uint32_t kCount = 4;
    constexpr float kNear = 0.1f;
    constexpr float kFar = 80.f;

    CHECK(PracticalSplitDistance(kNear, kFar, 0, kCount, 0.75f) == doctest::Approx(kNear));
    CHECK(PracticalSplitDistance(kNear, kFar, kCount, kCount, 0.75f) == doctest::Approx(kFar));

    // Every interior split sits strictly between its neighbours. A scheme that
    // repeats or inverts a distance gives a cascade zero or negative depth, and
    // the sphere fit divides by that length.
    float previous = PracticalSplitDistance(kNear, kFar, 0, kCount, 0.75f);
    for (std::uint32_t i = 1; i <= kCount; ++i)
    {
        const float split = PracticalSplitDistance(kNear, kFar, i, kCount, 0.75f);
        CHECK(split > previous);
        previous = split;
    }
}

TEST_CASE("Lambda weights between the uniform and logarithmic schemes")
{
    constexpr std::uint32_t kCount = 4;
    constexpr float kNear = 1.f;
    constexpr float kFar = 81.f;

    // At the second of four splits: uniform is 1 + 80/2 = 41, logarithmic is
    // 1 * 81^(1/2) = 9. Both endpoints are exact, so lambda's meaning is
    // pinned rather than merely monotone.
    CHECK(PracticalSplitDistance(kNear, kFar, 2, kCount, 0.f) == doctest::Approx(41.f));
    CHECK(PracticalSplitDistance(kNear, kFar, 2, kCount, 1.f) == doctest::Approx(9.f));
    CHECK(PracticalSplitDistance(kNear, kFar, 2, kCount, 0.5f) == doctest::Approx(25.f));

    // A logarithmic weighting pulls every interior split nearer the camera —
    // that is what buys the near cascades their resolution.
    for (std::uint32_t i = 1; i < kCount; ++i)
    {
        CHECK(PracticalSplitDistance(kNear, kFar, i, kCount, 1.f) <
              PracticalSplitDistance(kNear, kFar, i, kCount, 0.f));
    }
}

TEST_CASE("A slice sphere's radius does not depend on where the camera looks")
{
    // The invariant the whole fit rests on: turn the camera and the ortho
    // extent must not move, or every texel changes the world it covers and the
    // shadow edges shimmer at the rate of the turn. A box fitted to the eight
    // frustum corners fails exactly here.
    const glm::vec3 eye(3.f, 2.f, -7.f);
    const float reference =
        FrustumSliceSphere(glm::inverse(View(eye, glm::vec3(0.f, 0.f, -1.f))), kTanHalfFov, kAspect, 4.f, 20.f).radius;

    const std::vector<glm::vec3> directions = {
        {0.f, 0.f, -1.f}, {1.f, 0.f, 0.f},     {0.f, 0.f, 1.f},      {-1.f, 0.f, 0.f},
        {1.f, 0.f, -1.f}, {0.3f, -0.7f, 0.6f}, {-0.2f, 0.9f, -0.4f},
    };
    for (const glm::vec3 &forward : directions)
    {
        const Assisi::Geometry::BoundingSphere sphere =
            FrustumSliceSphere(glm::inverse(View(eye, forward)), kTanHalfFov, kAspect, 4.f, 20.f);
        CHECK(sphere.radius == doctest::Approx(reference));
    }

    // Nor on where it stands: the sphere translates with the camera, radius fixed.
    const Assisi::Geometry::BoundingSphere moved =
        FrustumSliceSphere(glm::inverse(View(eye + glm::vec3(100.f, -30.f, 55.f), glm::vec3(0.f, 0.f, -1.f))),
                           kTanHalfFov, kAspect, 4.f, 20.f);
    CHECK(moved.radius == doctest::Approx(reference));
}

TEST_CASE("A slice sphere encloses every corner of its sub-frustum")
{
    // Conservative or the cascade drops geometry at its own edges. Checked
    // against the corners the sphere is meant to replace, not against itself.
    const glm::mat4 inverseView = glm::inverse(View(glm::vec3(1.f, 5.f, 2.f), glm::vec3(0.4f, -0.2f, -1.f)));

    struct Slice
    {
        float nearView;
        float farView;
    };
    const std::vector<Slice> slices = {{0.1f, 2.f}, {2.f, 12.f}, {12.f, 40.f}, {40.f, 200.f}, {30.f, 31.f}};

    for (const Slice &slice : slices)
    {
        const Assisi::Geometry::BoundingSphere sphere =
            FrustumSliceSphere(inverseView, kTanHalfFov, kAspect, slice.nearView, slice.farView);
        for (const float depth : {slice.nearView, slice.farView})
        {
            const float halfHeight = depth * kTanHalfFov;
            const float halfWidth = halfHeight * kAspect;
            for (const float sx : {-1.f, 1.f})
            {
                for (const float sy : {-1.f, 1.f})
                {
                    const glm::vec3 corner =
                        glm::vec3(inverseView * glm::vec4(sx * halfWidth, sy * halfHeight, -depth, 1.f));
                    CHECK(glm::length(corner - sphere.center) <= sphere.radius + 1e-3f);
                }
            }
        }
    }
}

TEST_CASE("Snapping quantises the centre to whole texels and is idempotent")
{
    const glm::mat4 rotation = LightRotation(glm::normalize(glm::vec3(-0.4f, -1.f, -0.3f)));
    constexpr float kTexel = 0.25f;

    const glm::vec3 point(3.13f, -1.77f, 5.02f);
    const glm::vec3 snapped = SnapToTexelGrid(point, rotation, kTexel);

    // In light space the lattice is what it claims to be: both in-plane axes
    // land on exact multiples of the texel size.
    const glm::vec3 lightSpace = glm::vec3(rotation * glm::vec4(snapped, 1.f));
    CHECK(std::abs(std::remainder(lightSpace.x, kTexel)) < 1e-4f);
    CHECK(std::abs(std::remainder(lightSpace.y, kTexel)) < 1e-4f);

    // The depth axis is untouched — snapping it would move the near plane and
    // gain nothing, since depth is compared, not filtered.
    const glm::vec3 originalLightSpace = glm::vec3(rotation * glm::vec4(point, 1.f));
    CHECK(lightSpace.z == doctest::Approx(originalLightSpace.z));

    // Never further than one texel: the snap is a quantisation, not a drift.
    CHECK(glm::length(snapped - point) <= kTexel * 1.5f);

    const glm::vec3 twice = SnapToTexelGrid(snapped, rotation, kTexel);
    CHECK(glm::length(twice - snapped) < 1e-4f);

    // A degenerate texel size degrades to unsnapped rather than to NaN.
    const glm::vec3 unsnapped = SnapToTexelGrid(point, rotation, 0.f);
    CHECK(unsnapped.x == doctest::Approx(point.x));
    CHECK(unsnapped.y == doctest::Approx(point.y));
    CHECK(unsnapped.z == doctest::Approx(point.z));
}

TEST_CASE("A cascade's texel grid holds still while the camera strafes")
{
    // The crawl test. Slide the camera by fractions of a texel and the fitted
    // centre must move in whole-texel steps only — that is what keeps the
    // shadow map resampling the same lattice frame to frame. Without the snap,
    // the centre follows the camera continuously and every edge crawls.
    CascadeFitParams params = DefaultParams();
    params.settings.cascadeCount = 1;
    params.settings.resolution = 1024;
    params.settings.maxDistance = 40.f;

    const glm::mat4 rotation = LightRotation(params.lightDirection);

    const CascadeFit first = FitCascades(params);
    REQUIRE(first.count == 1);
    const float texel = first.cascades[0].worldUnitsPerTexel;
    REQUIRE(texel > 0.f);

    const glm::vec3 reference = glm::vec3(rotation * glm::vec4(first.cascades[0].center, 1.f));

    for (std::int32_t step = 1; step <= 40; ++step)
    {
        const float distance = static_cast<float>(step) * texel * 0.13f;
        params.cameraView = View(glm::vec3(distance, 0.f, 0.f), glm::vec3(0.f, 0.f, -1.f));

        const CascadeFit fit = FitCascades(params);
        REQUIRE(fit.count == 1);

        // The radius is the camera-independent half of the invariant.
        CHECK(fit.cascades[0].worldUnitsPerTexel == doctest::Approx(texel));

        const glm::vec3 moved = glm::vec3(rotation * glm::vec4(fit.cascades[0].center, 1.f));
        const float dx = (moved.x - reference.x) / texel;
        const float dy = (moved.y - reference.y) / texel;
        CHECK(std::abs(std::remainder(dx, 1.f)) < 1e-2f);
        CHECK(std::abs(std::remainder(dy, 1.f)) < 1e-2f);
    }
}

TEST_CASE("A cascade's ortho extent holds still while the camera rotates")
{
    // The swim test, and the reason the slice is bounded by a sphere. Spin the
    // camera on the spot: the world a texel covers must not change, so neither
    // may the radius or the extent of the box the matrix projects.
    CascadeFitParams params = DefaultParams();
    params.settings.cascadeCount = 4;
    params.settings.maxDistance = 60.f;

    const CascadeFit reference = FitCascades(params);
    REQUIRE(reference.count == params.settings.cascadeCount);

    for (std::int32_t degrees = 0; degrees < 360; degrees += 17)
    {
        const float radians = glm::radians(static_cast<float>(degrees));
        params.cameraView = View(glm::vec3(0.f), glm::vec3(std::sin(radians), 0.f, -std::cos(radians)));

        const CascadeFit fit = FitCascades(params);
        REQUIRE(fit.count == reference.count);
        for (std::uint32_t i = 0; i < fit.count; ++i)
        {
            CHECK(fit.cascades[i].radius == doctest::Approx(reference.cascades[i].radius));
            CHECK(fit.cascades[i].worldUnitsPerTexel == doctest::Approx(reference.cascades[i].worldUnitsPerTexel));
        }
    }
}

TEST_CASE("Each cascade's matrix covers its own slice")
{
    // The fit is only worth anything if the matrix it produces actually
    // contains the geometry the cascade is responsible for. Sample the corners
    // of every slice and require them inside that cascade's clip volume.
    CascadeFitParams params = DefaultParams();
    params.cameraView = View(glm::vec3(2.f, 3.f, 1.f), glm::vec3(0.3f, -0.15f, -1.f));
    params.settings.maxDistance = 80.f;

    const CascadeFit fit = FitCascades(params);
    REQUIRE(fit.count == params.settings.cascadeCount);

    const glm::mat4 inverseView = glm::inverse(params.cameraView);
    for (std::uint32_t i = 0; i < fit.count; ++i)
    {
        const ShadowCascade &cascade = fit.cascades[i];
        for (const float depth : {cascade.splitNearView, cascade.splitFarView})
        {
            const float halfHeight = depth * kTanHalfFov;
            const float halfWidth = halfHeight * kAspect;
            for (const float sx : {-1.f, 1.f})
            {
                for (const float sy : {-1.f, 1.f})
                {
                    const glm::vec3 corner =
                        glm::vec3(inverseView * glm::vec4(sx * halfWidth, sy * halfHeight, -depth, 1.f));
                    const glm::vec3 clip = ToClip(cascade, corner);
                    CHECK(clip.x >= -1.001f);
                    CHECK(clip.x <= 1.001f);
                    CHECK(clip.y >= -1.001f);
                    CHECK(clip.y <= 1.001f);
                    // Zero-to-one depth (the Vulkan convention GLM is configured for).
                    CHECK(clip.z >= -0.001f);
                    CHECK(clip.z <= 1.001f);
                }
            }
        }
    }

    // Consecutive cascades meet without a gap, or geometry between them is
    // shadowed by neither.
    for (std::uint32_t i = 1; i < fit.count; ++i)
    {
        CHECK(fit.cascades[i].splitNearView == doctest::Approx(fit.cascades[i - 1].splitFarView));
    }
}

TEST_CASE("A caster upstream of the slice is inside the cascade's depth range")
{
    // Geometry behind the camera still shadows what is in front of it. The near
    // plane has to reach back to it, because the depth clip that would
    // otherwise clamp it is not something nvrhi can be relied on to disable.
    CascadeFitParams params = DefaultParams();
    params.settings.cascadeCount = 1;
    params.settings.maxDistance = 40.f;

    const glm::vec3 lightDirection = glm::normalize(params.lightDirection);

    const CascadeFit tight = FitCascades(params);
    REQUIRE(tight.count == 1);

    // A caster 30 m upstream of where the slice's own near plane sits.
    const float sliceNear = glm::dot(tight.cascades[0].center, lightDirection) - tight.cascades[0].radius;
    const float casterNear = sliceNear - 30.f;
    params.casterNearAlongLight = casterNear;

    const CascadeFit extended = FitCascades(params);
    REQUIRE(extended.count == 1);

    // The slice itself is unmoved — pulling the near plane back must not
    // change which world the cascade covers, only how deep it reaches.
    CHECK(extended.cascades[0].radius == doctest::Approx(tight.cascades[0].radius));
    CHECK(glm::length(extended.cascades[0].center - tight.cascades[0].center) < 1e-3f);
    CHECK(extended.cascades[0].depthRange == doctest::Approx(tight.cascades[0].depthRange + 30.f));

    // A point at the caster's depth, over the middle of the slice, now projects
    // inside the clip volume instead of in front of it.
    const glm::vec3 caster =
        tight.cascades[0].center - lightDirection * (glm::dot(tight.cascades[0].center, lightDirection) - casterNear);
    CHECK(ToClip(tight.cascades[0], caster).z < 0.f);
    CHECK(ToClip(extended.cascades[0], caster).z >= -0.001f);
    CHECK(ToClip(extended.cascades[0], caster).z <= 1.001f);
}

TEST_CASE("Biases scale with the cascade they are applied in")
{
    // One setting, quoted in texels, has to mean the same thing in a cascade
    // with centimetre texels and one with metre texels — otherwise the near
    // cascade peter-pans or the far one acnes, whichever the number was tuned for.
    CascadeFitParams params = DefaultParams();
    params.settings.maxDistance = 100.f;
    params.settings.depthBiasTexels = 2.f;
    params.settings.normalOffsetTexels = 3.f;

    const CascadeFit fit = FitCascades(params);
    REQUIRE(fit.count == params.settings.cascadeCount);

    // Texels grow with the cascade, which is the whole reason for cascades.
    for (std::uint32_t i = 1; i < fit.count; ++i)
    {
        CHECK(fit.cascades[i].worldUnitsPerTexel > fit.cascades[i - 1].worldUnitsPerTexel);
    }

    for (std::uint32_t i = 0; i < fit.count; ++i)
    {
        const ShadowCascade &cascade = fit.cascades[i];
        CHECK(CascadeNormalOffsetWorld(cascade, params.settings) ==
              doctest::Approx(3.f * cascade.worldUnitsPerTexel));

        // The depth bias is quoted in the same [0, 1] the shader compares in,
        // so the world distance it stands for is the texel scale times the setting.
        const float worldBias = CascadeDepthBiasNdc(cascade, params.settings) * cascade.depthRange;
        CHECK(worldBias == doctest::Approx(2.f * cascade.worldUnitsPerTexel));
    }

    // A degenerate cascade biases by nothing rather than dividing by zero.
    CHECK(CascadeDepthBiasNdc(ShadowCascade{}, params.settings) == doctest::Approx(0.f));
}

TEST_CASE("The unconditional bias is small enough not to leak on its own")
{
    // The depth bias applies to every lookup, so its whole magnitude is a gap
    // under every contact, and it grows with the cascade because it is quoted in
    // texels. At a texel and a half the outermost cascade of the defaults lied
    // by 14 cm, which is what the leak was; this pins that it stayed small.
    //
    // The normal offset is deliberately not held to the same bound. It is scaled
    // in the shader by how much of the receiver-plane fit had to be withdrawn,
    // so it is absent exactly where a large offset used to leak and present only
    // on surfaces the fit cannot serve. That gating is not visible from here —
    // what this file can still say is that the bias which is *not* gated stayed
    // where it belongs.
    CascadeFitParams params = DefaultParams();
    const CascadeFit fit = FitCascades(params);
    REQUIRE(fit.count > 0);

    const ShadowCascade &outermost = fit.cascades[fit.count - 1];
    CHECK(CascadeDepthBiasNdc(outermost, params.settings) * outermost.depthRange < 0.06f);
}

TEST_CASE("The gradient limit is the slope a plane could have, in the map's own units")
{
    CascadeFitParams params = DefaultParams();
    const CascadeFit fit = FitCascades(params);
    REQUIRE(fit.count > 0);

    for (std::uint32_t i = 0; i < fit.count; ++i)
    {
        const ShadowCascade &cascade = fit.cascades[i];
        const float limit = CascadeReceiverGradientLimit(cascade, params.settings);

        // A receiver at exactly the steepest trusted slope reads right at the
        // limit: crossing the whole map is 2r of world per unit of UV, and
        // depthRange of world per unit of the depth the shader compares in.
        const float atLimit = kMaxReceiverPlaneSlope * (2.f * cascade.radius) / cascade.depthRange;
        CHECK(limit == doctest::Approx(atLimit));

        // A plane at 45 degrees is an ordinary receiver and must survive the
        // test, or the correction would be declined on a floor lit from above.
        const float atFortyFive = (2.f * cascade.radius) / cascade.depthRange;
        CHECK(atFortyFive < limit);
    }

    // A degenerate cascade trusts nothing rather than dividing by zero.
    CHECK(CascadeReceiverGradientLimit(ShadowCascade{}, params.settings) == doctest::Approx(0.f));
}

TEST_CASE("The texel size is the map's own, not the filter's reference step")
{
    // The two are equal only up to the reference resolution. Past it the tap
    // step stops shrinking, deliberately, while a texel keeps going — and it is
    // the texel the comparison sampler snaps to, so the half-texel term in the
    // shader has to read this one.
    SunShadowSettings settings;
    settings.resolution = kFilterReferenceResolution;
    CHECK(ShadowTexelSizeUv(settings) == doctest::Approx(FilterTapStepUv(settings)));

    settings.resolution = kFilterReferenceResolution * 2u;
    CHECK(ShadowTexelSizeUv(settings) == doctest::Approx(FilterTapStepUv(settings) * 0.5f));
    CHECK(ShadowTexelSizeUv(settings) == doctest::Approx(1.f / static_cast<float>(settings.resolution)));
}

TEST_CASE("A bigger shadow map does not narrow the blur it is filtered with")
{
    // Kernel offsets are in texels, so a radius measured in the cascade's own
    // texels halves in world terms every time the resolution doubles — and a
    // tier that raises the resolution ships a *harder* shadow than the tier
    // below it. The step is quoted against a reference size to stop that.
    SunShadowSettings coarse;
    coarse.resolution = kFilterReferenceResolution / 2;
    SunShadowSettings reference;
    reference.resolution = kFilterReferenceResolution;
    SunShadowSettings fine;
    fine.resolution = kFilterReferenceResolution * 2;

    // At or below the reference there is nothing between one texel and the next
    // to sample, so the step is exactly a texel.
    CHECK(FilterTapStepUv(coarse) == doctest::Approx(1.0f / static_cast<float>(coarse.resolution)));
    CHECK(FilterTapStepUv(reference) == doctest::Approx(1.0f / static_cast<float>(reference.resolution)));

    // Above it the step holds, so the footprint stays where it was.
    CHECK(FilterTapStepUv(fine) == doctest::Approx(FilterTapStepUv(reference)));
    CHECK(FilterTapStepUv(fine) > 1.0f / static_cast<float>(fine.resolution));
}

TEST_CASE("Raising quality never hardens the shadow")
{
    // The defect this exists to catch: Ultra came out sharper-edged than High
    // at every cascade, because doubling its resolution halved the world size
    // of the texels its filter radius was quoted in. Both axes a quality
    // setting can move are checked at a fixed cascade layout, since a tier that
    // also changes the layout changes the extents and with them the penumbra —
    // which is the fit doing its job, not the filter failing to.
    CascadeFitParams params = DefaultParams();
    params.settings.cascadeCount = 4;
    params.settings.maxDistance = 80.f;

    // Resolution: the kernel's reach is what the quoted radius controls, and it
    // must not move when the map grows — that is the whole of the fix. What does
    // still shrink is the hardware's own bilinear footprint, half a texel of a
    // map that now has twice as many, and that is the map resolving better
    // rather than the filter being quoted wrongly. So the kernel is asserted
    // exactly and the total is left alone.
    //
    // From the reference size upward only. Below it the tap step is one texel
    // because there is no finer grid to walk, so the reach tracks the resolution
    // and shrinking there is the map running out of texels.
    SunShadowSettings atReference = params.settings;
    atReference.resolution = kFilterReferenceResolution;
    SunShadowSettings aboveReference = params.settings;
    aboveReference.resolution = kFilterReferenceResolution * 2;

    const float reachAt = FilterRadiusTaps(atReference.filter) * FilterTapStepUv(atReference);
    const float reachAbove = FilterRadiusTaps(aboveReference.filter) * FilterTapStepUv(aboveReference);
    CHECK(reachAt > 0.f);
    CHECK(reachAbove == doctest::Approx(reachAt));

    // Filter: each kernel reaches further than the one below it, and the
    // one-tap filter is still soft to the hardware's own bilinear rather than
    // hard-edged.
    params.settings.resolution = 2048;
    const CascadeFit fit = FitCascades(params);
    REQUIRE(fit.count == params.settings.cascadeCount);

    float widest = 0.f;
    for (const ShadowFilter filter : {ShadowFilter::Point, ShadowFilter::Pcf3x3, ShadowFilter::Pcf5x5,
                                      ShadowFilter::Vogel})
    {
        params.settings.filter = filter;
        const float penumbra = CascadePenumbraWorld(fit.cascades[0], params.settings);
        CHECK(penumbra > widest);
        widest = penumbra;
    }

    // And across every cascade, not only the nearest, at the pair that was
    // inverted: same layout, Ultra's resolution and filter against High's.
    SunShadowSettings high = params.settings;
    high.resolution = 2048;
    high.filter = ShadowFilter::Pcf5x5;
    SunShadowSettings ultra = params.settings;
    ultra.resolution = 4096;
    ultra.filter = ShadowFilter::Vogel;

    CascadeFitParams ultraParams = DefaultParams();
    ultraParams.settings = ultra;
    const CascadeFit ultraFit = FitCascades(ultraParams);
    CascadeFitParams highParams = DefaultParams();
    highParams.settings = high;
    const CascadeFit highFit = FitCascades(highParams);
    REQUIRE(highFit.count == ultraFit.count);
    for (std::uint32_t i = 0; i < highFit.count; ++i)
    {
        CHECK(CascadePenumbraWorld(ultraFit.cascades[i], ultra) >
              CascadePenumbraWorld(highFit.cascades[i], high));
        CHECK(ultraFit.cascades[i].worldUnitsPerTexel < highFit.cascades[i].worldUnitsPerTexel);
    }
}

namespace
{
// 1080p, the gate resolution. A seam's visibility is a screen-space fact, so
// the figures below are only true against a stated camera.
constexpr float kGateScreenHeight = 1080.f;

/// The coarsest a cascade's texels get at the seam it takes over at, in screen
/// pixels — the incoming, coarser side, which is what decides whether the step
/// shows at all.
float WorstSeamPixels(const CascadeFit &fit)
{
    float worst = 0.f;
    for (std::uint32_t i = 1; i < fit.count; ++i)
    {
        worst = std::max(worst, CascadeTexelScreenPixels(fit.cascades[i], fit.cascades[i - 1].splitFarView,
                                                         kGateScreenHeight, kTanHalfFov));
    }
    return worst;
}
} // namespace

TEST_CASE("Adding cascades makes the seams smaller")
{
    // The property the split distribution exists for. Spacing the splits from
    // the camera's near plane leaves the near ones almost uniformly placed, so
    // their ratios do not shrink as cascades are added and the worst seam sits
    // where it is however many are paid for — eight cascades measured within a
    // few percent of four. Measured from kSplitDistributionNear the ratio is
    // constant and falls with the count, which is what makes the knob worth
    // having.
    CascadeFitParams params = DefaultParams();
    params.settings.resolution = 2048;
    params.settings.maxDistance = 80.f;

    params.settings.cascadeCount = 4;
    const float four = WorstSeamPixels(FitCascades(params));
    params.settings.cascadeCount = 8;
    const float eight = WorstSeamPixels(FitCascades(params));

    CHECK(four > 0.f);
    CHECK(eight < four * 0.75f);

    // And monotonically in between, so every cascade paid for buys something.
    float previous = four;
    for (std::uint32_t count = 5; count <= 8; ++count)
    {
        params.settings.cascadeCount = count;
        const float seam = WorstSeamPixels(FitCascades(params));
        CHECK(seam < previous);
        previous = seam;
    }
}

TEST_CASE("Cascade zero still covers the camera's near plane")
{
    // Moving where the splits are distributed must not leave close geometry
    // with no cascade to be shadowed by — the distribution starts at
    // kSplitDistributionNear, the coverage does not.
    CascadeFitParams params = DefaultParams();
    params.nearZ = 0.05f;
    REQUIRE(params.nearZ < kSplitDistributionNear);

    const CascadeFit fit = FitCascades(params);
    REQUIRE(fit.count > 0);
    CHECK(fit.cascades[0].splitNearView == doctest::Approx(params.nearZ));

    // A point just past the near plane, dead ahead, projects inside cascade
    // zero's clip volume rather than in front of it.
    const glm::vec3 justInFront =
        glm::vec3(glm::inverse(params.cameraView) * glm::vec4(0.f, 0.f, -params.nearZ * 1.5f, 1.f));
    const glm::vec3 clip = ToClip(fit.cascades[0], justInFront);
    CHECK(std::abs(clip.x) <= 1.001f);
    CHECK(std::abs(clip.y) <= 1.001f);
    CHECK(clip.z >= -0.001f);
    CHECK(clip.z <= 1.001f);
}

TEST_CASE("Every tier's seams stay inside what a blend band can hide")
{
    // No tier may ship a seam so coarse that no amount of blending reads as a
    // gradient. Ultra is held to the stricter bar it exists for: a seam at the
    // limit of what the gate resolution can resolve, around a pixel, rather
    // than one that is merely small.
    CascadeFitParams params = DefaultParams();
    for (std::uint32_t i = 0; i < kShadowTierCount; ++i)
    {
        const auto tier = static_cast<ShadowTier>(i);
        params.settings = TierSettings(tier).sun;
        const float seam = WorstSeamPixels(FitCascades(params));
        CHECK(seam <= (tier == ShadowTier::Low ? 6.5f : 3.0f));
        if (tier == ShadowTier::Ultra)
        {
            CHECK(seam <= 1.15f);
        }
    }

    // The band crossing it is a real distance, not a token one — and the
    // ceiling reaches a whole cascade, which is the cheap way to trade filter
    // cost for smoothness instead of buying another cascade.
    CHECK(SunShadowSettings{}.cascadeBlend >= 0.25f);
    CHECK(kMaxCascadeBlend >= 1.0f);
}

TEST_CASE("A vertical sun still produces a usable basis")
{
    // Straight down is both the most likely sun angle and the one where the
    // conventional up axis is parallel to the light.
    for (const glm::vec3 direction : {glm::vec3(0.f, -1.f, 0.f), glm::vec3(0.f, 1.f, 0.f)})
    {
        const glm::mat4 rotation = LightRotation(direction);
        for (std::int32_t row = 0; row < 4; ++row)
        {
            for (std::int32_t column = 0; column < 4; ++column)
            {
                CHECK(std::isfinite(rotation[column][row]));
            }
        }
        // The light travels along light space's -Z, which is what lets an
        // ordinary ortho projection be built against it.
        const glm::vec3 mapped = glm::vec3(rotation * glm::vec4(glm::normalize(direction), 0.f));
        CHECK(mapped.z == doctest::Approx(-1.f));
    }

    // A zero direction comes straight from a hand-edited level file.
    CascadeFitParams params = DefaultParams();
    params.lightDirection = glm::vec3(0.f);
    const CascadeFit fit = FitCascades(params);
    REQUIRE(fit.count == params.settings.cascadeCount);
    for (std::uint32_t i = 0; i < fit.count; ++i)
    {
        CHECK(std::isfinite(fit.cascades[i].radius));
        CHECK(std::isfinite(fit.cascades[i].depthRange));
        CHECK(std::isfinite(fit.cascades[i].viewProjection[3][3]));
    }
}

TEST_CASE("A degenerate camera range fits nothing rather than something wrong")
{
    CascadeFitParams params = DefaultParams();

    params.settings.enabled = false;
    CHECK(FitCascades(params).count == 0);

    params.settings.enabled = true;
    params.nearZ = 50.f;
    params.farZ = 50.f;
    CHECK(FitCascades(params).count == 0);

    // A shadow distance inside the near plane leaves nothing to cover.
    params.nearZ = 30.f;
    params.farZ = 200.f;
    params.settings.maxDistance = kMinShadowDistance;
    CHECK(FitCascades(params).count == 0);
}
