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

/// @brief How far each filter's outermost tap sits from the centre, in taps.
///
/// A grid kernel's reach is what its name says: a 3x3 reaches one texel from its
/// centre, a 5x5 reaches two. The Vogel disk's radius is a choice rather than a
/// consequence, and 2.5 is what fills sixteen taps without leaving holes.
///
/// mesh.frag walks the same kernels and so carries the same three numbers. They
/// are here rather than only there because the CPU sizes two things from them —
/// the penumbra cap, and the inset that keeps an atlas lookup inside its own
/// tile — and a kernel wider than the inset assumed reads the light next door.
///
/// Point has none of its own: the hardware's bilinear comparison is the whole of
/// its footprint, and that half-texel is added separately wherever it matters.
inline constexpr float kPointFilterRadiusTaps = 0.f;
inline constexpr float kPcf3FilterRadiusTaps = 1.f;
inline constexpr float kPcf5FilterRadiusTaps = 2.f;
inline constexpr float kVogelFilterRadiusTaps = 2.5f;

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

/// @brief Bounds on how many local lights may hold atlas tiles at once.
///
/// The ceilings are the Ultra tier's, and they are backstops rather than
/// targets: the default sits well above what a sensible scene places, so the
/// cap does not bind while anyone is authoring. Zero is a real setting — it
/// means this light type never shadows — which is why the floor is not one.
inline constexpr std::uint32_t kMinShadowCap = 0;
inline constexpr std::uint32_t kMaxShadowCap = 256;

/// @brief How far a light's priority may be biased, in score octaves.
///
/// The score is a product of coverage, distance and intensity, so a bias that
/// added to it would mean something different at every range. This one doubles
/// or halves, which means the same thing everywhere: +1 is "treat this as twice
/// as important as its geometry says".
inline constexpr float kMinShadowPriority = -8.0f;
inline constexpr float kMaxShadowPriority = 8.0f;

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

/// @brief Bounds on how many atlas faces may be re-rendered in one frame.
///
/// The ceiling is above every tier's total face count — 64 spots and 16 points
/// is 160 — so setting it there is "never wait", which is what the A/B against
/// the uncached path wants. The floor is one rather than zero: a budget of zero
/// re-renders nothing ever, which is not a slower cache but a broken one.
inline constexpr std::uint32_t kMinShadowBakeBudget = 1;
inline constexpr std::uint32_t kMaxShadowBakeBudget = 256;

/// @brief Bounds on how long a caster must hold still before it rejoins the
/// cached layer.
///
/// The floor is one frame, which is no hysteresis at all: every pause in a
/// motion re-bakes. The ceiling is four seconds at 60 Hz, past which a thing
/// that stopped moving is still being drawn as though it might not have.
inline constexpr std::uint32_t kMinPromoteStillFrames = 1;
inline constexpr std::uint32_t kMaxPromoteStillFrames = 240;

/// @brief Bounds on the update-rate divisor a light's dynamic layer may be
/// throttled to. One is every frame; three is every third.
///
/// Past a third the lag is visible as a shadow trailing its object rather than
/// as a softer update, which is the point at which the technique stops buying
/// anything a smaller tile would not buy more honestly.
inline constexpr std::uint32_t kMinLightUpdateDivisor = 1;
inline constexpr std::uint32_t kMaxLightUpdateDivisor = 3;

/// @brief Keeping a resting light's shadow instead of redrawing it.
///
/// A tile's depth is two layers: what the still geometry recorded, which changes
/// only when that geometry does, and what the moving geometry records, which
/// changes every frame. Cached, a tile costs a copy of the first plus a draw of
/// the second — and a light with nothing moving under it costs nothing at all,
/// because its tile already holds the right depth from whenever it was last
/// drawn.
///
/// Which casters are "still" is inferred rather than authored: Transform carries
/// a change tick, so a caster that has not been written is one that has not
/// moved. That is a fact the engine already has, and it cannot be set wrong the
/// way a mobility flag on a prefab can.
struct LocalShadowCacheSettings
{
    /// Whether tiles are cached at all. Off is the uncached path exactly: the
    /// whole atlas is cleared and every face of every served light is redrawn,
    /// which is the baseline every measurement here is quoted against.
    bool enabled = true;

    /// Faces that may be re-rendered in one frame, most important first.
    ///
    /// Distinct from the tile cap: a tile *held* costs memory, a face *drawn*
    /// costs time. This is what stops a burst — walking into a new room, a door
    /// opening onto a dozen lamps — landing as one long frame. A light whose
    /// face does not fit this frame's budget goes unshadowed until it does,
    /// never shadowed from a stale tile: a light waiting its turn is one not yet
    /// drawn, and that is a dimmer image rather than a wrong one.
    std::uint32_t updateBudgetFaces = 32;

