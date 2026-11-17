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
#include <functional>
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

/// The surface a lookup is made from, as mesh.frag's ShadowSurface: where it
/// starts, the normal its offset runs along, and the normal of the plane its
/// texels are compared against.
struct LookupSurface
{
    glm::vec3 position{0.f};
    glm::vec3 normal{0.f};
    glm::vec3 plane{0.f};
    float incidenceRadians = 0.f; ///< The light's angle off @ref normal.
};

/// The lookup mesh.frag makes from @p surface. @p distance is the receiver's
/// distance along a local light's axis, which scales a tile's biases; 1 for a
/// cascade, whose biases are already in world units.
Lookup BiasedLookup(const ShadowView &view, const LookupSurface &surface, float distance)
{
    const glm::vec3 biased =
        surface.position + surface.normal * (view.normalOffset * distance * std::sin(surface.incidenceRadians));
    const glm::vec3 coord = MapCoord(view, biased);
    // A texel of a cascade's depth is a texel of its UV; a tile's is measured at
    // the receiver, as mesh.frag measures it.
    const float depthPerUv =
        view.orthographic ? 1.f : view.pcssTexelDepthTimesDistance / distance / view.filterTapStepUv;
    return Lookup{.slope = ReceiverPlaneSlope(view, biased, surface.plane, depthPerUv),
                  .uv = glm::vec2(coord),
                  .reference = coord.z - view.depthBias / distance};
}

/// The lookup for a flat receiver at @p point facing @p normal, whose plane is
/// its own.
Lookup BiasedLookup(const ShadowView &view, const glm::vec3 &point, const glm::vec3 &normal, float incidenceRadians,
                    float distance)
{
    return BiasedLookup(
        view, LookupSurface{.position = point, .normal = normal, .plane = normal, .incidenceRadians = incidenceRadians},
        distance);
}

/// What a map stores at a UV.
using StoredDepthFn = std::function<float (const glm::vec2 &)>;

/// Checks no texel @p filter reads for @p lookup holds a blocker, in a scene
/// whose map stores @p storedDepthAt, all lit: whatever the map stores at each
/// texel must lie behind the depth the lookup compares it against. Every tap
/// reads the four texels around it, at the widest step the filter takes.
void CheckKernelSeesNoBlocker(const ShadowView &view, const StoredDepthFn &storedDepthAt, const Lookup &lookup,
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
                    CHECK(compared < storedDepthAt(centre));
                }
            }
        }
    }
}

/// The same check in a scene that is only @p surfaces.
void CheckKernelSeesNoBlocker(const ShadowView &view, std::span<const Surface> surfaces, const Lookup &lookup,
                              ShadowFilter filter)
{
    const StoredDepthFn stored = [&view, surfaces](const glm::vec2 &uv) { return StoredDepthAt(view, surfaces, uv); };
    CheckKernelSeesNoBlocker(view, stored, lookup, filter);
}

/// A smooth-shaded cylinder of so many flat sides, lying along the X axis: what
/// the map holds of it is its flat sides, while it is lit as the circle its
/// vertex normals describe.
struct FacetedCylinder
{
    glm::vec3 centre{0.f};
    float radius = 0.f;
    std::uint32_t sides = 0;

    /// The vertex normal of the corner between side @p corner - 1 and side
    /// @p corner, in the cross-section through the centre.
    [[nodiscard]] glm::vec3 Normal(std::uint32_t corner) const
    {
        const float angle = glm::two_pi<float>() * static_cast<float>(corner) / static_cast<float>(sides);
        return glm::vec3(0.f, std::cos(angle), std::sin(angle));
    }
    /// That corner's position.
    [[nodiscard]] glm::vec3 Corner(std::uint32_t corner) const { return centre + radius * Normal(corner); }
};

