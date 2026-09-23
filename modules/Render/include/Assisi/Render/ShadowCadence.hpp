/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ShadowCadence.hpp
/// @brief Which of the sun's cascades still hold the right depth, and which have
/// to be drawn again.
///
/// With a fixed sun, a still camera and a still scene, every cascade produces
/// bitwise-identical contents every frame. This is what stops that.
///
/// **Two layers per cascade**: the still casters' depth alone, and the slice
/// the shader reads, which is that depth with the moving casters drawn over it.
/// A still layer is kept while its fit has not moved, the sun has not turned
/// far enough to matter at its texel size, and no caster has joined or left it
/// — and a kept layer costs no gather, no cull, no instance build and no
/// upload, which is where the saving is. Objects moving about do not touch it:
/// when they stand somewhere new, the texels they covered are put back from it
/// and they are drawn again, and the shader never knows there are two.
///
/// **Kept whole or drawn whole.** There is no page table, no toroidal
/// addressing and no partial-region bookkeeping: a layer holds one frame's
/// depth or it is redrawn from scratch. That is the difference between this and
/// the virtual-shadow-map form, which is rejected.
///
/// **Iterate what changed, never what exists.** Invalidation walks the casters
/// that changed sides, and the moving layers the casters that move, against
/// the cascades — a handful against at most eight — and never every caster
/// against the cascades. A frame in which nothing moved does no work at all.
///
/// Which casters move, and when one changes sides, is ShadowCasterMobility's
/// answer, shared with the local-light atlas so the two can never disagree
/// about which layer an object is in.
///
/// Nothing here draws or allocates. ShadowPass owns the cascade array and draws
/// into it; this says which slices of it are worth drawing.

#include <array>
#include <cstdint>
#include <span>

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

    /// Casters drawn into the moving layers this frame, and where they stand
    /// (ShadowCasterMobility::Dynamic). They never touch a still layer, so
    /// their motion dirties nothing there: it only decides which moving layers
    /// have to be redrawn.
    std::span<const ShadowMover> dynamic;

    /// Casters that changed sides this frame, each with a sphere covering the
    /// pose a still layer holds it at and the pose it now takes. The one kind
    /// of motion a still layer has to answer to: a caster leaving it must be
    /// erased from it, and one joining must be drawn into it.
    std::span<const ShadowMover> invalidations;
};

/// @brief What the frame draws, and what it publishes to the shader.
struct SunShadowCadencePlan
{
    /// The fit this frame renders with **and samples with**. A kept cascade
    /// carries the fit it was drawn at rather than this frame's candidate: the
    /// depth in the slice was rasterized with that matrix, and sampling it with
    /// a newer one would slide every shadow by the difference.
    CascadeFit fit;

    /// Cascades whose still layer is cleared and drawn, ascending. Everything
    /// not named here keeps what it holds.
    std::array<std::uint32_t, kMaxShadowCascades> redraw{};

    /// Cascades whose movers are drawn again, ascending: the ones they covered
    /// last time put back from the still layer, and this frame's drawn over
    /// it. Includes a cascade whose movers have all gone, which is the put-back
    /// alone.
    std::array<std::uint32_t, kMaxShadowCascades> movingRedraw{};
    std::uint32_t redrawCount = 0;
    std::uint32_t movingRedrawCount = 0;

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

    /// Still layers a caster changing sides dirtied, as against ones the fit or
    /// the sun moved out from under. The two are different problems: motion is
    /// content and is answered by the scene, drift is the camera and the day
    /// cycle and is answered by the knob.
    std::uint32_t dirtiedByMotion = 0;

    /// Moving layers redrawn this frame, and ones whose movers stood where the
    /// last redraw drew them.
    std::uint32_t movingRedrawn = 0;
    std::uint32_t movingKept = 0;
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
    /// The movers and the casters changing sides are walked against the
    /// cascades and against nothing else, so this costs movers times cascades —
    /// never casters times cascades.
    ///
    /// Recording happens here: every layer named in @p out.redraw and
    /// @p out.movingRedraw is taken to have been drawn this frame, so the caller
    /// must draw all of them.
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

    /// Per cascade, the fingerprint (ShadowMoverSignature summed) of the movers
    /// drawn over its still depth, zero for none — which is also what a fresh
    /// still layer holds, since the slice it is copied into starts without any.
    std::array<std::uint64_t, kMaxShadowCascades> _movingSignature{};

    SunShadowCadenceStats _stats;
};

} // namespace Assisi::Render