    /// Frames a caster must hold still before it is folded back into the cached
    /// layer.
    ///
    /// A caster's first moved frame drops it out of the cached layer of every
    /// tile it touches — one re-bake — and it draws with the movers until it
    /// settles, at which point it is folded back in with one more. So a motion
    /// episode costs two re-bakes however long it lasts, and standing still
    /// costs none. Without the wait, a caster that pauses mid-motion re-bakes
    /// every tile around it and then immediately undoes that.
    std::uint32_t promoteStillFrames = 30;

    /// The slowest rate a light's moving layer may be redrawn at: 1 is every
    /// frame, 2 every other, 3 every third.
    ///
    /// A ceiling rather than a rate. The most important lights of a frame always
    /// redraw every frame; the divisor is spent on the ones further down the
    /// ordering, where a shadow one frame behind its object is not what anyone
    /// is looking at. One — the default — throttles nothing, so this costs
    /// nothing until it is turned up.
    std::uint32_t movingLightUpdateDivisor = 1;
};

/// @brief Spot and point lights' shadows, as the user edits them.
///
/// These are the knobs that decide what is *allocated*: how big the shared
/// atlas is, what a light's face gets out of it, and how a lookup into it is
/// filtered and biased. Which lights win those tiles when there are more of them
/// than there is atlas is a separate question, and it is answered by
/// LocalShadowSelectionSettings below.
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

    /// What a resting light is allowed to skip. Nested here rather than beside
    /// the selection knobs because it is a property of the atlas itself — it
    /// doubles the atlas's memory and changes what a tile means.
    LocalShadowCacheSettings cache;
};

/// @brief Which local lights hold atlas tiles when more of them want one than
/// the atlas can serve.
///
/// Separate from LocalShadowSettings because it answers a different question:
/// those knobs size what exists, these order who gets it. A scene under no
/// pressure never reaches any of this — every shadowed light is served and the
/// ordering never decides anything.
///
/// The caps are per type rather than one number, and that is design rather than
/// generosity. A point light is six shadow renders against a spot's one, so a
/// single cap of "eight lights" means eight renders or forty-eight depending on
/// what happens to be placed. Split by type, the setting means the same thing
/// whatever the mix.
struct LocalShadowSelectionSettings
{
    /// Whether importance decides at all.
    ///
    /// Off does not lift the physical limit — the atlas is a fixed-size texture
    /// and fills either way. What it removes is the *ordering*: lights are
    /// served until the allocator is full and the rest go unshadowed in arrival
    /// order rather than by what they contribute. Strictly worse under pressure,
    /// which is why it is not the default, and it is still the author's call.
    bool capEnabled = true;

    /// Max simultaneously shadowed spot lights. One face each.
    std::uint32_t capSpot = 16;
    /// Max simultaneously shadowed point lights. Six faces each, which is why
    /// this is about a quarter of the spot cap at every tier.
    std::uint32_t capPoint = 4;

    /// How much better a challenger's score must be before it takes a tile from
    /// a light that already holds one, as a fraction.
    ///
    /// Without it two lights whose scores cross keep swapping every frame, and
    /// swapping is visible: the loser's shadow disappears. The margin costs
    /// nothing but a slightly stale ordering, and staleness in this ordering is
    /// imperceptible where flicker is not.
    float capHysteresis = 0.15f;

    /// How far demand must pass the next size class before a light's tile is
    /// resized, as a fraction.
    ///
    /// A resize invalidates the tile, so reassigning classes every frame the way
    /// a purely demand-driven allocator would thrashes whatever the tile held.
    /// Tile resolution is the cheap axis — quadrupling the texels moved the
    /// depth pass by a fraction of a percent — so this is a quality-distribution
    /// control and there is nothing to be gained by letting it chase.
    float classHysteresis = 0.25f;
};

/// @brief Every shadow knob: the two quality halves, and who wins a tile.
struct ShadowSettings
{
    SunShadowSettings sun;
    LocalShadowSettings local;
    LocalShadowSelectionSettings selection;
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

    settings.maxDistance =
        ClampFiniteShadow(settings.maxDistance, kMinShadowDistance, kMaxShadowDistance, defaults.maxDistance);
    settings.splitLambda =
        ClampFiniteShadow(settings.splitLambda, kMinSplitLambda, kMaxSplitLambda, defaults.splitLambda);
    settings.depthBiasTexels =
        ClampFiniteShadow(settings.depthBiasTexels, kMinDepthBiasTexels, kMaxDepthBiasTexels, defaults.depthBiasTexels);
    settings.slopeBias = ClampFiniteShadow(settings.slopeBias, kMinSlopeBias, kMaxSlopeBias, defaults.slopeBias);
    settings.normalOffsetTexels = ClampFiniteShadow(settings.normalOffsetTexels, kMinNormalOffsetTexels,
                                                    kMaxNormalOffsetTexels, defaults.normalOffsetTexels);
    settings.cascadeBlend =
        ClampFiniteShadow(settings.cascadeBlend, kMinCascadeBlend, kMaxCascadeBlend, defaults.cascadeBlend);
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

