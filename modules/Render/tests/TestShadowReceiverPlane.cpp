/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/ShadowAtlas.hpp>
#include <Assisi/Render/ShadowCascades.hpp>
#include <Assisi/Render/ShadowSettings.hpp>
#include <Assisi/Render/ShadowView.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

using namespace Assisi::Render;

namespace
{
/// The atlas the local tests cut their tile from.
constexpr std::uint32_t kAtlas = 4096;

/// A standing player's eye height, and the camera's vertical field of view and
/// aspect, for the cascades the sun tests fit.
constexpr float kEyeHeight = 1.7f;
constexpr float kHalfFovYDegrees = 30.f;
constexpr float kAspect = 16.f / 9.f;

/// A camera at eye height looking at @p target, fitted with cascades for a sun
/// travelling along @p lightDirection.
CascadeFitParams SunParams(const glm::vec3 &target, const glm::vec3 &lightDirection)
{
    CascadeFitParams params;
    params.cameraView = glm::lookAt(glm::vec3(0.f, kEyeHeight, 0.f), target, glm::vec3(0.f, 1.f, 0.f));
    params.tanHalfFovY = std::tan(glm::radians(kHalfFovYDegrees));
    params.aspectRatio = kAspect;
    params.lightDirection = lightDirection;
    return params;
}

/// A sun @p elevationDegrees above the horizon, travelling toward -Z.
glm::vec3 SunDirection(float elevationDegrees)
{
    const float elevation = glm::radians(elevationDegrees);
    return glm::vec3(0.f, -std::sin(elevation), -std::cos(elevation));
}

/// Where @p world lands in @p view's target: UV in xy, comparison depth in z.
/// The same as mesh.frag's ShadowCoord and LocalShadowCoord.
glm::vec3 MapCoord(const ShadowView &view, const glm::vec3 &world)
{
    const glm::vec4 clip = view.viewProjection * glm::vec4(world, 1.f);
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    const glm::vec2 viewUv = glm::vec2(ndc) * glm::vec2(0.5f, -0.5f) + 0.5f;
    const glm::vec4 scaleOffset = ShadowViewUvScaleOffset(view);
    return glm::vec3(viewUv * glm::vec2(scaleOffset) + glm::vec2(scaleOffset.z, scaleOffset.w), ndc.z);
}

/// A flat surface, cut off where it meets another: it exists only on the side
/// of `point` that `keptAlong` points to. A zero `keptAlong` keeps all of it.
struct Surface
{
    glm::vec3 point{0.f};
    glm::vec3 normal{0.f};
    glm::vec3 keptAlong{0.f};
};

/// The depth the map stores at @p uv: the nearest of @p surfaces the light's ray
/// through @p uv meets, or the far plane where it meets none.
float StoredDepthAt(const ShadowView &view, std::span<const Surface> surfaces, const glm::vec2 &uv)
{
    const glm::vec4 scaleOffset = ShadowViewUvScaleOffset(view);
    const glm::vec2 viewUv = (uv - glm::vec2(scaleOffset.z, scaleOffset.w)) / glm::vec2(scaleOffset);
    const glm::vec2 ndc = (viewUv - 0.5f) / glm::vec2(0.5f, -0.5f);

    const glm::mat4 unproject = glm::inverse(view.viewProjection);
    const glm::vec4 nearClip = unproject * glm::vec4(ndc, 0.f, 1.f);
    const glm::vec4 farClip = unproject * glm::vec4(ndc, 1.f, 1.f);
    const glm::vec3 nearWorld = glm::vec3(nearClip) / nearClip.w;
    const glm::vec3 farWorld = glm::vec3(farClip) / farClip.w;

    float nearest = 1.f;
    for (const Surface &surface : surfaces)
    {
        const float along =
            glm::dot(surface.normal, surface.point - nearWorld) / glm::dot(surface.normal, farWorld - nearWorld);
        const glm::vec3 hit = nearWorld + along * (farWorld - nearWorld);
        if (along >= 0.f && along <= 1.f && glm::dot(surface.keptAlong, hit - surface.point) >= 0.f)
        {
            nearest = std::min(nearest, MapCoord(view, hit).z);
        }
    }
    return nearest;
}

/// One lookup, biased the way mesh.frag biases it.
struct Lookup
{
    glm::vec2 slope{0.f};
    glm::vec2 uv{0.f};
    float reference = 0.f;
};

/// The lookup mesh.frag makes for a receiver at @p point, @p normal, lit at
/// @p incidenceRadians off its normal. @p distance is the receiver's distance
/// along a local light's axis, which scales a tile's biases; 1 for a cascade,
/// whose biases are already in world units.
Lookup BiasedLookup(const ShadowView &view, const glm::vec3 &point, const glm::vec3 &normal, float incidenceRadians,
                    float distance)
{
    const glm::vec3 biased = point + normal * (view.normalOffset * distance * std::sin(incidenceRadians));
    const glm::vec3 coord = MapCoord(view, biased);
    // A texel of a cascade's depth is a texel of its UV; a tile's is measured at
    // the receiver, as mesh.frag measures it.
    const float depthPerUv =
        view.orthographic ? 1.f : view.pcssTexelDepthTimesDistance / distance / view.filterTapStepUv;
    return Lookup{.slope = ReceiverPlaneSlope(view, biased, normal, depthPerUv),
                  .uv = glm::vec2(coord),
                  .reference = coord.z - view.depthBias / distance};
}

/// Checks no texel @p filter reads for @p lookup holds a blocker, in a scene
/// that is only @p surfaces, all lit: whatever the map stores at each texel must
/// lie behind the depth the lookup compares it against. Every tap reads the four
/// texels around it, at the widest step the filter takes.
void CheckKernelSeesNoBlocker(const ShadowView &view, std::span<const Surface> surfaces, const Lookup &lookup,
                              ShadowFilter filter)
{
    const int radius = static_cast<int>(FilterRadiusTaps(filter));
    const float texelUv = view.filterTapStepUv;
    for (int y = -radius; y <= radius; ++y)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            const glm::vec2 tapUv = lookup.uv + glm::vec2(static_cast<float>(x), static_cast<float>(y)) * texelUv;
            const glm::vec2 first = glm::floor(tapUv / texelUv - 0.5f);
            for (int j = 0; j <= 1; ++j)
            {
                for (int i = 0; i <= 1; ++i)
                {
                    const glm::vec2 centre =
                        (first + glm::vec2(static_cast<float>(i), static_cast<float>(j)) + 0.5f) * texelUv;
                    const float compared = ShadowComparedDepth(lookup.reference, centre - lookup.uv, lookup.slope);
                    CAPTURE(x);
                    CAPTURE(y);
                    CAPTURE(i);
                    CAPTURE(j);
                    CHECK(compared < StoredDepthAt(view, surfaces, centre));
                }
            }
        }
    }
}
} // namespace

