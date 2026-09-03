/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ShadowImportance.hpp
/// @brief Which local lights get atlas tiles, and how big.
///
/// The atlas is one fixed texture and a level may place hundreds of shadowed
/// lights, so something has to order them. This is that ordering, and it is a
/// backstop rather than a budget: the default caps sit above what a sensible
/// scene places, so on ordinary content nothing here decides anything — every
/// shadowed light is served and the selection is the identity.
///
/// Two decisions live here, and they are separate:
///
///   * **Who is served.** A score per light, and the top N of each type hold
///     tiles. Per type because a point light is six renders against a spot's
///     one, so one combined number would mean six times the work depending on
///     what happened to be placed.
///   * **How big a tile.** A light covering a sliver of the screen cannot show
///     what a 512-texel map holds, so it takes a smaller class and leaves the
///     texels for one that can. Demand only ever demotes — the tier's face
///     resolution is a ceiling, because it is also what the tier's memory figure
///     was computed from.
///
/// Both are hysteretic, and for the same reason: a light that gains and loses
/// its shadow every other frame is worse than one that never had it, and a tile
/// that changes size is a tile whose contents are thrown away. Neither margin
/// costs anything but a slightly stale answer, and staleness is invisible where
/// flicker is not.
///
/// Device-free, like the allocator it feeds.

#include <cstdint>
#include <span>
#include <vector>

#include <Assisi/Render/ShadowSettings.hpp>

namespace Assisi::Render
{

/// @brief Which of the two local light types a record is about.
///
/// Spot and point are ordered and capped separately throughout, so this rides
/// with every record rather than being implied by which array it came from.
enum class LocalLightKind : std::uint8_t
{
    Spot = 0,
    Point = 1,
};

/// @brief How many atlas faces one light of each kind needs.
///
/// A spot's cone is a single frustum. A point light radiates in every direction
/// and takes a cube's worth, which is the whole reason the two are capped apart.
[[nodiscard]] constexpr std::uint32_t LocalShadowFaceCount(LocalLightKind kind)
{
    return kind == LocalLightKind::Point ? 6u : 1u;
}

/// @brief The screen coverage at which a light earns the full face resolution.
///
/// A light whose influence sphere spans half the screen's height is filling the
/// view, and that is what the tier's face class was sized for. Every halving
/// below this drops one class, which keeps a map's texels roughly matched to the
/// screen pixels that will ever read them.
inline constexpr float kFaceCoverageReference = 0.5f;

/// @brief One local light, as the selector needs to see it.
///
/// Deliberately not the GPU light struct: what is scored is a handful of scalars
/// the caller already has, and taking them by value is what keeps this testable
/// without a scene, a camera or a device.
struct LocalShadowCandidate
{
    LocalLightKind kind = LocalLightKind::Spot;

    /// Row in this kind's GPU light buffer. Carried through untouched, so the
    /// selection names lights in the terms the shader already reads them in.
    std::uint32_t lightIndex = 0;

    /// Fraction of the screen's height this light's influence sphere spans, in
    /// [0, 1]. A light the camera stands inside saturates at 1.
    ///
    /// This is where distance enters the score: coverage is the light's reach
    /// over its range, so a light twice as far away covers half as much and
    /// scores a quarter. Passing it in rather than deriving it here is what
    /// keeps the camera out of this file.
    float screenCoverage = 0.f;

    /// The light's authored intensity. Its magnitude is what counts — a
    /// subtractive light occludes exactly as much as an additive one, and its
    /// shadow is as visible.
    float intensity = 0.f;

    /// Octaves of bias on the score. +1 is "twice as important as the geometry
    /// says", -1 half. Zero leaves the light scored on its own terms.
    float priority = 0.f;

    /// Never dropped by the cap, whatever it scores. For a key light whose
    /// shadow is the shot — something no bias can guarantee, because what a bias
    /// has to beat depends on what else the level places.
    bool pinned = false;

    /// Whether the camera can see anything this light lights. A light behind the
    /// camera is not scored at all: it would hold a tile against lights that are
    /// on screen.
    bool visible = true;
};

/// @brief One light that won a tile, and what size it takes.
struct LocalShadowAssignment
{
    LocalLightKind kind = LocalLightKind::Spot;
    std::uint32_t lightIndex = 0;
    /// What it scored, before the holder's margin. Reported so a diagnostic can
    /// say why one light kept its shadow and its neighbour did not.
    float score = 0.f;
    std::uint32_t sizeClass = 0;
};

/// @brief What one frame's selection came to.
struct LocalShadowSelection
{
    /// The winners, most important first, spot and point interleaved in that one
    /// order. Sorted because the allocator serves them in it: the atlas fills
    /// from the top, so a light that overflows is by construction one of the
    /// least important, and demotion needs no separate pass.
    std::vector<LocalShadowAssignment> lights;

