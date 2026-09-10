/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <algorithm>
#include <cmath>

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
    // Inside its own sphere the instance fills the view; the ratio past that
    // point grows without bound.
    if (distance <= worldSphere.radius)
    {
        return 1.f;
    }
    return worldSphere.radius / (distance * view.tanHalfFovY);
}

uint32_t SelectLodLevel(std::span<const Geometry::LodRange> lods, float screenSize, uint32_t previousLevel,
                        const LodSettings &settings)
{
    if (lods.size() <= 1 || !settings.enabled)
    {
        return 0;
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
    return static_cast<uint32_t>(lods.size()) - 1u;
}

uint32_t LodSelector::Select(ECS::Entity entity, std::span<const Geometry::LodRange> lods,
                             const Geometry::BoundingSphere &worldSphere)
{
    // No measurement and no table entry for a mesh with one level, so a scene
    // without chains pays nothing here.
    if (entity == ECS::NullEntity || !(_view.tanHalfFovY > 0.f) || lods.size() <= 1)
    {
        return 0;
    }

    const uint32_t level = SelectLodLevel(lods, LodScreenSize(worldSphere, _view), Remembered(entity), _settings);

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