TEST_CASE("Ground under a low sun reads none of its own texels as a blocker")
{
    // A sun twenty degrees above flat ground meets it seventy degrees off its
    // normal. Compared against one depth, the default kernel reads the ground on
    // the sun's side as a blocker there, in stripes across the sun's direction.
    constexpr float kSunElevationDegrees = 20.f;
    // Ground in front of the camera, inside the first cascade.
    const glm::vec3 point(0.f, 0.f, -2.f);
    const glm::vec3 up(0.f, 1.f, 0.f);

    CascadeFitParams params = SunParams(point, SunDirection(kSunElevationDegrees));
    const float incidence = glm::radians(90.f - kSunElevationDegrees);

    for (const ShadowTier tier : {ShadowTier::Medium, ShadowTier::High})
    {
        CAPTURE(tier);
        params.settings = Sanitized(TierSettings(tier).sun);
        const CascadeFit fit = FitCascades(params);
        REQUIRE(fit.count > 0);

        const ShadowView view = CascadeShadowView(fit.cascades[0], 0, params.settings);
        const std::array<Surface, 1> ground{Surface{.point = point, .normal = up}};
        CheckKernelSeesNoBlocker(view, ground, BiasedLookup(view, point, up, incidence, 1.f), params.settings.filter);
    }
}

TEST_CASE("A wall a spot light grazes reads none of its own texels as a blocker")
{
    // A spot hung half a metre out from a wall, shining down it: two metres down
    // the wall meets the light about seventy-five degrees off its normal.
    constexpr float kOffWall = 0.5f;
    constexpr float kDown = 2.f;
    constexpr float kRange = 10.f;
    constexpr float kOuterAngleDegrees = 30.f;
    constexpr std::uint32_t kTile = 512;
    const glm::vec3 lightPosition(kOffWall, 0.f, 0.f);
    const glm::vec3 point(0.f, -kDown, 0.f);
    const glm::vec3 wallNormal(1.f, 0.f, 0.f);

    const LocalShadowLightPose pose{.position = lightPosition,
                                    .direction = glm::vec3(0.f, -1.f, 0.f),
                                    .range = kRange,
                                    .outerAngleDegrees = kOuterAngleDegrees};
    const ShadowAtlasTile tile{.rect = ShadowViewRect{.x = 0, .y = 0, .width = kTile, .height = kTile},
                               .atlasResolution = kAtlas};
    const glm::vec3 toLight = glm::normalize(lightPosition - point);
    const float incidence = std::acos(glm::dot(toLight, wallNormal));
    // The spot looks straight down, so the distance along its axis is the drop.
    const float axisDepth = kDown;

    for (const ShadowTier tier : {ShadowTier::Medium, ShadowTier::High})
    {
        CAPTURE(tier);
        const LocalShadowSettings settings = Sanitized(TierSettings(tier).local);
        const ShadowView view = SpotShadowView(pose, tile, settings);
        const std::array<Surface, 1> wall{Surface{.point = point, .normal = wallNormal}};
        CheckKernelSeesNoBlocker(view, wall, BiasedLookup(view, point, wallNormal, incidence, axisDepth),
                                 settings.filter);
    }
}

