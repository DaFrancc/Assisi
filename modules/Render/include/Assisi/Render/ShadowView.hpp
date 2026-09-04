/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ShadowView.hpp
/// @brief One shadow map view: where it draws, where in its target it lands,
/// and how a lookup into it is biased.
///
/// A cascade, a spot light's map and one face of a point light differ in how
/// their matrix is fitted and in nothing else. Each rasterizes the scene's
/// depth from somewhere, into some rectangle of some target, and is later
/// sampled with a bias scaled to how much world one of its texels covers. This
/// is that common shape, so the depth renderer takes a view rather than a
/// cascade index and the same code path serves all three.
///
/// The rectangle is carried in texels rather than as a UV sub-rect, because
/// texels are what the rasterizer's viewport wants and what an allocator hands
/// out. The UV form the shader samples with is derived from it, so the two can
/// never disagree.

#include <cstdint>

#include <nvrhi/nvrhi.h>

#include <Assisi/Math/Angles.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/GpuLayout.hpp>
#include <Assisi/Render/ShadowCascades.hpp>
#include <Assisi/Render/ShadowSettings.hpp>

namespace Assisi::Render
{

/// @brief A rectangle of a shadow-map target, in texels.
struct ShadowViewRect
{
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

/// @brief One tile of an atlas: the rectangle, and the atlas it was cut from.
///
/// The two are meaningless apart. A rectangle in texels says nothing about what
/// fraction of a texture it is, and every use of one here — the UV transform,
/// the filter's tap step, the inset that keeps a kernel inside its own tile —
/// needs both. ShadowView keeps them as two fields for the same reason.
struct ShadowAtlasTile
{
    ShadowViewRect rect;
    std::uint32_t atlasResolution = 0;
};

/// @brief Where one shadow map view renders and how it is sampled.
struct ShadowView
{
    /// World space to this view's clip space. What the depth pass draws with
    /// and what a lookup transforms by.
    glm::mat4 viewProjection{1.f};

    /// The part of @ref targetResolution this view owns. A cascade owns its
    /// whole slice; an atlas tile owns a rectangle of the shared texture.
    ShadowViewRect rect;

    /// Width and height of the target the rect is measured in. Square, because
    /// both the cascade array and the atlas are.
    std::uint32_t targetResolution = 0;

    /// Which array slice of the target. Cascades take one each; every atlas
    /// tile shares slice zero.
    std::uint32_t arraySlice = 0;

    /// Whether this view projects orthographically.
    ///
    /// Two things in the depth pass are only valid when it does, and both are
    /// silently wrong when it does not:
    ///
    ///   * **Pancaking.** shadow_depth.vert clamps a caster upstream of the near
    ///     plane onto it rather than letting it clip. That is a clamp in the
    ///     depth the comparison uses only while `w` is exactly 1. Under a
    ///     perspective projection the comparison depth is `z / w`, so clamping
    ///     `z` on some vertices of a triangle and not others leaves an
    ///     interpolated depth that means nothing — too near in places, too far in
    ///     others, and too far is a leak.
    ///   * **Dropping the near plane from the cull.** A perspective frustum's
    ///     four side planes extended form a double cone, so without the near
    ///     plane the mirrored half behind the light passes the test.
    ///
    /// False by default: a view that has not said gets the conservative
    /// treatment, which costs a caster its pancaking rather than corrupting a map.
    bool orthographic = false;

    /// Depth bias, in the [0, 1] depth a lookup compares against.
    ///
    /// A cascade's is the whole bias, already scaled by its world-per-texel — an
    /// orthographic texel is the same size everywhere in the map, so one number
    /// describes it. An atlas tile's is that number **times the receiver's
    /// distance from the light**, and the shader divides: a perspective texel
    /// grows with distance, so no single number can describe it.
    float depthBias = 0.f;

    /// How far along the surface normal a sample is pushed.
    ///
    /// World units for a cascade; world units **per unit of the receiver's
    /// distance** for an atlas tile, for the same reason. See
    /// LocalNormalOffsetPerDistance for why getting this one wrong leaks rather
    /// than acnes.
    float normalOffset = 0.f;

    /// UV distance between adjacent filter taps, in this view's target.
    float filterTapStepUv = 0.f;

