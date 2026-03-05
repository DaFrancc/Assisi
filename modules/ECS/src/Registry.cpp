/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <algorithm>

#include <Assisi/ECS/Registry.hpp>

namespace Assisi::ECS
{

Entity Registry::Create()
{
    if (!_freeSlots.empty())
    {
        /* Reuse a previously freed slot. */
        const uint32_t index = _freeSlots.back();
        _freeSlots.pop_back();

        ++_aliveCount;

        /* Return the slot with its current generation, which was incremented
           when the previous occupant was destroyed. */
        return { index, _generations[index] };
    }

    /* No free slots — grow the generations array with a new slot at index 0. */
    const auto index = static_cast<uint32_t>(_generations.size());
    _generations.push_back(0);

    ++_aliveCount;
    return { index, 0 };
}

void Registry::Destroy(Entity entity)
{
    if (!IsAlive(entity))
        return;

    for (auto& entry : _pools)
        entry.remove(entry.pool, entity);

    /* Bump the generation so any stored handles to this entity become stale. */
    ++_generations[entity.index];

    /* Make the slot available for the next Create() call. */
    _freeSlots.push_back(entity.index);

    --_aliveCount;
}

void Registry::UnregisterPool(void* pool)
{
    auto it = std::find_if(_pools.begin(), _pools.end(),
        [pool](const PoolEntry& entry) { return entry.pool == pool; });
    if (it != _pools.end())
        _pools.erase(it);
}

bool Registry::IsAlive(Entity entity) const
{
    /* Bounds check first to safely handle NullEntity and out-of-range indices. */
    return entity.index < _generations.size()
        && _generations[entity.index] == entity.generation;
}

std::size_t Registry::AliveCount() const
{
    return _aliveCount;
}

} // namespace Assisi::ECS