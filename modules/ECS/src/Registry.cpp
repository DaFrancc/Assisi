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
        _alive[index] = true;

        ++_aliveCount;

        /* Return the slot with its current generation, which was incremented
           when the previous occupant was destroyed. */
        return {.index = index, .generation = _generations[index]};
    }

    /* No free slots — grow the generations array with a new slot at index 0. */
    const auto index = static_cast<uint32_t>(_generations.size());
    _generations.push_back(0);
    _alive.push_back(true);

    ++_aliveCount;
    return {.index = index, .generation = 0};
}

void Registry::Destroy(Entity entity)
{
    if (!IsAlive(entity))
    {
        return;
    }

    for (auto &entry : _pools)
    {
        entry.remove(entry.pool, entity);
    }

    /* Bump the generation so any stored handles to this entity become stale. */
    ++_generations[entity.index];
    _alive[entity.index] = false;

    /* Make the slot available for the next Create() call. */
    _freeSlots.push_back(entity.index);

    --_aliveCount;
}

void Registry::UnregisterPool(void *pool)
{
    auto iter = std::ranges::find_if(_pools, [pool](const PoolEntry &entry) { return entry.pool == pool; });
    if (iter != _pools.end())
    {
        _pools.erase(iter);
    }
}

bool Registry::IsAlive(Entity entity) const
{
    /* Bounds check first to safely handle NullEntity and out-of-range indices.
       The alive flag closes the case the generation compare alone misses: a
       handle carrying a freed slot's already-bumped generation. */
    return entity.index < _generations.size() && _generations[entity.index] == entity.generation &&
           _alive[entity.index];
}

Entity Registry::EntityAt(uint32_t index) const
{
    if (index >= _generations.size())
    {
        return NullEntity;
    }

    /* A slot present in the generations table is live unless it is currently
       parked in the free list awaiting reuse — tracked by the O(1) alive flag
       (this used to scan the free list, O(free) per call). */
    if (!_alive[index])
    {
        return NullEntity;
    }

    return {.index = index, .generation = _generations[index]};
}

std::size_t Registry::AliveCount() const
{
    return _aliveCount;
}

} // namespace Assisi::ECS