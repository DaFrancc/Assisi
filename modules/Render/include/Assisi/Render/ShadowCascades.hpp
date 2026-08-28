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
#include <optional>

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

    /// The smallest `dot(p, lightDirection)` reached by any shadow caster, its
    /// bounding radius included — how far upstream of the camera geometry still
    /// throws shadows into view. Every cascade's near plane is pulled back to
    /// it, so a wall behind the camera still shadows the ground in front of it.
    ///
    /// Absent means no casters were gathered, and each cascade keeps its own
    /// sphere as its near plane.
    ///
    /// One value for every cascade rather than one each: a per-cascade minimum
    /// would keep the near slices' depth ranges tight, at the cost of a second
    /// pass over the casters. The cost of sharing is depth precision — the
    /// range becomes the scene's extent along the light instead of the slice's
    /// — which D32 absorbs completely and D16 absorbs down to millimetres over
    /// any level this engine loads.
    std::optional<float> casterNearAlongLight;

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
/// texel — what the comparison sampler snaps a tap onto, and therefore how far
/// off the receiver's plane a tap can land before its kernel offset is even
/// considered.
[[nodiscard]] float ShadowTexelSizeUv(const SunShadowSettings &settings);

/// @brief The steepest receiver the plane fit is trusted to describe, as world
/// depth gained per world unit travelled across the map.
///
/// A receiver-plane gradient is differenced from the fragment's screen-space
/// quad, and is only a slope while that quad lies on one surface. Where two
/// meet — an inside corner, a silhouette — it spans both, and what it reports
/// is the gap between them rather than any surface's slope.
///
/// The two cases are indistinguishable from the gradient alone: a plane nearly
/// edge-on to the light and a quad straddling a corner both report an enormous
/// number. So the ambiguous case needs a default, and the default is to decline.
/// Past this limit the gradient is dropped and the tap compares uncorrected,
/// which costs a little acne where a very grazing receiver really was a plane.
/// The alternative default — correcting by as much as the limit allows — spends
/// that same ambiguity on lighting a band along every inside edge, which is the
/// leak the plane bias exists to remove.
///
/// Four is a receiver about 76 degrees off face-on. Past that a texel spans
/// enough of the surface that the kernel cannot resolve its slope anyway.
inline constexpr float kMaxReceiverPlaneSlope = 4.0f;

/// @brief The largest gradient the plane fit may report before it is treated as
/// spanning a discontinuity, in comparison depth per unit of shadow-map UV.
///
/// A cascade's own quantity: a given world slope becomes a steeper gradient in
/// UV the wider the cascade's box is, and a shallower one the deeper its range.
[[nodiscard]] float CascadeReceiverGradientLimit(const ShadowCascade &cascade, const SunShadowSettings &settings);

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

/// @brief How far along the surface normal a sample is pushed, in world units.
///
/// The same auto-scaling as the depth bias. Both are small by default and meant
/// to stay that way: what used to justify a large one — a receiver sloped
/// across the filter kernel — is corrected per tap by the mesh shader's
/// receiver-plane bias, and the residue these cover does not grow with the
/// angle to the light. Growing either buys a leak along every silhouette,
/// widening with the cascade because a texel does.
[[nodiscard]] float CascadeNormalOffsetWorld(const ShadowCascade &cascade, const SunShadowSettings &settings);

} // namespace Assisi::Render
