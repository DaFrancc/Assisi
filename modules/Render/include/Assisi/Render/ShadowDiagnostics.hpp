/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ShadowDiagnostics.hpp
/// @brief What the shadow system did to each light this frame, in the terms an
/// author can act on.
///
/// Every mechanism that bounds shadow cost — the importance cap, the atlas
/// allocator, the size-class demotion, the redraw budget — makes a light look
/// different without saying so. Four lights losing their shadows reads on screen
/// as "some of my lamps look wrong", and which four changes as the camera moves,
/// so the natural conclusion is that the engine is broken. This is the readout
/// that turns that into a sentence naming the setting to reach for.
///
/// One state per shadow-casting local light, and they are deliberately distinct
/// rather than one "no shadow" flag: they are four different settings. A light
/// the cap turned away wants a bigger cap; one the atlas could not fit wants a
/// bigger atlas; one the budget refused wants a bigger budget; one that was
/// demoted has its shadow, only coarser.
///
/// Device-free, like the selection and the allocator it reports on. Nothing here
/// is gathered unless something is looking: the frame builds this only when a
/// panel is open, which is what keeps a closed panel free.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <Assisi/Render/LocalShadowCache.hpp>
#include <Assisi/Render/ShadowImportance.hpp>
#include <Assisi/Render/ShadowSettings.hpp>

namespace Assisi::Render
{

/// @brief What became of one shadow-casting local light this frame.
///
/// Ordered worst-to-best is deliberately *not* the order: these are causes, not
/// severities, and a panel that sorted them would imply a ranking the engine
/// does not have.
enum class LocalShadowState : std::uint8_t
{
    /// Holds the tile it asked for. The ordinary reading, and what every light
    /// on sensible content reports.
    Shadowed,
    /// Holds a tile, but a smaller one than it asked for: the atlas had no
    /// rectangle of the class it wanted. Its shadow is coarser, not absent.
    Demoted,
    /// The redraw budget did not reach it this frame, so it lights unshadowed
    /// rather than out of a tile that no longer describes the scene. Transient
    /// by construction — a burst condition, and only a defect when it persists.
    Deferred,
    /// Selected, but the atlas had no room for it even at the smallest class.
    AtlasFull,
    /// The importance cap turned it away before the atlas was asked.
    CappedOut,
    /// Local shadows are switched off entirely, so nothing below decided
    /// anything. Distinct from every state above, which are all verdicts — this
    /// one says no verdict was reached.
    Disabled,
};

/// @brief What happened to one light, and what it would take to change it.
struct LocalShadowLightReport
{
    LocalLightKind kind = LocalLightKind::Spot;
    /// Row in this kind's GPU light buffer — the same name the selection, the
    /// cache and the shader all know the light by.
    std::uint32_t lightIndex = 0;

    LocalShadowState state = LocalShadowState::CappedOut;

    /// What the light scored, so a panel can say why this one kept its shadow
    /// and its neighbour did not. Zero for a light that was never scored.
    float score = 0.f;

    /// The tile edge it holds, in texels, and zero when it holds none.
    std::uint32_t resolution = 0;
    /// The tile edge it asked for. Above @ref resolution exactly when the atlas
    /// demoted it.
    std::uint32_t requestedResolution = 0;
};

/// @brief One frame of shadow diagnostics, whole.
struct ShadowDiagnostics
{
    /// One entry per shadow-casting local light, candidates the cap turned away
    /// included. Empty while nothing is looking.
    std::vector<LocalShadowLightReport> lights;

    /// Lights holding a tile this frame, against lights that asked for one —
    /// the "N of M" reading. A gap between them is the whole subject of this
    /// file, and the reports say which mechanism opened it.
    std::uint32_t shadowedLights = 0;
    std::uint32_t castingLights = 0;

    /// Faces the redraw budget refused this frame, and the budget it refused
    /// them against. Saturation is a burst condition and expected on the frame a
    /// door opens; sustained saturation is content that has outrun the budget,
    /// and would otherwise show up only as an unexplained hitch.
    std::uint32_t deferredFaces = 0;
    std::uint32_t budgetFaces = 0;
    [[nodiscard]] bool BudgetSaturated() const { return deferredFaces > 0; }

    /// Casters each sun cascade drew, nearest cascade first. Summed over the
    /// cascades this is the sun's whole draw cost, and the split is what says
    /// whether it is the near detail or the far distance paying for it.
    std::array<std::uint32_t, kMaxShadowCascades> cascadeCasters{};
    std::uint32_t cascadeCount = 0;

    void Clear();

    /// @brief The report for one light, or null when it casts no shadow or
    /// nothing gathered this frame.
    [[nodiscard]] const LocalShadowLightReport *Find(LocalLightKind kind, std::uint32_t lightIndex) const;
};

/// @brief Everything one frame's local-light report is built from.
///
/// Every span comes straight off the frame that has just been rendered, and they
/// are meaningless apart: the plans and the requests are index-parallel, and a
/// served tile names a request by its index into that same span.
struct LocalShadowDiagnosticsFrame
{
    /// Every shadow-casting local light that was scored — the M of "N of M".
    std::span<const LocalShadowCandidate> candidates;

    /// The lights the selection kept, in importance order.
    std::span<const LocalShadowRequest> requests;
    /// What the cache decided for each of them, index-parallel to @ref requests.
    /// May be empty, which is a frame where the cache is off and nothing was
    /// deferred.
    std::span<const LocalShadowTilePlan> plans;

    /// The requests that ended the frame holding tiles, and the edge each got.
    std::span<const LocalShadowServedTile> served;

    /// Faces the budget refused, and the budget itself. Carried rather than
    /// re-derived from the plans, because the plans are what the budget was
    /// spent against and counting them again would be a second answer.
    std::uint32_t deferredFaces = 0;
    std::uint32_t budgetFaces = 0;

    /// Whether the local-light shadow pass ran at all. False makes every
    /// candidate Disabled rather than letting them fall through to a verdict:
    /// with no selection made, an absent light would otherwise read as one the
    /// cap turned away, which blames a mechanism that never ran.
    ///
    /// The lights are still counted. How many want a shadow is a fact about the
    /// content, not about whether the feature is switched on, and a readout that
    /// said "0 of 0" over a level full of lamps would be worse than silent.
    bool active = true;
};

/// @brief Join a frame's candidates, requests, plans and tiles into one report
/// per light.
///
/// A candidate absent from @p frame.requests was turned away by the cap. A
/// request with no served tile was refused by the budget if its plan says so and
/// by the atlas otherwise — the distinction the whole readout exists to make.
///
/// @p out is cleared and refilled, so a steady state allocates nothing.
void BuildLocalShadowDiagnostics(const LocalShadowDiagnosticsFrame &frame, ShadowDiagnostics &out);

/// @brief One word for @p state, for a table column.
[[nodiscard]] const char *LocalShadowStateName(LocalShadowState state);

/// @brief A one-line description of @p state, for a tooltip or an inspector.
///
/// Says what happened and which knob answers it, because a state name alone
/// tells an author that something is wrong without telling them what to do.
[[nodiscard]] const char *DescribeLocalShadowState(LocalShadowState state);

} // namespace Assisi::Render