TEST_CASE("The foot of a wall a spot light grazes does not read the floor as a blocker")
{
    // The same spot and wall, now standing on a floor, looked up two centimetres
    // above the floor. The wall's plane carried down past its foot runs behind
    // the floor, which the light meets first there, so a lookup that follows the
    // plane that far reads the floor as the wall's blocker: a band of shadow
    // along the foot of every wall a light shines down.
    constexpr float kOffWall = 0.5f;
    constexpr float kDown = 2.f;
    constexpr float kAboveFloor = 0.02f;
    constexpr float kRange = 10.f;
    constexpr float kOuterAngleDegrees = 30.f;
    constexpr std::uint32_t kTile = 512;
    const glm::vec3 lightPosition(kOffWall, 0.f, 0.f);
    const glm::vec3 foot(0.f, -kDown, 0.f);
    const glm::vec3 point = foot + glm::vec3(0.f, kAboveFloor, 0.f);
    const glm::vec3 wallNormal(1.f, 0.f, 0.f);
    const glm::vec3 up(0.f, 1.f, 0.f);
    const std::array<Surface, 2> corner{
        Surface{.point = foot, .normal = wallNormal, .keptAlong = up},
        Surface{.point = foot, .normal = up, .keptAlong = wallNormal},
    };

    const LocalShadowLightPose pose{.position = lightPosition,
                                    .direction = glm::vec3(0.f, -1.f, 0.f),
                                    .range = kRange,
                                    .outerAngleDegrees = kOuterAngleDegrees};
    const ShadowAtlasTile tile{.rect = ShadowViewRect{.x = 0, .y = 0, .width = kTile, .height = kTile},
                               .atlasResolution = kAtlas};
    const float incidence = std::acos(glm::dot(glm::normalize(lightPosition - point), wallNormal));
    const float axisDepth = kDown - kAboveFloor;

    for (const ShadowTier tier : {ShadowTier::Medium, ShadowTier::High})
    {
        CAPTURE(tier);
        const LocalShadowSettings settings = Sanitized(TierSettings(tier).local);
        const ShadowView view = SpotShadowView(pose, tile, settings);
        CheckKernelSeesNoBlocker(view, corner, BiasedLookup(view, point, wallNormal, incidence, axisDepth),
                                 settings.filter);
    }
}

TEST_CASE("A receiver's plane is held at the steepest slope a lookup follows")
{
    // Ground the sun barely clears: its plane deepens faster than the limit, so
    // it is held there, in texels of depth per texel of the map.
    constexpr float kSunElevationDegrees = 0.01f;
    constexpr float kDepthPerUv = 1.f;
    const CascadeFitParams params = SunParams(glm::vec3(0.f, 0.f, -1.f), SunDirection(kSunElevationDegrees));
    const CascadeFit fit = FitCascades(params);
    REQUIRE(fit.count > 0);
    const ShadowView view = CascadeShadowView(fit.cascades[0], 0, params.settings);

    const glm::vec2 slope = ReceiverPlaneSlope(view, glm::vec3(0.f), glm::vec3(0.f, 1.f, 0.f), kDepthPerUv);
    CHECK(glm::length(slope) == doctest::Approx(kMaxReceiverSlopeTexels * kDepthPerUv));

    // A surface edge-on to the map has no plane to follow.
    const glm::vec3 alongLight = params.lightDirection;
    const glm::vec3 edgeOnNormal = glm::normalize(glm::cross(alongLight, glm::vec3(1.f, 0.f, 0.f)));
    const glm::vec2 edgeOn = ReceiverPlaneSlope(view, glm::vec3(0.f), edgeOnNormal, kDepthPerUv);
    CHECK(glm::length(edgeOn) <= kMaxReceiverSlopeTexels * kDepthPerUv);
}