    /// The rectangle of the target a lookup into this view may sample, as
    /// xy = minimum UV, zw = maximum UV.
    ///
    /// The whole target for a view that owns one; the tile inset by the filter's
    /// reach for an atlas tile, which is the only thing stopping a kernel at a
    /// tile's edge from reading the light next to it. See ShadowViewClampUv.
    glm::vec4 clampUv{0.f, 0.f, 1.f, 1.f};
};

/// @brief The view's rectangle as a UV transform: xy scale, zw offset.
///
/// A lookup that lands at @c uv inside the view samples the target at
/// `uv * scale + offset`. A view owning its whole target gets (1, 1, 0, 0), so
/// the atlas case costs the same multiply-add the cascade case already pays.
///
/// A zero target resolution — an unfitted view — returns the identity rather
/// than dividing by it.
[[nodiscard]] glm::vec4 ShadowViewUvScaleOffset(const ShadowView &view);

/// @brief The rasterizer viewport and scissor that confine drawing to the
/// view's rectangle.
[[nodiscard]] nvrhi::Viewport ShadowViewViewport(const ShadowView &view);

/// @brief One view's record in the table the GPU reads, std430-packed.
///
/// The array slice rides in a float lane because that is the form the array
/// sampler takes it in; there is no integer conversion at the sample site.
struct ShadowViewGpu
{
    glm::mat4 viewProjection{1.f};
    /// xy = UV scale, zw = UV offset. See ShadowViewUvScaleOffset.
    glm::vec4 uvScaleOffset{1.f, 1.f, 0.f, 0.f};
    /// x = depth bias, y = normal offset, z = filter tap step, w = array slice.
    glm::vec4 params{0.f};
    /// xy = minimum UV, zw = maximum UV a lookup may sample. See
    /// ShadowView::clampUv.
    glm::vec4 clampUv{0.f, 0.f, 1.f, 1.f};
};
ASSISI_GPU_LAYOUT(ShadowViewGpu);
ASSISI_GPU_FIRST_FIELD(ShadowViewGpu, viewProjection);
ASSISI_GPU_FIELD_AFTER(ShadowViewGpu, uvScaleOffset, viewProjection);
ASSISI_GPU_FIELD_AFTER(ShadowViewGpu, params, uvScaleOffset);
ASSISI_GPU_FIELD_AFTER(ShadowViewGpu, clampUv, params);
ASSISI_GPU_NO_TAIL_PADDING(ShadowViewGpu, clampUv);

/// @brief @p view in the layout the GPU table carries it in.
[[nodiscard]] ShadowViewGpu PackShadowView(const ShadowView &view);

/// @brief The view for one fitted cascade of the sun.
///
/// A cascade owns its whole array slice, so its rectangle is the full target
/// and its UV transform is the identity. The biases arrive already multiplied
/// by the cascade's world-units-per-texel, which is what lets one setting hold
/// across cascades whose texels differ by an order of magnitude.
[[nodiscard]] ShadowView CascadeShadowView(const ShadowCascade &cascade, std::uint32_t slice,
                                           const SunShadowSettings &settings);

/// @brief Which face of a point light's cube a direction falls in.
///
/// +X, -X, +Y, -Y, +Z, -Z — the cubemap convention. The *numbers* are the
/// contract rather than a choice: a light's six views are written into the frame
/// table in this order, and mesh.frag recomputes the index from the same rule, so
/// the two orderings have to be the same one. Reordering these renames nothing
/// and breaks nothing at build time; it makes every point light sample a
/// neighbouring face's depth.
///
/// A plain index rather than an enum class because it is exactly that — an
/// offset from a light's first view into the table, and every use of it is
/// arithmetic on that table.
enum PointLightFace : std::uint32_t
{
    kPointLightFacePositiveX = 0,
    kPointLightFaceNegativeX = 1,
    kPointLightFacePositiveY = 2,
    kPointLightFaceNegativeY = 3,
    kPointLightFacePositiveZ = 4,
    kPointLightFaceNegativeZ = 5,
    /// One past the last, which is also how many views a point light spends.
    kPointLightFaceCount = 6,
};

/// @brief The axis face @p face looks along.
[[nodiscard]] glm::vec3 PointLightFaceDirection(std::uint32_t face);

/// @brief Which of the six faces a direction from the light falls in.
///
/// The face whose axis @p direction is most aligned with, which is what the
/// shader's own selection has to agree with. A zero direction lands on +X rather
/// than nowhere.
[[nodiscard]] std::uint32_t PointLightFaceOf(const glm::vec3 &direction);

/// @brief How much wider than 90 degrees a point light's face is drawn, in
/// degrees of full field of view.
///
/// Six 90-degree faces tile a cube exactly, and exactly is the problem: a PCF
/// kernel at the edge of a tile reaches past it, and past it is the next light's
/// tile. Widening every face gives the kernel somewhere to land that still
/// belongs to this light, so the seam between two faces of one point light shows
/// the same geometry from either side instead of a hard line.
///
/// The cost is resolution — a wider frustum spreads the same texels over more
/// world — so this is as small as it can be while covering the widest kernel the
/// filters here use.
inline constexpr float kPointLightFaceOverlapDegrees = 6.0f;

/// @brief Where a local light stands and how far it reaches — everything the
/// shape of its shadow map depends on.
///
/// One struct rather than four arguments because these four always travel
/// together and are always compared together: the atlas's cache asks whether a
/// tile was recorded from where the light now stands, and a field added here
/// that the comparison did not know about would be a light that moved without
/// its shadow noticing.
struct LocalShadowLightPose
{
    glm::vec3 position{0.f};
    /// Where a spot aims, in world space and already normalised. Unread for a
    /// point light, which aims in all six directions.
    glm::vec3 direction{0.f, -1.f, 0.f};
    /// The light's influence radius, which is also its shadow map's far plane:
    /// nothing past it is lit, so nothing past it needs to occlude.
    float range = 0.f;
    /// Half-angle of the spot's outer cone, in degrees, at most
    /// Math::kMaxConeHalfAngleDegrees. Unread for a point light.
    float outerAngleDegrees = Math::kDefaultSpotOuterAngleDegrees;