    settings.depthBiasTexels =
        ClampFiniteShadow(settings.depthBiasTexels, kMinDepthBiasTexels, kMaxDepthBiasTexels, defaults.depthBiasTexels);
    settings.slopeBias = ClampFiniteShadow(settings.slopeBias, kMinSlopeBias, kMaxSlopeBias, defaults.slopeBias);
    settings.normalOffsetTexels = ClampFiniteShadow(settings.normalOffsetTexels, kMinNormalOffsetTexels,
                                                    kMaxNormalOffsetTexels, defaults.normalOffsetTexels);

    settings.cache.updateBudgetFaces =
        std::clamp(settings.cache.updateBudgetFaces, kMinShadowBakeBudget, kMaxShadowBakeBudget);
    settings.cache.promoteStillFrames =
        std::clamp(settings.cache.promoteStillFrames, kMinPromoteStillFrames, kMaxPromoteStillFrames);
    settings.cache.movingLightUpdateDivisor =
        std::clamp(settings.cache.movingLightUpdateDivisor, kMinLightUpdateDivisor, kMaxLightUpdateDivisor);
    return settings;
}

/// @brief The selection knobs, on the same terms.
[[nodiscard]] inline LocalShadowSelectionSettings Sanitized(LocalShadowSelectionSettings settings)
{
    const LocalShadowSelectionSettings defaults;

    settings.capSpot = std::clamp(settings.capSpot, kMinShadowCap, kMaxShadowCap);
    settings.capPoint = std::clamp(settings.capPoint, kMinShadowCap, kMaxShadowCap);
    // Both fractions are bounded above by 1: a margin of a whole score would
    // mean a challenger has to be twice the holder to displace it, which is no
    // longer hysteresis but a different policy.
    settings.capHysteresis = ClampFiniteShadow(settings.capHysteresis, 0.f, 1.f, defaults.capHysteresis);
    settings.classHysteresis = ClampFiniteShadow(settings.classHysteresis, 0.f, 1.f, defaults.classHysteresis);
    return settings;
}

/// @brief Every half, each sanitized on its own terms.
[[nodiscard]] inline ShadowSettings Sanitized(ShadowSettings settings)
{
    settings.sun = Sanitized(settings.sun);
    settings.local = Sanitized(settings.local);
    settings.selection = Sanitized(settings.selection);
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
        settings.selection.capSpot = 8;
        settings.selection.capPoint = 2;
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
        settings.selection.capSpot = 32;
        settings.selection.capPoint = 8;
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
        settings.selection.capSpot = 64;
        settings.selection.capPoint = 16;
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
                                preset.sun.format == settings.sun.format && preset.sun.filter == settings.sun.filter &&
                                preset.sun.maxDistance == settings.sun.maxDistance;
        const bool localMatches = preset.local.atlasResolution == settings.local.atlasResolution &&
                                  preset.local.format == settings.local.format &&
                                  preset.local.faceResolution == settings.local.faceResolution &&
                                  preset.local.filter == settings.local.filter;
        // Compared because a tier sets them, on the same rule as everything else
        // here: a preset's own fields decide whether the settings are that
        // preset. Lowering the cap alone is leaving the tier — it changes how
        // many lights shadow, which is exactly what a tier is a statement about.
        const bool capsMatch = preset.selection.capSpot == settings.selection.capSpot &&
                               preset.selection.capPoint == settings.selection.capPoint;
        if (sunMatches && localMatches && capsMatch)
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
///
/// Two textures when tiles are cached: the still geometry's depth is kept in its
/// own atlas so a tile can be composed from it without having been redrawn, and
/// that second copy is the whole price of the cache. It is the same shape as the
/// first, so caching exactly doubles this figure.
[[nodiscard]] inline std::uint64_t LocalShadowMemoryBytes(const LocalShadowSettings &settings)
{
    const LocalShadowSettings safe = Sanitized(settings);
    if (!safe.enabled)
    {
        return 0;
    }
    const std::uint64_t bytesPerTexel = safe.format == ShadowMapFormat::D16 ? 2u : 4u;
    const std::uint64_t texels = static_cast<std::uint64_t>(safe.atlasResolution) * safe.atlasResolution;
    return texels * bytesPerTexel * (safe.cache.enabled ? 2u : 1u);
}

/// @brief Bytes every shadow map occupies at these settings. What the tier
/// table's memory column is, computed rather than quoted.
[[nodiscard]] inline std::uint64_t ShadowMemoryBytes(const ShadowSettings &settings)
{
    return SunShadowMemoryBytes(settings.sun) + LocalShadowMemoryBytes(settings.local);
}

} // namespace Assisi::Render