/// The depth the map stores at @p uv for @p cylinder: where the light's ray
/// through @p uv enters it, or the far plane where it misses. The cylinder is
/// the space inside every side's plane, so the ray enters at the last of those
/// planes it crosses inward, if that comes before the first it crosses outward.
float CylinderDepthAt(const ShadowView &view, const FacetedCylinder &cylinder, const glm::vec2 &uv)
{
    const glm::vec4 scaleOffset = ShadowViewUvScaleOffset(view);
    const glm::vec2 viewUv = (uv - glm::vec2(scaleOffset.z, scaleOffset.w)) / glm::vec2(scaleOffset);
    const glm::vec2 ndc = (viewUv - 0.5f) / glm::vec2(0.5f, -0.5f);
    const glm::mat4 unproject = glm::inverse(view.viewProjection);
    const glm::vec4 nearClip = unproject * glm::vec4(ndc, 0.f, 1.f);
    const glm::vec4 farClip = unproject * glm::vec4(ndc, 1.f, 1.f);
    const glm::vec3 nearWorld = glm::vec3(nearClip) / nearClip.w;
    const glm::vec3 ray = glm::vec3(farClip) / farClip.w - nearWorld;

    float enter = 0.f;
    float leave = 1.f;
    for (std::uint32_t side = 0; side < cylinder.sides; ++side)
    {
        const glm::vec3 outward = glm::normalize(cylinder.Normal(side) + cylinder.Normal(side + 1));
        const float approach = glm::dot(outward, ray);
        const float along = glm::dot(outward, cylinder.Corner(side) - nearWorld) / approach;
        if (approach < 0.f)
        {
            enter = std::max(enter, along);
        }
        else if (approach > 0.f)
        {
            leave = std::min(leave, along);
        }
    }
    return enter <= leave ? MapCoord(view, nearWorld + enter * ray).z : 1.f;
}