    /// Exactly, never within a tolerance: a light nothing has written carries
    /// bit-identical values frame to frame, so any difference at all is one the
    /// depth in its tile did not see.
    [[nodiscard]] bool operator==(const LocalShadowLightPose &) const = default;
};

/// @brief The view for a spot light's single map.
///
/// A spot is a cone, so its map is one perspective frustum with the cone's outer
/// angle for a field of view — widened a little, for the same reason a point
/// light's faces are: the filter kernel at the edge of the cone must land on
/// depth this light recorded rather than on whatever the neighbouring tile holds.
///
/// The near plane is a fraction of the light's range rather than a constant: a
/// spot lighting a room and one lighting a hall differ by two orders of
/// magnitude in reach, and a fixed near plane spends the entire depth format on
/// whichever of the two it was chosen for.
///
/// @p tile is what the allocator handed out. The biases scale by the tile's own
/// resolution, so a demoted light is biased as the smaller map it actually got.
[[nodiscard]] ShadowView SpotShadowView(const LocalShadowLightPose &pose, const ShadowAtlasTile &tile,
                                        const LocalShadowSettings &settings);

/// @brief The view for one face of a point light's cube.
///
/// Six of these cover every direction. Each is a 90-degree frustum widened by
/// kPointLightFaceOverlapDegrees so the faces overlap rather than abut.
[[nodiscard]] ShadowView PointFaceShadowView(const LocalShadowLightPose &pose, std::uint32_t face,
                                             const ShadowAtlasTile &tile, const LocalShadowSettings &settings);

/// @brief What one texel of a tile covers per unit of distance from the light.
///
/// A local light's map is a perspective projection, so a texel is a solid angle:
/// what it covers is not a width but a width *per unit distance*, and it grows
/// linearly as you move away from the light. That is the whole difference from a
/// cascade, whose orthographic texel covers the same world distance everywhere in
/// the map and so can be described by one number.
///
/// Every bias below is quoted this way and multiplied by the receiver's own
/// distance in the shader. Quoting them at the far plane instead — one number,
/// cascade-style — over-biases everything nearer by the ratio of the light's
/// range to the receiver's distance, which for a floor under a long-ranged lamp
/// is an order of magnitude.
[[nodiscard]] float LocalTexelsPerUnitDistance(std::uint32_t tileResolution, float tanHalfFov);

/// @brief The depth bias for one tile, in [0, 1] depth **times** the receiver's
/// distance from the light.
///
/// The shader divides by that distance. Two things vary with it and they do not
/// cancel: a texel's world footprint grows linearly, while a world unit is worth
/// less depth the further out it is, as the inverse square. Their product is the
/// inverse of the distance, which is why this is a coefficient rather than a
/// constant.
///
/// It divides by the tile's own resolution, which is what makes a demoted light
/// correctly biased for the smaller map it actually got rather than for the one
/// the setting names.
[[nodiscard]] float LocalDepthBiasNdcTimesDistance(std::uint32_t tileResolution, float nearPlane, float farPlane,
                                                   float tanHalfFov, const LocalShadowSettings &settings);

/// @brief How far along the surface normal a lookup is pushed, in world units
/// **per unit** of the receiver's distance from the light.
///
/// The shader multiplies by that distance and by the sine of the light's
/// incidence. Distance matters because this offset is only ever meant to clear
/// one texel of the map, and a texel out at the receiver is larger than a texel
/// near the light.
///
/// Getting that wrong leaks rather than acnes, and leaks hard: the offset moves
/// the lookup *sideways*, so an offset sized for somewhere further away walks the
/// sample out from under the occluder that should be shading it — and, at a point
/// light, far enough sideways to leave the face it was chosen for.
[[nodiscard]] float LocalNormalOffsetPerDistance(std::uint32_t tileResolution, float tanHalfFov,
                                                 const LocalShadowSettings &settings);

/// @brief Ceiling on the rasterizer's slope-scaled bias for a tile of
/// @p tileResolution texels, in the [0, 1] depth the map stores.
///
/// Slope scaling is unbounded — a polygon approaching edge-on to the light gains
/// depth per pixel without limit — so this cap is the widest contact gap the
/// caster side can open, and it belongs to the map's resolution.
///
/// **Not the cascades' `1 / resolution`.** That identity holds only for an
/// orthographic projection, where depth is linear in distance and a texel is
/// always the same fraction of the depth range. A local light's projection is
/// perspective, where a fraction of NDC depth near the far plane is an enormous
/// world distance, and the cascade figure lets a grazing surface be pushed metres
/// behind itself — which reads as light under everything the surface should have
/// shadowed.
///
/// The light's own range cancels out, which is what makes one number serve every
/// light: the near plane is a fixed fraction of the far plane, so the ratio that
/// sets the depth curve is the same in all of them.
///
/// Quoted for a point light's face, the widest frustum here. A narrow spot gets a
/// slightly looser cap than it strictly needs, which errs toward acne rather than
/// toward the leak this exists to close.
[[nodiscard]] float LocalSlopeBiasClampNdc(std::uint32_t tileResolution);

/// @brief UV distance between adjacent filter taps in an atlas of
/// @p atlasResolution texels a side.
///
/// One texel, because a tile's texels *are* the atlas's texels — a tile is a
/// rectangle of the same grid, not a resampling of it. That is also why a
/// demoted tile does not want a wider step: its texels did not grow, there are
/// simply fewer of them, and a kernel quoted against the tile would step several
/// atlas texels at a time and skip past what it is meant to be filtering.
[[nodiscard]] float LocalFilterTapStepUv(std::uint32_t atlasResolution);

/// @brief The tile's interior, inset by however far the filter reaches.
///
/// A tile in an atlas has no border to clamp against: the sampler's own clamp is
/// to the whole texture, so a tap that leaves the tile lands in the neighbouring
/// light's depth and reports whatever that light saw. Every lookup is clamped to
/// this rectangle instead, which is the tile minus the kernel's radius on each
/// side. The overlap the faces are drawn with is what makes that inset lossless —
/// the geometry the inset trims off is geometry the neighbouring face also has.
///
/// Returned as UV, in the atlas's own space: xy = minimum, zw = maximum.
[[nodiscard]] glm::vec4 ShadowViewClampUv(const ShadowView &view, ShadowFilter filter);

} // namespace Assisi::Render
