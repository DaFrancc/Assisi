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
/// A camera inside the sphere gets 1 — it fills the view, and the ratio past
/// that point grows without bound. An unset view measures 0, which is "no
/// measurement" rather than "vanishingly small": feeding it to a threshold
/// compare would select the coarsest level in the chain.
[[nodiscard]] float LodScreenSize(const Geometry::BoundingSphere &worldSphere, const LodView &view);

/// @brief The level of @p lods to draw at @p screenSize, holding the dead band
///        around @p previousLevel.
///
/// The finest level the instance is big enough for, against thresholds raised by
/// the hysteresis fraction for every level finer than @p previousLevel.
///
/// Idempotent: feeding its own result back returns that result again, which is
/// what lets draw-extract and the shadow gathers select the same instance in one
/// frame without fighting over the remembered level.
[[nodiscard]] uint32_t SelectLodLevel(std::span<const Geometry::LodRange> lods, float screenSize,
                                      uint32_t previousLevel, const LodSettings &settings);

/// @brief The level each entity drew at last, and the settings selection reads.
///
/// The remembered level is what hysteresis needs and the only state selection
/// carries.
class LodSelector
{
public:
    void SetSettings(const LodSettings &settings) { _settings = settings; }
    [[nodiscard]] const LodSettings &Settings() const { return _settings; }

    /// @brief Point selection at the camera it measures from, for the frame.
    ///
    /// One view per frame rather than one per call: the draw path and both
    /// shadow gathers select the same instance within a frame, and measuring
    /// with different views would put them on different levels.
    ///
    /// A selector never given a view measures nothing and selects LOD0.
    void BeginFrame(const LodView &view) { _view = view; }
    [[nodiscard]] const LodView &View() const { return _view; }

    /// @brief Choose the level for @p entity and remember it.
    ///
    /// A mesh with no chain, or a frame with no view, is LOD0 and takes no room
    /// in the table: a scene of single-level meshes pays nothing here.
    [[nodiscard]] uint32_t Select(ECS::Entity entity, std::span<const Geometry::LodRange> lods,
                                  const Geometry::BoundingSphere &worldSphere);

    /// @brief The level @p entity was last selected at, or 0 if it never was.
    ///
    /// For a consumer drawing the same instance again in the same frame — an
    /// editor outline — that wants the geometry the mesh pass drew rather than a
    /// second opinion about it.
    [[nodiscard]] uint32_t Remembered(ECS::Entity entity) const;

    /// @brief Forget every remembered level. Call when the scene is replaced:
    ///        the entity indices survive it and the instances do not.
    void Clear() { _levels.clear(); }

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
};

} // namespace Assisi::Runtime
