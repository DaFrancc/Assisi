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

/// @brief Half-width of the PCF kernel, in tap steps.
///
/// The tap *count* is the filter's own business (the shader walks a grid or a
/// disk); this is how far out the outermost tap sits, which is what sets how
/// soft the edge reads.
[[nodiscard]] float FilterRadiusTaps(ShadowFilter filter);

/// @brief UV distance between adjacent PCF taps.
///
/// One texel at or below kFilterReferenceResolution, and fixed above it — so
/// raising the resolution sharpens the occluder's silhouette without narrowing
/// the blur that softens it. See kFilterReferenceResolution for why that
/// matters more than it sounds.
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
/// Scaled by the cascade's texel and by the filter's reach, because the tap
/// that has to clear the surface is the outermost one. The setting is therefore
/// a multiplier on "one kernel radius of texels" rather than an absolute count,
/// so changing the filter does not silently require retuning it.
///
/// Its cost is bounded and worth stating: a lookup moved this far can read past
/// an occluder that is nearer than the offset, so the offset is also the largest
/// contact gap the shadow can show. That is why it is quoted in texels — the
/// error it hides scales with a texel, so the leak it trades for should too.
[[nodiscard]] float CascadeNormalOffsetWorld(const ShadowCascade &cascade, const SunShadowSettings &settings);

} // namespace Assisi::Render
