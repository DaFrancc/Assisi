/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ShadowCascades.hpp
/// @brief Where the sun's cascades sit: the split scheme, the slice fit, and
/// the texel snap that keeps their edges still.
///
/// All of it is scalar CPU math with no device in it, which is the point — a
/// cascade that swims under rotation or crawls under translation is the loudest
/// artifact in the whole feature, and the arithmetic that prevents it is
/// checkable without rendering anything.
///
/// Two invariants carry that:
///
///   * A slice is bounded by a **sphere**, not by the eight corners of its
///     sub-frustum. A sphere's radius depends only on the two split distances
///     and the field of view, so turning the camera cannot change how much
///     world one texel covers. Fitting the corners directly gives a box that
///     grows and shrinks as the frustum rotates inside it, and the shadow
///     shimmers at exactly that rate.
///   * The sphere's centre is **snapped** to the cascade's own texel lattice in
///     light space before the matrices are built. Without it, a translation of
///     half a texel resamples every edge in the map and the shadow crawls.

#include <array>

#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/ShadowSettings.hpp>

namespace Assisi::Render
{

/// @brief One fitted cascade: the matrix the depth pass draws with, and the
/// scalars the mesh shader needs to sample and bias it.
struct ShadowCascade
{
    /// World space to this cascade's clip space. What the depth pass renders
    /// with and what the mesh shader looks up with.
    glm::mat4 viewProjection{1.f};

    /// The slice's bounding sphere, after snapping. The centre moves in whole
    /// texels; the radius is fixed for a given split pair and field of view.
    glm::vec3 center{0.f};
    float radius = 0.f;

    /// View-space distances this cascade covers. The mesh shader selects on
    /// `splitFarView`, and fades into the next cascade over the end of the range.
    float splitNearView = 0.f;
    float splitFarView = 0.f;

    /// World units one texel spans. Every bias scales by this, which is what
    /// lets one bias setting hold across cascades whose texels differ by an
    /// order of magnitude.
    float worldUnitsPerTexel = 0.f;

    /// Depth the ortho box spans, in world units. A world-space bias divided by
    /// this is the same bias in the [0, 1] depth the shader compares.
    float depthRange = 0.f;
};

/// @brief The fitted set. `count` is zero when shadows are off or the camera's
/// depth range is degenerate, and the pass draws nothing.
struct CascadeFit
{
    std::array<ShadowCascade, kMaxShadowCascades> cascades{};
    std::uint32_t count = 0;
};

/// @brief What FitCascades needs to know about the camera, the sun and the scene.
struct CascadeFitParams
{
    /// World to camera view. Only its inverse is used, to place the slices.
    glm::mat4 cameraView{1.f};
    /// tan(fovY / 2) and width/height of the camera's projection. Taken as
    /// scalars rather than a matrix because the slice fit is analytic in them.
    float tanHalfFovY = 0.f;
    float aspectRatio = 1.f;

    float nearZ = 0.1f;
    float farZ = 200.f;

    /// The direction the light travels (not the direction toward it).
    glm::vec3 lightDirection{0.f, -1.f, 0.f};

