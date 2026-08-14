/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <algorithm>

#include <Assisi/Core/Assert.hpp>
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

    /* No free slots — append a fresh slot at the end, generation 0. */
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

void Registry::ReviveAt(Entity entity)
{
    /* Grow the slot table when the target sits past its end. The invariant is "no
       live handle for this slot", not "the slot exists": a Clear()ed scene has no
       slots at all, and a restore spanning that reset must still land every entity
       on its exact handle. Slots skipped on the way in are created free, so the
       next Create() fills them instead of allocating past them. */
    if (entity.index >= _generations.size())
    {
        for (auto index = static_cast<uint32_t>(_generations.size()); index < entity.index; ++index)
        {
            _generations.push_back(0);
            _alive.push_back(false);
            _freeSlots.push_back(index);
        }
        _generations.push_back(0);
        _alive.push_back(false); // made live by the common path below
    }

    ASSISI_ASSERT(!_alive[entity.index],
                  "ReviveAt: slot is already live — reviving over a live occupant would alias two "
                  "entities onto one slot. Only a freed slot may be revived (linear-history invariant).");

    /* Restore the exact generation — may decrease relative to the current value,
       the one sanctioned break of the monotonic-generation rule. */
    _generations[entity.index] = entity.generation;
    _alive[entity.index]       = true;

    /* Scrub the slot from the free list; without this the next Create() would
       reuse it and hand out a second live handle sharing all components. */
    std::erase(_freeSlots, entity.index);

    ++_aliveCount;
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
       parked in the free list awaiting reuse — which the O(1) alive flag answers
       without scanning that list. */
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