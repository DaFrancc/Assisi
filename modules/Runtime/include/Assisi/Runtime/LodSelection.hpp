/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file LodSelection.hpp
/// @brief Which level of an authored LOD chain an instance draws at, and the
///        memory that keeps that answer from flickering.
///
/// Selection is screen-relative size: an instance's bounding-sphere diameter as
/// a fraction of the viewport's height, against the descending thresholds on the
/// mesh's LOD chain. Size rather than distance, which is wrong the moment the
/// field of view or the resolution changes.

#include <cstdint>
#include <span>
#include <vector>

#include <Assisi/ECS/Entity.hpp>
#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::Runtime
{

/// @brief How many levels a per-level tally has room for; deeper chains fold
///        into the last bucket.
inline constexpr uint32_t kMaxReportedLods = 8;

/// @brief The knobs selection reads. One set per renderer — the per-level switch
///        points live on the asset.
struct LodSettings
{
    /// Off pins every instance to LOD0: the A/B against the whole feature, and
    /// the answer if a chain turns out to be authored wrong.
    bool enabled = true;

    /// Multiplies the measured screen size. Above 1 holds a finer level further
    /// away, below 1 drops detail sooner; a quality tier sets this and nothing
    /// else.
    float bias = 1.f;

    /// The dead band, as a fraction of a threshold: a level is given up at T and
    /// taken back only at T*(1+hysteresis), so an instance parked on a boundary
    /// settles. Two percent is about a pixel of travel at the sizes that switch.
    float hysteresis = 0.02f;

    /// The level every instance draws at, or -1 to measure for it. Clamped to
    /// each mesh's own chain, so one control serves chains of different depths
    /// and a mesh without one is unaffected.
    ///
    /// For looking at a level in place: an artist judging whether LOD2 is
    /// acceptable cannot walk backwards until it appears and still see it beside
    /// what it replaced. It names a level rather than measuring one, so neither
    /// the bias nor the dead band moves it.
    int32_t forcedLevel = -1;
};

/// @brief Where selection is measured from.
///
/// The camera's position rather than its view matrix: a shadow caster behind the
/// camera has a negative view-space depth and no projected size, and the shadow
/// gathers select for exactly those.
struct LodView
{
    glm::vec3 cameraPosition{0.f};
    /// Tangent of half the vertical field of view. Zero means unset.
    float tanHalfFovY = 0.f;
};

/// @brief @p worldSphere's diameter as a fraction of the viewport's height.
///
/// At most 1, reached where the sphere fills the view: nothing is more full for
/// being closer, and that point is well outside the sphere, so a ratio left
/// uncapped would overshoot on the way in. An unset view measures 0, which is "no
/// measurement" rather than "vanishingly small": feeding it to a threshold
/// compare would select the coarsest level in the chain.
[[nodiscard]] float LodScreenSize(const Geometry::BoundingSphere &worldSphere, const LodView &view);

/// @brief The level of @p lods to draw at @p screenSize, holding the dead band
///        around @p previousLevel.
///
/// The finest level the instance is big enough for, against thresholds raised by
/// the hysteresis fraction for every level finer than @p previousLevel. A forced
/// level short-circuits all of that, and selection being off outranks even that:
/// the off switch is the A/B against the whole feature and has to mean LOD0.
///
/// Idempotent: feeding its own result back returns that result again, which is
/// what lets draw-extract and the shadow gathers select the same instance in one
/// frame without fighting over the remembered level.
[[nodiscard]] uint32_t SelectLodLevel(std::span<const Geometry::LodRange> lods, float screenSize,
                                      uint32_t previousLevel, const LodSettings &settings);

/// @brief What selection did with one instance, and the sizes on either side of
///        the level it landed on.
///
/// For an inspector: an artist tuning a threshold reads the measurement against
/// the number it is compared to, instead of walking the camera back and forth to
/// find which side of it they are on.
struct LodReport
{
    uint32_t level = 0;           ///< The level drawn.
    uint32_t levelCount = 1;      ///< Levels in the chain; 1 is a mesh without one.
    float screenSize = 0.f;       ///< The measurement, before the bias.
    float biasedScreenSize = 0.f; ///< What the thresholds are compared against.

    /// The size this level is held down to; under it the next coarser one takes
    /// over. Zero where no comparison is happening — the coarsest level, which
    /// has nothing to fall to, and a level that was named rather than measured.
    float dropBelow = 0.f;

    /// The size at which the next finer level is taken back, the dead band
    /// included. Zero under the same rule, plus at LOD0, which has nothing finer.
    float regainAt = 0.f;

    /// The level was named rather than measured, so neither switch point above
    /// is being compared against.
    bool forced = false;
};

/// @brief Describe drawing @p worldSphere at @p level, as @ref LodReport.
///
/// Reports the level it is given rather than selecting one: it says what was
/// drawn, and re-deriving would disagree with the screen exactly when the
/// remembered level and a fresh measurement differ — which is the moment the
/// readout is worth consulting.
[[nodiscard]] LodReport DescribeLodSelection(std::span<const Geometry::LodRange> lods,
                                             const Geometry::BoundingSphere &worldSphere, uint32_t level,
                                             const LodView &view, const LodSettings &settings);

/// @brief The level each entity drew at last, and the settings selection reads.
///
/// The remembered level is what hysteresis needs and the only state selection
/// carries.
class LodSelector
{
public:
    void SetSettings(const LodSettings &settings) { _settings = settings; }
    [[nodiscard]] const LodSettings &Settings() const { return _settings; }

    /// @brief Whether a remembered level holds the dead band. On by default.
    ///
    /// Off while the GPU cull draws the scene: it keeps no memory per object
    /// and so thresholds plainly, and the shadow gathers and the editor select
    /// through here. A band held on this side alone would put a shadow or an
    /// outline on a different level from the instance the GPU drew, for as long
    /// as it sat in the band.
    ///
    /// Separate from the settings' hysteresis because it is a fact about which
    /// path draws, not a quality knob: the settings round-trip through the
    /// editor, and a band zeroed there would stay zeroed on the CPU path.
    void SetHoldsDeadBand(bool holds) { _holdsDeadBand = holds; }
    [[nodiscard]] bool HoldsDeadBand() const { return _holdsDeadBand; }

    /// @brief Point selection at the camera it measures from, for the frame.
    ///
    /// One view per frame rather than one per call: the draw path and both
    /// shadow gathers select the same instance within a frame, and measuring
    /// with different views would put them on different levels.
    ///
    /// A selector never given a view measures nothing and selects LOD0, unless
    /// the settings name a level, which needs no measurement.
    void BeginFrame(const LodView &view) { _view = view; }
    [[nodiscard]] const LodView &View() const { return _view; }

    /// @brief Choose the level for @p entity and remember it.
    ///
    /// A mesh with no chain, or a frame with neither a view nor a forced level,
    /// is LOD0 and takes no room in the table: a scene of single-level meshes
    /// pays nothing here.
    [[nodiscard]] uint32_t Select(ECS::Entity entity, std::span<const Geometry::LodRange> lods,
                                  const Geometry::BoundingSphere &worldSphere);

    /// @brief The level @ref Select would choose for @p entity now, without
    ///        remembering it.
    ///
    /// For a consumer that needs the level of an instance nothing on the CPU
    /// selected — the GPU cull's pick, which with the dead band released is a
    /// function of the frame alone and so lands here too.
    [[nodiscard]] uint32_t Preview(ECS::Entity entity, std::span<const Geometry::LodRange> lods,
                                   const Geometry::BoundingSphere &worldSphere) const;

    /// @brief The level @p entity was last selected at, or 0 if it never was.
    ///
    /// For a consumer drawing the same instance again in the same frame — an
    /// editor outline — that wants the geometry the mesh pass drew rather than a
    /// second opinion about it.
    [[nodiscard]] uint32_t Remembered(ECS::Entity entity) const;

    /// @brief Draw @p entity at @p level whatever it measures, or release the pin
    ///        with a negative @p level.
    ///
    /// One entity at a time — pinning a second releases the first. Held here
    /// rather than on the component because it is a way of looking at a scene,
    /// not a fact about it: it is never saved, and a level that reached content
    /// would be a chain authored around a debugging session.
    ///
    /// Outranks the settings' forced level, being the more specific of the two.
    void Pin(ECS::Entity entity, int32_t level);

    /// @brief The level @p entity is pinned to, or -1 when it is not the pinned
    ///        one.
    [[nodiscard]] int32_t PinnedLevel(ECS::Entity entity) const;

    /// @brief Whether any entity is pinned. What tells a viewport that one
    ///        instance is not showing what the scene's rules would give it.
    [[nodiscard]] bool HasPin() const { return _pinned != ECS::NullEntity; }

    /// @brief The level named for @p entity, or -1 when nothing names one and it
    ///        has to be measured.
    ///
    /// The entity's own pin first, then the settings' forced level. Selection off
    /// names LOD0, which is the whole of what off means.
    ///
    /// For a consumer that cannot measure — the GPU cull path has no per-instance
    /// answer to reach for, and a named level is the only kind it can take.
    [[nodiscard]] int32_t NamedLevelFor(ECS::Entity entity) const;

    /// @brief Forget every remembered level and release the pin. Call when the
    ///        scene is replaced: the entity indices survive it and the instances
    ///        do not, so a kept pin would land on whatever reused its index.
    void Clear();

private:
    // Indexed by ECS::Entity::index, grown on demand. The generation sits beside
    // the level so a recycled index reads as stale rather than as a level.
    struct Slot
    {
        uint32_t generation = 0;
        uint32_t level = 0;
        bool occupied = false;
    };

    [[nodiscard]] const Slot *Find(ECS::Entity entity) const;

    LodSettings _settings;
    LodView _view;
    std::vector<Slot> _levels;
    bool _holdsDeadBand = true;

    // The one pinned instance. One rather than a table because it answers "what
    // does this thing look like at LOD2", which is a question about the entity
    // being looked at, and there is one of those.
    ECS::Entity _pinned = ECS::NullEntity;
    uint32_t _pinnedLevel = 0;
};

} // namespace Assisi::Runtime