    SunShadowSettings settings;
};

/// @brief View-space distance to split @p index of @p count, counting the near
/// plane as 0 and the far plane as `count`.
///
/// The practical scheme: a logarithmic split distributes texels evenly in
/// screen space but puts its first plane implausibly close, and a uniform one
/// wastes the near cascades entirely. @p lambda weights between them — 1 is
/// fully logarithmic, 0 fully uniform.
[[nodiscard]] float PracticalSplitDistance(float nearZ, float farZ, std::uint32_t index, std::uint32_t count,
                                           float lambda);

/// @brief The bounding sphere of the camera sub-frustum between two view-space
/// distances, in world space.
///
/// The radius is a function of the two distances and the field of view alone,
/// so it is invariant under every camera rotation and translation — which is
/// what makes the cascade's world-per-texel constant and its edges still.
[[nodiscard]] Geometry::BoundingSphere FrustumSliceSphere(const glm::mat4 &inverseCameraView, float tanHalfFovY,
                                                          float aspectRatio, float nearView, float farView);

/// @brief World to light space, rotation only — the basis every cascade shares.
///
/// The light travels along light space's -Z, matching a camera, so an ortho
/// projection built against it needs no sign games. The up axis flips to +Z for
/// a near-vertical sun, where the usual +Y would be degenerate.
[[nodiscard]] glm::mat4 LightRotation(const glm::vec3 &lightDirection);

/// @brief @p center moved to the nearest texel corner of a lattice of
/// @p worldUnitsPerTexel, laid out in @p lightRotation's XY plane.
///
/// Idempotent by construction: snapping an already-snapped point returns it.
/// A zero or non-finite texel size returns @p center untouched, so a degenerate
/// cascade degrades to unsnapped rather than to NaN.
[[nodiscard]] glm::vec3 SnapToTexelGrid(const glm::vec3 &center, const glm::mat4 &lightRotation,
                                        float worldUnitsPerTexel);

/// @brief Fit every cascade for this frame.
[[nodiscard]] CascadeFit FitCascades(const CascadeFitParams &params);

/// @brief The constant depth bias for one cascade, in the [0, 1] depth the
/// shader compares against.
///
/// The setting is in texels, so this is what auto-scales it: a cascade with
/// metre-wide texels gets a metre-scale bias and a cascade with centimetre
/// texels gets a centimetre-scale one, from the same number.
[[nodiscard]] float CascadeDepthBiasNdc(const ShadowCascade &cascade, const SunShadowSettings &settings);

/// @brief Ceiling on the rasterizer's slope-scaled bias, in the [0, 1] depth the
/// shader compares against.
///
/// Slope scaling is unbounded: a polygon approaching edge-on to the light gains
/// depth per pixel without limit, and the bias with it, until the caster sits far
/// enough behind itself to let light under its own shadow. The clamp is what
/// makes that finite, and it is therefore the largest contact gap the caster side
/// can open — so it is quoted in texels, like every other bias here, rather than
/// as a raw fraction of the depth range.
///
/// It needs no cascade because it does not vary by one. A cascade's texel is
/// `2 * radius / resolution` and its depth range is `2 * radius`, so a texel's
/// worth of depth is `1 / resolution` in every cascade alike — which is the same
/// identity that lets one bias setting hold across all of them.
///
/// Past this the sample side's normal offset is what carries the surface. That
/// hand-over is deliberate: the offset is bounded by the sine of the incidence
/// and so cannot run away, where slope scaling can.
[[nodiscard]] float SlopeBiasClampNdc(const SunShadowSettings &settings);

/// @brief Half-width of the PCF kernel, in tap steps.
///
/// The tap *count* is the filter's own business (the shader walks a grid or a
/// disk); this is how far out the outermost tap sits, which is what sets how
/// soft the edge reads.
[[nodiscard]] float FilterRadiusTaps(ShadowFilter filter);

/// @brief Half the penumbra an occluder one world unit away casts, which is the
/// tangent of the sun's angular radius.
///
/// The sun subtends about half a degree from the ground, so a metre of clearance
/// under an occluder softens its shadow edge by a little over four millimetres.
/// This is the figure that makes a shadow's softness a property of the scene
/// rather than of the map: an occluder near its receiver casts a sharp edge and
/// a distant one casts a broad edge, which is both what the eye expects and what
/// keeps a filter from reaching past the very occluder it is filtering.
///
/// A fragment tucked under an occluder is by definition close to it — the inside
/// of a box's ceiling is a hand's breadth from the wall beneath — so the width
/// this yields there is well under a millimetre, and a kernel that narrow has
/// nothing to reach past. That is the difference between narrowing a leak and
/// ending one.
inline constexpr float kSunPenumbraPerWorldUnit = 0.00463f;

/// @brief The widest the sun's penumbra may be, in world units.
///
/// A ceiling on the figure above, for the fragment whose blocker is far enough
/// that the honest penumbra would span more of the map than the map can spare —
/// and for the lit side of an edge, where there is no blocker in front to
/// measure and so no distance to derive a width from.
///
/// A cascade's texel is a different size in every cascade — an order of
/// magnitude across a frame — so a kernel quoted in texels has a reach in metres
/// that grows with the cascade it lands in. Past a point that reach exceeds the
/// thickness of ordinary geometry, and the outer taps stop filtering the
/// occluder and start reading past it: light arrives inside a closed box because
/// the filter is wider than the box's walls.
///
/// Five centimetres is the sun's own penumbra behind an occluder a few metres
/// away, which is the physical figure this is standing in for, and it is thinner
/// than anything a level is likely to be built from.
///
/// A far cascade capped by this filters inside a single texel, so its shadows
/// harden rather than leak. That is the right way round: an edge that aliases at
/// eighty metres is a worse picture, and light through a wall is a wrong one.
///
/// This is the head-on figure. The band a kernel produces lies on the surface
/// being shaded, and a surface the light grazes is crossed by a map that barely
/// moves — so a map-space reach of r covers r / NdotL of it. The shader divides
/// this allowance by the incidence for that reason, which is the same cap
/// expressed where it is seen rather than where it is sampled. Bounding only the
/// map-space reach leaves that magnification unbounded, and a band on a grazing
/// wall then narrows with every reduction without ever closing.
inline constexpr float kMaxPenumbraWorld = 0.05f;

/// @brief UV distance between adjacent PCF taps, for one cascade.
///
/// A texel of the map, except where a texel's worth of kernel would reach
/// further into the world than kMaxPenumbraWorld — there the step goes
/// sub-texel, and the taps crowd inside one texel rather than spreading past
/// what they are meant to be filtering.
///
/// The head-on step. The shader narrows it further by the light's incidence on
/// the surface it is shading, which this cannot know; what is quoted here is the
/// widest that surface will ever ask for.
[[nodiscard]] float CascadeFilterTapStepUv(const ShadowCascade &cascade, const SunShadowSettings &settings);

/// @brief UV distance between adjacent PCF taps.
///
/// One texel of the map, always — so the kernel's world footprint halves every
/// time the resolution doubles.
///
/// This was once quoted against a fixed reference resolution instead, to keep a
/// tier that raised the resolution from also narrowing the penumbra and coming
/// out looking harder than the tier below it. The cost of that was a kernel
/// spanning several real texels, and a footprint has a reach in metres: where it
/// exceeds the thickness of a wall, the outer taps fall past the caster's
/// silhouette onto whatever the map holds beyond it, and vote lit. Light then
/// reaches the inside of a closed box, at every resolution, because the
/// footprint was the one thing resolution did not shrink.
///
/// A shadow that hardens as the map grows is a shadow resolving what is there.
/// Softness that has to be bought with a wider footprint is bought from the
/// occluders it reads through.
[[nodiscard]] float FilterTapStepUv(const SunShadowSettings &settings);

/// @brief The size of one of the map's texels, in UV.
///
/// Not the same as FilterTapStepUv, which is quoted against a fixed reference
/// resolution and so stops shrinking once the map passes it. This is the real
/// texel, which is what the comparison sampler snaps a tap onto.
[[nodiscard]] float ShadowTexelSizeUv(const SunShadowSettings &settings);

/// @brief How many screen pixels one of this cascade's texels covers, for a
/// surface at @p viewDistance.
///
/// The criterion a seam is judged by. A cascade boundary is a step change in
/// texel size, and the step is only visible if the texels are big enough to see
/// in the first place: below about a pixel the shadow's resolution is set by the
/// display rather than by the map, and handing over to a coarser cascade there
/// changes nothing anyone can resolve. That is the same reasoning that puts a
/// mesh LOD switch where the detail it drops has fallen below a pixel.
///
/// @p screenHeight and @p tanHalfFovY describe the camera the shadow is being
/// judged on, so a figure quoted from this is only true at that resolution.
[[nodiscard]] float CascadeTexelScreenPixels(const ShadowCascade &cascade, float viewDistance, float screenHeight,
                                             float tanHalfFovY);

/// @brief Half-width of the shadow's penumbra in this cascade, in world units.
///
/// The kernel's reach plus the half-texel the hardware's own bilinear
/// comparison covers, which is why the one-tap filter is still soft to a texel
/// rather than hard-edged. Reported so a tier's softness is a number rather
/// than an impression: a higher tier that comes out *narrower* here is the
/// defect this exists to catch.
[[nodiscard]] float CascadePenumbraWorld(const ShadowCascade &cascade, const SunShadowSettings &settings);

/// @brief How far along the surface normal a sample is pushed, in world units,
/// before the shader scales it by the sine of the light's incidence.
///
/// The main defence against acne on this side, and the one the constants above
/// deliberately do not try to be. Moving the lookup off the surface covers a
/// texel's worth of the surface's own depth at any angle, where a constant
/// large enough to do the same at grazing incidence would detach every shadow
/// from its contact edge at head-on incidence.
///
/// Scaled by the cascade's texel and by nothing else — in particular not by the
/// filter's reach. The offset moves the lookup sideways, and sideways is the one
/// direction that walks it out from under a neighbouring occluder: at a concave
/// corner it escapes the very surface that shades the receiver, so a leak is
/// what it buys and a texel is all it may spend. Sizing it by the kernel instead
/// makes the widest filter the leakiest tier, which inverts what a quality
/// setting means. The taps' own self-shadowing belongs to the depth biases,
/// which push along the light and leave the corner where it is.
///
/// Its cost is bounded and worth stating: a lookup moved this far can read past
/// an occluder that is nearer than the offset, so the offset is also the largest
/// contact gap the shadow can show. That is why it is quoted in texels — the
/// error it hides scales with a texel, so the leak it trades for should too, and
/// raising the resolution then narrows both together.
[[nodiscard]] float CascadeNormalOffsetWorld(const ShadowCascade &cascade, const SunShadowSettings &settings);

} // namespace Assisi::Render
