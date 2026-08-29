/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ShadowSettings.hpp
/// @brief The shadow quality knobs, and the tiers that are presets over them.
///
/// Everything here is a value the CPU and the shader have to agree about: what
/// a filter selection means on the wire, what range a bias is allowed to take,
/// and what a named tier sets. The cascade *math* is in ShadowCascades.hpp; the
/// pass that renders with it is in ShadowPass.hpp.
///
/// The knobs come in two halves. The sun's shadow is a set of cascades fitted
/// to the camera, so it has a cascade count, a split distribution and a
/// distance past which it stops. A spot light's shadow is a tile in an atlas
/// and has none of those: nothing distributes, nothing splits, and the light's
/// own range is how far it reaches. The halves therefore share their filter and
/// bias vocabulary and nothing else, and a single struct holding both would
/// have to say which of its fields a spot light ignores.
///
/// A tier is a preset, never a lock: selecting one writes the fields below and
/// then the fields are editable. That is why Tier() reports Custom rather than
/// storing which button was last pressed.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Assisi::Render
{

/// @brief The PCF kernel a shadow is filtered with.
///
/// These values are the wire encoding — mesh.frag switches on the integer, so
/// reordering them changes the look of every saved options.json rather than
/// failing to build.
enum class ShadowFilter : std::uint8_t
{
    /// One bilinear comparison fetch. The hardware still blends four texels, so
    /// this is soft to about a texel, not a hard-edged point sample.
    Point = 0,
    /// 3x3 comparison taps on the texel grid.
    Pcf3x3 = 1,
    /// 5x5, same grid.
    Pcf5x5 = 2,
    /// 16 taps on a Vogel disk, rotated per pixel by an interleaved-gradient
    /// angle. Trades the grid's banding for noise, which reads as a softer edge
    /// than a grid of the same reach.
    Vogel = 3,
};

inline constexpr std::uint32_t kShadowFilterCount = 4;

/// @brief Depth format of a shadow map.
///
/// Wire encoding, like ShadowFilter: it is persisted.
enum class ShadowMapFormat : std::uint8_t
{
    D16 = 0, ///< Half the memory and half the bandwidth, at 16 bits over the whole depth range.
    D32 = 1,
};

inline constexpr std::uint32_t kShadowMapFormatCount = 2;

/// @brief A named point in the knob space below.
///
/// Custom is not a setting anything can be put into — it is what Tier() reports
/// when the fields match no preset, which is the normal state after any edit.
enum class ShadowTier : std::uint8_t
{
    Low = 0,
    Medium = 1,
    High = 2,
    Ultra = 3,
    Custom = 4,
};

inline constexpr std::uint32_t kShadowTierCount = 4; // presets only; Custom is a report, not a choice

/// @brief Hard cap on cascades. The shader carries this many matrices in its
/// frame constants and the pass allocates this many array slices, so raising it
/// is a change on both sides.
///
/// Eight rather than four because the seam between two cascades is a step in
/// how coarse the shadow is, and its size is the ratio between their extents —
/// which for a fixed distance falls as the count rises. Four cascades cannot
/// cover a useful range without a step you can see; more, smaller ones can, and
/// each covers little enough that it needs fewer texels to do it.
inline constexpr std::uint32_t kMaxShadowCascades = 8;
inline constexpr std::uint32_t kMinShadowCascades = 1;

/// @brief Cascade resolution bounds. The floor is where a 3x3 kernel stops
/// resolving anything at typical splits; the ceiling is 4 x 4096^2 x 4 bytes,
/// which is already 256 MiB of shadow map.
inline constexpr std::uint32_t kMinShadowResolution = 256;
inline constexpr std::uint32_t kMaxShadowResolution = 4096;

/// @brief Local-light atlas bounds. Wider than a cascade's at the top because
/// one atlas holds every spot and point face at once, where a cascade is one
/// map among a handful.
inline constexpr std::uint32_t kMinShadowAtlasResolution = 512;
inline constexpr std::uint32_t kMaxShadowAtlasResolution = 8192;

/// @brief Bounds on the tile one local light's face gets. The ceiling is a
/// quarter of the largest atlas in each axis, so even the biggest face class
/// leaves room for fifteen more.
inline constexpr std::uint32_t kMinShadowFaceResolution = 128;
inline constexpr std::uint32_t kMaxShadowFaceResolution = 2048;

/// @brief How far from the camera the sun casts. Past the ceiling the outermost
/// cascade's texels are metres wide and the shadow is a stain rather than a shape.
inline constexpr float kMinShadowDistance = 5.0f;
inline constexpr float kMaxShadowDistance = 500.0f;

/// @brief Weight between the uniform and logarithmic split schemes. 0 is
/// uniform, 1 is fully logarithmic.
inline constexpr float kMinSplitLambda = 0.0f;
inline constexpr float kMaxSplitLambda = 1.0f;

/// @brief Distance the split distribution is measured from, whatever the camera
/// puts its near plane at.
///
/// A logarithmic split spaces cascades by a constant ratio, which is the only
/// arrangement whose seams are all the same size and the only one that gets
/// better as cascades are added. Measured from a camera near plane of 0.1 m it
/// is unusable: the ratio over that range is enormous, so the first split lands
/// at half a metre and two entire cascades are spent before the near wall of a
/// room. Measured from here, the ratio is the far distance over this — around
/// 50 rather than 800 — and the scheme behaves.
///
/// Cascade zero still *covers* from the camera's near plane; only the
/// distribution starts here. Nothing goes unshadowed for being closer.
inline constexpr float kSplitDistributionNear = 1.5f;

/// @brief Constant depth bias, in shadow-map texels. Auto-scaled per view by
/// that view's world-units-per-texel, so one value holds across maps whose
/// texels differ by an order of magnitude.
///
/// Like the normal offset, this is a floor under the receiver-plane bias rather
/// than the thing that stops acne. It covers what the plane cannot: the depth
/// format's own quantisation, and the residual where a receiver is curved
/// across the kernel. Raising it buys nothing the plane bias does not already
/// give and detaches every shadow from its contact edge.
inline constexpr float kMinDepthBiasTexels = 0.0f;
inline constexpr float kMaxDepthBiasTexels = 8.0f;

/// @brief Slope-scaled bias applied by the rasterizer while the map is drawn,
/// in depth-format units per unit of depth slope.
inline constexpr float kMinSlopeBias = 0.0f;
inline constexpr float kMaxSlopeBias = 8.0f;

/// @brief How far along the surface normal a sample is pushed before it is
/// looked up, in shadow-map texels.
///
/// A safety net, not the mechanism: the receiver-plane bias in the mesh shader
/// is what corrects a tap for the receiver's slope, and it does so by an amount
/// derived from the surface rather than by a constant that has to be large
/// enough for the worst case. What is left for this to cover is the case that
/// bias cannot see — a receiver whose curvature or displacement leaves it off
/// the plane its own screen-space quad reports.
///
/// Every texel of it is a leak: the offset moves the *lookup*, so near a
/// silhouette it moves the lookup off the occluder and the fragment reads lit.
/// The offset is quoted in texels and a texel grows with the cascade, so a
/// setting large enough to matter close up opens a hole metres wide out at the
/// last cascade.
inline constexpr float kMinNormalOffsetTexels = 0.0f;
inline constexpr float kMaxNormalOffsetTexels = 8.0f;

/// @brief Fraction of each cascade's depth range spent fading into the next.
///
/// Zero shows the seam. A seam is a step change in how coarse the shadow is —
/// texels roughly double across one — so the band is not hiding a subtle thing,
/// and the faster the camera crosses it the more the change reads as an event
/// rather than a gradient.
///
/// The ceiling is a whole cascade, where every shadowed pixel blends the
/// cascade it is in with the next one. That is the cheapest way to soften a
/// seam — it buys smoothness with filter cost rather than with the memory and
/// the extra depth pass another cascade would need — and it is why the ceiling
/// is not lower: a wide band is a legitimate setting, not an abuse of one. Its
/// price is up to two filter kernels per shadowed pixel instead of one.
inline constexpr float kMinCascadeBlend = 0.0f;
inline constexpr float kMaxCascadeBlend = 1.0f;

/// @brief The sun's shadows, as the user edits them.
struct SunShadowSettings
{
    /// Whether the sun casts at all. Off costs the shadow pass nothing: no
    /// cascade is fitted, no depth map is drawn, and the mesh shader's sampling
    /// branch is skipped on a frame constant.
    bool enabled = true;

    /// Four, not more. A cascade costs GPU time in proportion to the count —
    /// every caster is submitted again for each one — and the default is the
    /// tier that has to be cheap.
    std::uint32_t cascadeCount = 4;
    std::uint32_t resolution = 2048;
    ShadowMapFormat format = ShadowMapFormat::D32;

    float maxDistance = 80.0f;

    /// Fully logarithmic, which is what makes every seam the same size and what
    /// makes adding a cascade actually help — a blended scheme spaces the near
    /// splits almost uniformly, so their ratios stay put however many cascades
    /// are added and the worst seam barely moves. Usable only because the
    /// distribution is measured from kSplitDistributionNear rather than from the
    /// camera's near plane.
    float splitLambda = 1.0f;

    ShadowFilter filter = ShadowFilter::Pcf3x3;

    /// A quarter texel, and it is not the mechanism. Everything angle-dependent
    /// is covered by the rasterizer's slope bias on the caster and the normal
    /// offset on the receiver; what is left for a constant is the depth format's
    /// quantisation and a receiver curved across the kernel. Raising it past
    /// that buys nothing and detaches every shadow from its contact edge.
    float depthBiasTexels = 0.25f;

    /// Slope-scaled, applied by the rasterizer as the map is drawn. The only
    /// bias that sees the polygon being recorded, so it is the one that can
    /// answer for that polygon's own tilt.
    float slopeBias = 2.0f;

    /// Texels of the cascade's own map, moved along the geometric normal. Half a
    /// texel is the floor that means anything: the hardware comparison blends
    /// four texels, so a lookup that has not cleared half a texel has not left
    /// the surface it is standing on. It is also the ceiling worth paying — this
    /// is the one bias that moves the lookup sideways, and sideways is what walks
    /// a receiver out from under the occluder next to it.
    float normalOffsetTexels = 0.5f;

    /// A third of each cascade. A seam is a step in texel size and the band is
    /// the only thing that turns it into a gradient, so it wants real distance —
    /// especially for a camera moving fast enough to cross a short one in a
    /// frame or two, which is when a ramp reads as a pop.
    float cascadeBlend = 0.33f;

};

/// @brief Spot and point lights' shadows, as the user edits them.
///
/// These are the knobs that decide what is *allocated*: how big the shared
/// atlas is, what a light's face gets out of it, and how a lookup into it is
/// filtered and biased. How lights compete for those tiles when there are more
/// of them than there is atlas is selection policy, and it belongs with the
/// selector that applies it rather than here.
struct LocalShadowSettings
{
    /// Whether spot and point lights cast at all.
    bool enabled = true;

    /// One atlas for every local light's face. D16 by default and at every
    /// tier: a local light's depth range is its own reach, metres rather than
    /// the scene's extent, and 16 bits over metres is millimetres.
    std::uint32_t atlasResolution = 4096;
    ShadowMapFormat format = ShadowMapFormat::D16;

    /// The tile one face gets before anything demotes it. A spot spends one of
    /// these and a point light six, which is why a single count of "shadowed
    /// lights" would mean six times the work depending on the mix.
    std::uint32_t faceResolution = 512;

    ShadowFilter filter = ShadowFilter::Pcf3x3;

    float depthBiasTexels = 1.5f;
    float slopeBias = 2.0f;
    float normalOffsetTexels = 1.5f;
};

/// @brief Every shadow knob, in its two halves.
struct ShadowSettings
{
    SunShadowSettings sun;
    LocalShadowSettings local;
};

/// @brief Clamps to [low, high], substituting `fallback` for a non-finite value.
///
/// std::clamp alone returns NaN unchanged — both of its comparisons are false —
/// and a NaN reaching a cascade matrix takes every shadowed pixel with it.
[[nodiscard]] inline float ClampFiniteShadow(float value, float low, float high, float fallback)
{
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

/// @brief @p value clamped into [low, high] and then rounded down to a power of
/// two.
///
/// Rounded down rather than merely clamped: a map whose width is not a power of
/// two still allocates, but the texel grid a snap quantises to stops lining up
/// with anything, an atlas's size classes stop tiling it exactly, and the memory
/// figure in the tier table stops being predictable.
[[nodiscard]] inline std::uint32_t ShadowResolutionPowerOfTwo(std::uint32_t value, std::uint32_t low,
                                                              std::uint32_t high)
{
    value = std::clamp(value, low, high);
    std::uint32_t power = low;
    while (power * 2u <= value)
    {
        power *= 2u;
    }
    return power;
}

/// @brief The same settings with every lane inside its range, and every
/// resolution rounded down to a power of two.
///
/// options.json is hand-editable and these values size GPU allocations and
/// reach a shader, so nothing downstream may assume they are sane.
[[nodiscard]] inline SunShadowSettings Sanitized(SunShadowSettings settings)
{
    const SunShadowSettings defaults;

    if (static_cast<std::uint32_t>(settings.filter) >= kShadowFilterCount)
    {
        settings.filter = defaults.filter;
    }
    if (static_cast<std::uint32_t>(settings.format) >= kShadowMapFormatCount)
    {
        settings.format = defaults.format;
    }

    settings.cascadeCount = std::clamp(settings.cascadeCount, kMinShadowCascades, kMaxShadowCascades);
    settings.resolution = ShadowResolutionPowerOfTwo(settings.resolution, kMinShadowResolution, kMaxShadowResolution);

    settings.maxDistance = ClampFiniteShadow(settings.maxDistance, kMinShadowDistance, kMaxShadowDistance,
                                             defaults.maxDistance);
    settings.splitLambda = ClampFiniteShadow(settings.splitLambda, kMinSplitLambda, kMaxSplitLambda,
                                             defaults.splitLambda);
    settings.depthBiasTexels = ClampFiniteShadow(settings.depthBiasTexels, kMinDepthBiasTexels, kMaxDepthBiasTexels,
                                                 defaults.depthBiasTexels);
    settings.slopeBias = ClampFiniteShadow(settings.slopeBias, kMinSlopeBias, kMaxSlopeBias, defaults.slopeBias);
    settings.normalOffsetTexels = ClampFiniteShadow(settings.normalOffsetTexels, kMinNormalOffsetTexels,
                                                    kMaxNormalOffsetTexels, defaults.normalOffsetTexels);
    settings.cascadeBlend = ClampFiniteShadow(settings.cascadeBlend, kMinCascadeBlend, kMaxCascadeBlend,
                                              defaults.cascadeBlend);
    return settings;
}

/// @brief The local half, sanitized on the same terms as the sun's.
[[nodiscard]] inline LocalShadowSettings Sanitized(LocalShadowSettings settings)
{
    const LocalShadowSettings defaults;

    if (static_cast<std::uint32_t>(settings.filter) >= kShadowFilterCount)
    {
        settings.filter = defaults.filter;
    }
    if (static_cast<std::uint32_t>(settings.format) >= kShadowMapFormatCount)
    {
        settings.format = defaults.format;
    }

    settings.atlasResolution =
        ShadowResolutionPowerOfTwo(settings.atlasResolution, kMinShadowAtlasResolution, kMaxShadowAtlasResolution);
    settings.faceResolution =
        ShadowResolutionPowerOfTwo(settings.faceResolution, kMinShadowFaceResolution, kMaxShadowFaceResolution);
    // A face larger than the atlas it is cut from cannot be allocated at all,
    // and the allocator would have nothing sensible to demote it to.
    settings.faceResolution = std::min(settings.faceResolution, settings.atlasResolution);

    settings.depthBiasTexels = ClampFiniteShadow(settings.depthBiasTexels, kMinDepthBiasTexels, kMaxDepthBiasTexels,
                                                 defaults.depthBiasTexels);
    settings.slopeBias = ClampFiniteShadow(settings.slopeBias, kMinSlopeBias, kMaxSlopeBias, defaults.slopeBias);
    settings.normalOffsetTexels = ClampFiniteShadow(settings.normalOffsetTexels, kMinNormalOffsetTexels,
                                                    kMaxNormalOffsetTexels, defaults.normalOffsetTexels);
    return settings;
}

/// @brief Both halves, each sanitized on its own terms.
[[nodiscard]] inline ShadowSettings Sanitized(ShadowSettings settings)
{
    settings.sun = Sanitized(settings.sun);
    settings.local = Sanitized(settings.local);
    return settings;
}

/// @brief The knobs a named tier sets. Everything a tier does not name — the
/// biases, the blend band, the cull side — is left at the struct's default,
/// because those are correctness settings rather than quality ones.
///
/// An unrecognised tier gets Medium, which is also the default.
[[nodiscard]] inline ShadowSettings TierSettings(ShadowTier tier)
{
    ShadowSettings settings;
    switch (tier)
    {
    case ShadowTier::Low:
        // Fewest cascades of any tier, and deliberately so: every cascade is a
        // depth pass, and the machines this targets feel a submit before they
        // feel a seam. Its transitions are the most visible of the four, and
        // the blend band is the knob that costs nothing to widen.
        settings.sun.cascadeCount = 4;
        settings.sun.resolution = 1024;
        settings.sun.format = ShadowMapFormat::D16;
        settings.sun.maxDistance = 40.0f;
        settings.sun.filter = ShadowFilter::Point;
        settings.local.atlasResolution = 2048;
        settings.local.faceResolution = 256;
        settings.local.filter = ShadowFilter::Point;
        break;
    case ShadowTier::High:
        // Resolution rather than cascades. Both buy a smaller seam, but a
        // cascade is a re-submission of every caster and costs GPU time
        // proportional to the count, while the map's size is very nearly free —
        // measured, quadrupling the texels moved the depth pass by 0.3% and
        // each extra cascade moved it by about 0.09 ms. So this is a smaller
        // seam than six cascades at half the size, and cheaper.
        settings.sun.cascadeCount = 4;
        settings.sun.resolution = 4096;
        settings.sun.format = ShadowMapFormat::D32;
        settings.sun.maxDistance = 80.0f;
        settings.sun.filter = ShadowFilter::Pcf5x5;
        settings.local.atlasResolution = 4096;
        settings.local.faceResolution = 512;
        settings.local.filter = ShadowFilter::Pcf5x5;
        break;
    case ShadowTier::Ultra:
        // The only preset whose seams land at roughly one screen pixel at
        // 1080p, which is the point past which a cascade boundary stops being
        // something a display can resolve. Reaching further under that costs
        // another cascade at 4096, and the memory stops being defensible.
        settings.sun.cascadeCount = 6;
        settings.sun.resolution = 4096;
        settings.sun.format = ShadowMapFormat::D32;
        settings.sun.maxDistance = 100.0f;
        settings.sun.filter = ShadowFilter::Vogel;
        settings.local.atlasResolution = 8192;
        settings.local.faceResolution = 512;
        settings.local.filter = ShadowFilter::Vogel;
        break;
    case ShadowTier::Medium:
    case ShadowTier::Custom:
    default:
        break; // the structs' own defaults are Medium
    }
    return settings;
}

/// @brief Which tier @p settings match, or Custom.
///
/// Only the fields a tier sets are compared: a user who moves a bias has not
/// left the tier they picked, and a panel that said otherwise would flip to
/// Custom on a knob no tier has an opinion about.
[[nodiscard]] inline ShadowTier Tier(const ShadowSettings &settings)
{
    for (std::uint32_t i = 0; i < kShadowTierCount; ++i)
    {
        const auto candidate = static_cast<ShadowTier>(i);
        const ShadowSettings preset = TierSettings(candidate);
        const bool sunMatches = preset.sun.cascadeCount == settings.sun.cascadeCount &&
                                preset.sun.resolution == settings.sun.resolution &&
                                preset.sun.format == settings.sun.format &&
                                preset.sun.filter == settings.sun.filter &&
                                preset.sun.maxDistance == settings.sun.maxDistance;
        const bool localMatches = preset.local.atlasResolution == settings.local.atlasResolution &&
                                  preset.local.format == settings.local.format &&
                                  preset.local.faceResolution == settings.local.faceResolution &&
                                  preset.local.filter == settings.local.filter;
        if (sunMatches && localMatches)
        {
            return candidate;
        }
    }
    return ShadowTier::Custom;
}

/// @brief Bytes the cascade array occupies at these settings.
[[nodiscard]] inline std::uint64_t SunShadowMemoryBytes(const SunShadowSettings &settings)
{
    const SunShadowSettings safe = Sanitized(settings);
    if (!safe.enabled)
    {
        return 0;
    }
    const std::uint64_t bytesPerTexel = safe.format == ShadowMapFormat::D16 ? 2u : 4u;
    const std::uint64_t texels = static_cast<std::uint64_t>(safe.resolution) * safe.resolution * safe.cascadeCount;
    return texels * bytesPerTexel;
}

/// @brief Bytes the local-light atlas occupies at these settings. One texture
/// whatever the light count is — that is the point of an atlas.
[[nodiscard]] inline std::uint64_t LocalShadowMemoryBytes(const LocalShadowSettings &settings)
{
    const LocalShadowSettings safe = Sanitized(settings);
    if (!safe.enabled)
    {
        return 0;
    }
    const std::uint64_t bytesPerTexel = safe.format == ShadowMapFormat::D16 ? 2u : 4u;
    const std::uint64_t texels = static_cast<std::uint64_t>(safe.atlasResolution) * safe.atlasResolution;
    return texels * bytesPerTexel;
}

/// @brief Bytes every shadow map occupies at these settings. What the tier
/// table's memory column is, computed rather than quoted.
[[nodiscard]] inline std::uint64_t ShadowMemoryBytes(const ShadowSettings &settings)
{
    return SunShadowMemoryBytes(settings.sun) + LocalShadowMemoryBytes(settings.local);
}

} // namespace Assisi::Render