    std::uint32_t spotCount = 0;
    std::uint32_t pointCount = 0;

    /// Candidates the cap turned away. Zero is the ordinary reading, and it is
    /// what "the cap does not bind on the reference scene" looks like as a
    /// number rather than an impression.
    std::uint32_t droppedByCap = 0;

    void Clear();
};

/// @brief What a light contributes to the image, as one number.
///
/// The product of how much of the view it fills and how bright it is. Coverage
/// is squared because it is quoted as a length across the screen and what
/// matters is the area — a light covering half the height covers a quarter of
/// the image, and is a quarter as worth a shadow map.
///
/// Non-finite inputs score zero rather than propagating: a NaN here would sort
/// unpredictably against everything and take an arbitrary light's shadow with it.
[[nodiscard]] float LocalShadowScore(const LocalShadowCandidate &candidate);

/// @brief The sentinel for "this light held no tile last frame", which takes the
/// demand outright rather than resisting a change from nothing.
inline constexpr std::uint32_t kNoPreviousSizeClass = 0xFFFFFFFFu;

/// @brief The size class @p coverage calls for, capped at @p baseClass and
/// resisted by @p previousClass.
///
/// Demand alone would reassign every frame the camera moves, and a resize throws
/// the tile's contents away — so a change has to clear the boundary by
/// @p hysteresis before it is taken. The margin applies in both directions, so a
/// light hovering on a boundary stays where it is rather than alternating.
///
/// @p baseClass is a ceiling and never a floor: a light close enough to want
/// more than the tier's face resolution does not get it, because that resolution
/// is what the tier's memory figure was computed from.
[[nodiscard]] std::uint32_t StableSizeClass(std::uint32_t baseClass, float coverage, std::uint32_t previousClass,
                                            float hysteresis);

/// @brief Scores local lights and picks which of them hold atlas tiles.
///
/// Holds the previous frame's answer, which is the whole of its state and the
/// only reason it is an object rather than a function. Lights are remembered by
/// their buffer index, so adding one mid-scene shifts the rest and costs a single
/// frame of forgotten hysteresis — on a frame where the scene changed anyway.
class LocalShadowSelector
{
public:
    LocalShadowSelector() = default;

    /// @brief Choose this frame's tile holders from @p candidates.
    ///
    /// With the cap off, the ordering is dropped rather than the limit: every
    /// visible shadowed light is a winner, in arrival order, and the allocator
    /// serves them until the atlas is full. That is strictly worse under
    /// pressure — the lights that go without are whichever came last rather than
    /// whichever matter least — which is why it is not the default.
    void Select(std::span<const LocalShadowCandidate> candidates, const LocalShadowSettings &local,
                const LocalShadowSelectionSettings &selection, LocalShadowSelection &out);

    /// @brief Forget the previous frame, so the next Select takes its demand
    /// outright. What a settings change or a level load wants — the remembered
    /// classes were sized against an atlas that no longer exists.
    void Forget();

private:
    /// @brief One candidate as the ordering sees it: what it will become, plus
    /// the two things that decide where it lands and nothing downstream reads.
    struct Ranked
    {
        LocalShadowAssignment assignment;
        /// The score with the holder's margin already in it. Separate from the
        /// assignment's own score because the assignment reports what the light
        /// is worth, and this is what it is worth *against a challenger*.
        float effectiveScore = 0.f;
        bool pinned = false;
    };

    /// @brief The class @p kind's light @p index held last frame, or
    /// kNoPreviousSizeClass.
    [[nodiscard]] std::uint32_t PreviousClass(LocalLightKind kind, std::uint32_t index) const;

    // Last frame's winners, in the same shape as the answer, so a lookup is a
    // walk over what is usually a handful of entries. Kept across frames, which
    // is what both margins are measured against.
    std::vector<LocalShadowAssignment> _previous;
    // Scratch for the ordering, kept so a steady state allocates nothing.
    std::vector<Ranked> _ranked;
};

} // namespace Assisi::Render
