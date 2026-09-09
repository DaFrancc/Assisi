/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ShadowCadence.hpp
/// @brief Which of the sun's cascades still hold the right depth, and which have
/// to be drawn again.
///
/// With a fixed sun, a still camera and a still scene, every cascade produces
/// bitwise-identical contents every frame. This is what stops that. A cascade is
/// kept when its fit has not moved, the sun has not turned far enough to matter
/// at its texel size, and nothing has moved inside it — and a kept cascade costs
/// no gather, no cull, no instance build and no upload, which is where the
/// saving is. Drawing a cascade is a fraction of a millisecond; deciding what
/// goes into it means walking every caster in the scene.
///
/// **Kept whole or drawn whole.** There is no page table, no toroidal
/// addressing and no partial-region bookkeeping: a cascade holds one frame's
/// depth or it is redrawn from scratch. That is the difference between this and
/// the virtual-shadow-map form, which is rejected.
///
/// Two rules run through all of it, and they are the same two the local-light
/// atlas keeps:
///
///   * **Iterate what changed, never what exists.** Invalidation walks the
///     casters that moved against the cascades — a handful against at most
///     eight — and never the casters against the cascades. A frame in which
///     nothing moved does no work at all, which is the whole claim.
///   * **When it is ambiguous, redraw.** A caster moving for the first time has
///     no recorded pose, so nothing here knows which cascades were drawn holding
///     it; every cascade is dirtied rather than guessing. That is one frame of
///     today's cost, once per caster, against a shadow left behind by an object
///     that walked away from it.
///
/// Nothing here draws or allocates. ShadowPass owns the cascade array and draws
/// into it; this says which slices of it are worth drawing.

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>

#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Math/Angles.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/ShadowCascades.hpp>
#include <Assisi/Render/ShadowSettings.hpp>

namespace Assisi::Render
{

/// @brief One frame, as the cadence needs to see it.
///
/// The candidate fit is passed separately, because it is the one input that is
/// compared field by field rather than merely read.
struct SunShadowCadenceFrame
{
    /// Counts frames, and only has to advance by one per frame and never wrap
    /// during a session. Drives the ages the panel reports.
    std::uint32_t frameIndex = 0;

    SunShadowCadenceSettings settings;

    /// The direction the sun's light travels this frame. Compared against the
    /// direction each kept cascade was drawn at, which is what the drift term
    /// measures.
    glm::vec3 lightDirection{0.f, -1.f, 0.f};

    /// Casters that moved this frame, and where they now stand. Empty on a still
    /// frame, and that is the case the whole design is for.
    std::span<const ShadowMover> movers;
};

/// @brief What the frame draws, and what it publishes to the shader.
struct SunShadowCadencePlan
{
    /// The fit this frame renders with **and samples with**. A kept cascade
    /// carries the fit it was drawn at rather than this frame's candidate: the
    /// depth in the slice was rasterized with that matrix, and sampling it with
    /// a newer one would slide every shadow by the difference.
    CascadeFit fit;

    /// Cascades to clear and draw, ascending. Everything not named here keeps
    /// what it holds.
    std::array<std::uint32_t, kMaxShadowCascades> redraw{};
    std::uint32_t redrawCount = 0;

    /// Frames since each cascade's depth was drawn. Zero for one drawn this
    /// frame; a large number is a cascade that has been paying nothing for a
    /// long time, which is what this is for — and, on a cascade something is
    /// visibly walking through, the shape a missed invalidation takes on screen.
    std::array<std::uint32_t, kMaxShadowCascades> ageFrames{};

    /// @brief Whether cascade @p index is drawn this frame.
    [[nodiscard]] bool IsRedrawn(std::uint32_t index) const
    {
        for (std::uint32_t i = 0; i < redrawCount; ++i)
        {
            if (redraw[i] == index)
            {
                return true;
            }
        }
        return false;
    }
};

/// @brief What one frame's planning came to, for the panel and the gates.
struct SunShadowCadenceStats
{
    /// Cascades drawn this frame, and cascades that kept what they held. On a
    /// still scene with a fixed sun the first is zero after the first frame,
    /// which is the reading the pay-for-what-you-place gate is taken from.
    std::uint32_t redrawn = 0;
    std::uint32_t kept = 0;

    /// Cascades a caster's motion dirtied, as against ones the fit or the sun
    /// moved out from under. The two are different problems: motion is content
    /// and is answered by the scene, drift is the camera and the day cycle and
    /// is answered by the knob.
    std::uint32_t dirtiedByMotion = 0;

    /// Movers with no recorded pose, each of which dirtied every cascade. A
    /// steady stream of these is a scene whose casters keep leaving the shadow
    /// distance and coming back, which costs a full redraw every time.
    std::uint32_t unrecordedMovers = 0;
};

/// @brief Which cascades hold usable depth, and what invalidates it.
class SunShadowCadence
{
public:
    SunShadowCadence() = default;

    /// @brief Decide what @p candidate's cascades need this frame.
    ///
    /// @p out.fit is what the frame must both draw with and hand the shader:
    /// kept cascades carry the fit their depth was drawn at, redrawn ones carry
    /// @p candidate's.
    ///
    /// The movers are walked against the cascades and against nothing else, so
    /// this costs movers times cascades — never casters times cascades, and
    /// nothing at all on a frame in which nothing moved.
    ///
    /// Recording happens here: every cascade named in @p out.redraw is taken to
    /// have been drawn this frame, so the caller must draw all of them.
    void Plan(const SunShadowCadenceFrame &frame, const CascadeFit &candidate, SunShadowCadencePlan &out);

    /// @brief Forget every cascade, so the next Plan draws all of them.
    ///
    /// What a reallocation wants — the slices hold depth of a texture that no
    /// longer exists — and what a discontinuous jump wants: a time skip, a
    /// cutscene or a level load invalidates everything and pays one frame of
    /// today's cost, which is exactly where a spike is invisible.
    void Forget();

    /// @brief What the last Plan() decided.
    [[nodiscard]] const SunShadowCadenceStats &Stats() const { return _stats; }

private:
    /// @brief One slice's depth: the fit it was drawn at, and when.
    struct Slice
    {
        ShadowCascade cascade;
        /// The direction the light travelled when this depth was rasterized.
        glm::vec3 lightDirection{0.f, -1.f, 0.f};
        std::uint32_t drawFrame = 0;
        bool valid = false;
    };

    /// @brief Whether @p slice still describes what @p candidate asks for.
    [[nodiscard]] static bool WithinTolerance(const Slice &slice, const ShadowCascade &candidate,
                                              const glm::vec3 &lightDirection, float driftTexels);

    std::array<Slice, kMaxShadowCascades> _slices{};

    /// @brief Where a cascade's depth holds one caster, and when it last moved.
    struct Record
    {
        Geometry::BoundingSphere sphere;
        /// The frame it was last written on. An entry is kept while it is still
        /// moving even once it is past every cascade, so a caster wandering out
        /// of the shadow distance does not read as unrecorded on its way back.
        std::uint32_t moveFrame = 0;
    };

    /// Where the depth holds each caster that has moved. Consulted so a caster
    /// leaving a cascade dirties the cascade it left as well as the one it
    /// entered — testing only where it now stands leaves its shadow behind it,
    /// with no visual tell until someone notices.
    ///
    /// An entry lives while some cascade's volume still contains the pose or the
    /// caster is still moving, so this is bounded by the movers within the shadow
    /// distance rather than by every caster that has ever moved.
    std::unordered_map<std::uint64_t, Record> _drawnPose;

    SunShadowCadenceStats _stats;
};

} // namespace Assisi::Render