/// The lookup mesh.frag makes toward @p toLight at @p t of the way across side
/// @p side of @p cylinder, lifted onto the curve as far as the terminator gate
/// lets it.
LookupSurface CylinderLookupSurface(const FacetedCylinder &cylinder, std::uint32_t side, float t,
                                    const glm::vec3 &toLight)
{
    // Either of the side's two triangles: a point on the cross-section lies on
    // the edge they share with the next cross-section.
    const glm::vec3 along(1.f, 0.f, 0.f);
    const ShadedTriangle triangle{
        .corners = {cylinder.Corner(side), cylinder.Corner(side + 1), cylinder.Corner(side) + along},
        .normals = {cylinder.Normal(side), cylinder.Normal(side + 1), cylinder.Normal(side)}};
    const glm::vec3 barycentric(1.f - t, t, 0.f);
    const glm::vec3 point = glm::mix(triangle.corners[0], triangle.corners[1], t);
    const glm::vec3 normal = glm::normalize(glm::mix(triangle.normals[0], triangle.normals[1], t));
    const glm::vec3 facet = glm::normalize(cylinder.Normal(side) + cylinder.Normal(side + 1));

    const glm::vec3 lift = SmoothSurfaceLift(triangle, barycentric, normal);
    const float gate = glm::dot(lift, lift) > 0.f ? TerminatorGate(normal, facet, toLight) : 0.f;
    return LookupSurface{.position = point + lift * gate,
                         .normal = normal,
                         .plane = glm::normalize(glm::mix(facet, normal, gate)),
                         .incidenceRadians = std::acos(glm::dot(normal, toLight))};
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

namespace
{
/// The smooth-shaded cylinders the terminator tests light: half a metre
/// across, lying on the ground in front of the camera, inside the first
/// cascade.
constexpr float kCylinderRadius = 0.5f;
constexpr float kCylinderDistance = 2.f;

/// How far across each side the tests look up, avoiding the corners, where the
/// lift is zero and the side's own plane is the curve's.
constexpr std::array<float, 4> kAcrossSide{0.125f, 0.375f, 0.625f, 0.875f};

FacetedCylinder CylinderOf(std::uint32_t sides)
{
    return FacetedCylinder{
        .centre = glm::vec3(0.f, kCylinderRadius, -kCylinderDistance), .radius = kCylinderRadius, .sides = sides};
}

/// The first cascade's view for a sun some degrees up, and the filter it is
/// read with.
struct SunLookup
{
    ShadowView view;
    ShadowFilter filter = ShadowFilter::Pcf3x3;
};

SunLookup SunLookupAt(float elevationDegrees, ShadowTier tier)
{
    CascadeFitParams params = SunParams(glm::vec3(0.f, 0.f, -kCylinderDistance), SunDirection(elevationDegrees));
    params.settings = Sanitized(TierSettings(tier).sun);
    const CascadeFit fit = FitCascades(params);
    REQUIRE(fit.count > 0);
    return SunLookup{.view = CascadeShadowView(fit.cascades[0], 0, params.settings), .filter = params.settings.filter};
}
} // namespace

TEST_CASE("A smooth-shaded cylinder under a low sun reads none of its own sides as a blocker")
{
    // Near the edge between light and dark, a side can face away from the sun
    // while the normal it is lit by still faces it, and the map has that side
    // behind its neighbours: a stripe of shadow per side. Lifted onto the
    // circle the normals describe, every lookup on the lit half is clear.
    for (const std::uint32_t sides : {8u, 16u, 32u})
    {
        for (const float elevation : {5.f, 10.f, 20.f})
        {
            for (const ShadowTier tier : {ShadowTier::Medium, ShadowTier::High})
            {
                CAPTURE(sides);
                CAPTURE(elevation);
                CAPTURE(tier);
                const SunLookup sun = SunLookupAt(elevation, tier);
                const ShadowView &view = sun.view;
                const FacetedCylinder cylinder = CylinderOf(sides);
                const glm::vec3 toLight = -SunDirection(elevation);
                const StoredDepthFn stored = [&view, &cylinder](const glm::vec2 &uv) {
                                                 return CylinderDepthAt(view, cylinder, uv);
                                             };
                for (std::uint32_t side = 0; side < sides; ++side)
                {
                    for (const float t : kAcrossSide)
                    {
                        const LookupSurface surface = CylinderLookupSurface(cylinder, side, t, toLight);
                        if (glm::dot(surface.normal, toLight) <= 0.f)
                        {
                            continue;
                        }
                        CAPTURE(side);
                        CAPTURE(t);
                        CheckKernelSeesNoBlocker(view, stored, BiasedLookup(view, surface, 1.f), sun.filter);
                    }
                }
            }
        }
    }
}

TEST_CASE("Unlifted, a cylinder side facing away from a low sun is in its neighbours' shadow")
{
    // What the lift is for: without it, lookups on a side past the edge of the
    // light find the side before it nearer the sun.
    constexpr float kElevation = 10.f;
    constexpr std::uint32_t kSides = 8;
    const ShadowView view = SunLookupAt(kElevation, ShadowTier::High).view;
    const FacetedCylinder cylinder = CylinderOf(kSides);
    const glm::vec3 toLight = -SunDirection(kElevation);

    std::uint32_t shadowed = 0;
    for (std::uint32_t side = 0; side < kSides; ++side)
    {
        const glm::vec3 facet = glm::normalize(cylinder.Normal(side) + cylinder.Normal(side + 1));
        for (const float t : kAcrossSide)
        {
            const LookupSurface lifted = CylinderLookupSurface(cylinder, side, t, toLight);
            if (glm::dot(lifted.normal, toLight) <= 0.f || glm::dot(facet, toLight) >= 0.f)
            {
                continue;
            }
            const glm::vec3 point = glm::mix(cylinder.Corner(side), cylinder.Corner(side + 1), t);
            const Lookup flat = BiasedLookup(view,
                                             LookupSurface{.position = point,
                                                           .normal = lifted.normal,
                                                           .plane = facet,
                                                           .incidenceRadians = lifted.incidenceRadians},
                                             1.f);
            shadowed += CylinderDepthAt(view, cylinder, flat.uv) < flat.reference ? 1u : 0u;
        }
    }
    CHECK(shadowed > 0u);
}

TEST_CASE("Another object's shadow stays dark across a cylinder's lifted band")
{
    // A wall between the sun and the cylinder, a few centimetres clear of it:
    // the lift moves lookups by centimetres, and every one of them is still
    // behind the wall.
    constexpr float kElevation = 10.f;
    constexpr float kWallDistance = 0.05f;
    for (const std::uint32_t sides : {8u, 16u})
    {
        CAPTURE(sides);
        const ShadowView view = SunLookupAt(kElevation, ShadowTier::High).view;
        const FacetedCylinder cylinder = CylinderOf(sides);
        const glm::vec3 toLight = -SunDirection(kElevation);
        const std::array<Surface, 1> wall{
            Surface{.point = cylinder.centre + toLight * (kCylinderRadius + kWallDistance), .normal = toLight}};

        for (std::uint32_t side = 0; side < sides; ++side)
        {
            for (const float t : kAcrossSide)
            {
                const LookupSurface surface = CylinderLookupSurface(cylinder, side, t, toLight);
                if (glm::dot(surface.normal, toLight) <= 0.f)
                {
                    continue;
                }
                CAPTURE(side);
                CAPTURE(t);
                const Lookup lookup = BiasedLookup(view, surface, 1.f);
                CHECK(StoredDepthAt(view, wall, lookup.uv) < lookup.reference);
            }
        }
    }
}
