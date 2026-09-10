/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <algorithm>
#include <cmath>
#include <limits>

#include <Assisi/Runtime/LodSelection.hpp>

namespace Assisi::Runtime
{

float LodScreenSize(const Geometry::BoundingSphere &worldSphere, const LodView &view)
{
    if (!(view.tanHalfFovY > 0.f) || !(worldSphere.radius > 0.f))
    {
        return 0.f;
    }

    const glm::vec3 toCenter = worldSphere.center - view.cameraPosition;
    const float distance = std::sqrt(glm::dot(toCenter, toCenter));
    // Inside its own sphere there is no projected size to speak of, and the
    // division below would run to infinity at the surface.
    if (distance <= worldSphere.radius)
    {
        return 1.f;
    }
    // Capped where the instance fills the view, which is well outside its
    // sphere: closer is not more full, and without the cap the ratio climbs
    // past 1 on the way in and then drops back to it at the surface.
    return std::min(worldSphere.radius / (distance * view.tanHalfFovY), 1.f);
}

uint32_t SelectLodLevel(std::span<const Geometry::LodRange> lods, float screenSize, uint32_t previousLevel,
                        const LodSettings &settings)
{
    if (lods.size() <= 1 || !settings.enabled)
    {
        return 0;
    }

    const uint32_t coarsest = static_cast<uint32_t>(lods.size()) - 1u;
    if (settings.forcedLevel >= 0)
    {
        // Clamped, not rejected: one control stands over chains of every depth,
        // and a level past the end of this one means its last.
        return std::min(static_cast<uint32_t>(settings.forcedLevel), coarsest);
    }

    const float biased = screenSize * settings.bias;
    // The band applies only to levels finer than the one being drawn, and that
    // asymmetry is the whole mechanism: a level is given up the moment the
    // instance falls under its threshold, and taken back only a band above it.
    const float band = 1.f + std::max(settings.hysteresis, 0.f);

    for (uint32_t level = 0; level < lods.size(); ++level)
    {
        const float threshold = Geometry::LodScreenSizeThreshold(lods[level], level) * (level < previousLevel ? band : 1.f);
        if (biased >= threshold)
        {
            return level;
        }
    }
    // Smaller than the coarsest threshold still draws that level; dropping the
    // instance is a cull, and culling is the frustum's job.
    return coarsest;
}

float LodOrthographicSize(const Geometry::BoundingSphere &worldSphere, float viewExtent)
{
    if (!(viewExtent > 0.f) || !(worldSphere.radius > 0.f))
    {
        return 0.f;
    }
    return std::min(2.f * worldSphere.radius / viewExtent, 1.f);
}

uint32_t SelectShadowLodLevel(std::span<const Geometry::LodRange> lods, float viewSize, uint32_t drawnLevel,
                              const LodSettings &settings)
{
    const uint32_t coarsest = lods.empty() ? 0u : static_cast<uint32_t>(lods.size()) - 1u;
    const uint32_t drawn = std::min(drawnLevel, coarsest);
    if (!settings.enabled || !settings.shadowLod || settings.forcedLevel >= 0 || !(viewSize > 0.f))
    {
        return drawn;
    }

    // LOD0 as the previous level: no band, since the size a view measures does
    // not move with the camera and so has no boundary to oscillate across.
    const uint32_t measured = SelectLodLevel(lods, viewSize, 0u, settings);
    return std::clamp(measured, drawn, std::min(drawn + 1u, coarsest));
}

LodReport DescribeLodSelection(std::span<const Geometry::LodRange> lods, const Geometry::BoundingSphere &worldSphere,
                               uint32_t level, const LodView &view, const LodSettings &settings)
{
    LodReport report;
    report.levelCount = std::max<uint32_t>(static_cast<uint32_t>(lods.size()), 1u);
    report.level = std::min(level, report.levelCount - 1u);
    report.screenSize = LodScreenSize(worldSphere, view);
    report.biasedScreenSize = report.screenSize * settings.bias;
    report.forced = settings.forcedLevel >= 0;

    // A pinned level compares against nothing, and a chain of one has nothing to
    // compare against: printing a threshold in either case would read as a
    // measurement that agreed.
    if (report.levelCount <= 1 || !settings.enabled || report.forced)
    {
        return report;
    }

    if (report.level + 1u < report.levelCount)
    {
        report.dropBelow = Geometry::LodScreenSizeThreshold(lods[report.level], report.level);
    }
    if (report.level > 0)
    {
        const uint32_t finer = report.level - 1u;
        report.regainAt =
            Geometry::LodScreenSizeThreshold(lods[finer], finer) * (1.f + std::max(settings.hysteresis, 0.f));
    }
    return report;
}

void LodSelector::Pin(ECS::Entity entity, int32_t level)
{
    if (level < 0 || entity == ECS::NullEntity)
    {
        _pinned = ECS::NullEntity;
        _pinnedLevel = 0;
        return;
    }
    _pinned = entity;
    _pinnedLevel = static_cast<uint32_t>(level);
}

int32_t LodSelector::PinnedLevel(ECS::Entity entity) const
{
    return entity != ECS::NullEntity && entity == _pinned ? static_cast<int32_t>(_pinnedLevel) : -1;
}

int32_t LodSelector::NamedLevelFor(ECS::Entity entity) const
{
    if (!_settings.enabled)
    {
        return 0;
    }
    const int32_t pinned = PinnedLevel(entity);
    return pinned >= 0 ? pinned : _settings.forcedLevel;
}

void LodSelector::Clear()
{
    _levels.clear();
    Pin(ECS::NullEntity, -1);
}

uint32_t LodSelector::Preview(ECS::Entity entity, std::span<const Geometry::LodRange> lods,
                              const Geometry::BoundingSphere &worldSphere) const
{
    // A named level is not a measurement, so it stands in a frame with no view
    // to measure in.
    const int32_t named = entity != ECS::NullEntity ? NamedLevelFor(entity) : -1;
    if (entity == ECS::NullEntity || lods.size() <= 1 || (named < 0 && !(_view.tanHalfFovY > 0.f)))
    {
        return 0;
    }

    // The one place the two ways of naming a level are resolved into one answer;
    // everything below this line reads a single forced level.
    LodSettings settings = _settings;
    settings.forcedLevel = named;

    // LOD0 as the previous level holds the band around nothing, since the band
    // only ever applies to levels finer than the previous one.
    const uint32_t previous = _holdsDeadBand ? Remembered(entity) : 0u;
    return SelectLodLevel(lods, LodScreenSize(worldSphere, _view), previous, settings);
}

uint32_t LodSelector::Select(ECS::Entity entity, std::span<const Geometry::LodRange> lods,
                             const Geometry::BoundingSphere &worldSphere)
{
    const uint32_t level = Preview(entity, lods, worldSphere);

    // No table entry for a mesh with one level, so a scene without chains pays
    // nothing here.
    if (entity == ECS::NullEntity || lods.size() <= 1)
    {
        return level;
    }

    if (entity.index >= _levels.size())
    {
        _levels.resize(static_cast<size_t>(entity.index) + 1u);
    }
    _levels[entity.index] = Slot{.generation = entity.generation, .level = level, .occupied = true};
    return level;
}

uint32_t LodSelector::Remembered(ECS::Entity entity) const
{
    const Slot *const slot = Find(entity);
    return slot != nullptr ? slot->level : 0u;
}

uint32_t LodSelector::CoarserShadowViews(ECS::Entity entity, std::span<const Geometry::LodRange> lods,
                                         const Geometry::BoundingSphere &worldSphere, uint32_t drawnLevel) const
{
    if (lods.size() <= 1 || PinnedLevel(entity) >= 0)
    {
        return 0u;
    }

    const auto views = static_cast<uint32_t>(
        std::min<size_t>(_shadowViewExtents.size(), static_cast<size_t>(std::numeric_limits<uint32_t>::digits)));
    uint32_t coarser = 0;
    for (uint32_t view = 0; view < views; ++view)
    {
        const float size = LodOrthographicSize(worldSphere, _shadowViewExtents[view]);
        if (SelectShadowLodLevel(lods, size, drawnLevel, _settings) > drawnLevel)
        {
            coarser |= 1u << view;
        }
    }
    return coarser;
}

const LodSelector::Slot *LodSelector::Find(ECS::Entity entity) const
{
    if (entity.index >= _levels.size())
    {
        return nullptr;
    }
    const Slot &slot = _levels[entity.index];
    // The generation is what makes a recycled index a different instance rather
    // than the old one having changed size.
    if (!slot.occupied || slot.generation != entity.generation)
    {
        return nullptr;
    }
    return &slot;
}

} // namespace Assisi::Runtime
