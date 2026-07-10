/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Registry.hpp
/// @brief Central ECS registry — owns entity IDs and component pools.
///
/// Call Create() to allocate an entity handle, Destroy() to release it.
/// IsAlive() validates a handle against the current generation so stale
/// handles (e.g. held by another entity after its target dies) are safely
/// detected.

#include <cstdint>
#include <vector>

#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/SparseSet.hpp>

namespace Assisi::ECS
{

struct PoolEntry
{
    void *pool;
    void (*remove)(void *pool, Entity entity);
};

struct Registry
{
    Registry() = default;

    // Non-copyable and non-movable: _pools holds non-owning pointers back to
    // pools that a Scene owns and registers. A shallow copy would share those
    // raw pointers between two registries, and a move would not re-seat them,
    // so either leaves a registry pointing at pools it does not coherently own.
    // A Registry only ever lives as a Scene member, which is itself pinned.
    Registry(const Registry &)            = delete;
    Registry &operator=(const Registry &) = delete;
    Registry(Registry &&)                 = delete;
    Registry &operator=(Registry &&)      = delete;

    /// @brief Allocates a new entity, reusing a free slot if one is available.
    Entity Create();

    /// @brief Releases an entity, invalidating all existing handles to it.
    ///
    /// Increments the generation for the slot so any stored Entity with the
    /// old generation will fail IsAlive().  Does nothing if the entity is
    /// already dead.
    void Destroy(Entity entity);

    /// @brief Returns true if the entity handle is still valid.
    [[nodiscard]] bool IsAlive(Entity entity) const;

    /// @brief Returns the live entity currently occupying a slot index.
    ///
    /// Resolves a bare slot index (as an inspector or tool would hold it) to a
    /// full handle carrying the slot's current generation.  Returns NullEntity
    /// if the index is out of range or the slot is free (no live occupant).
    [[nodiscard]] Entity EntityAt(uint32_t index) const;

    /// @brief Invokes fn(Entity) for every live entity, in ascending slot order.
    ///
    /// Skips free slots.  Intended for tooling (entity pickers, inspectors); the
    /// per-slot free-list check makes it O(n·free) — fine at editor scale, not a
    /// hot-loop primitive (use Query for the frame loop).
    template <typename Fn> void ForEachLive(Fn &&fn) const
    {
        for (uint32_t i = 0; i < static_cast<uint32_t>(_generations.size()); ++i)
        {
            const Entity e = EntityAt(i);
            if (e != NullEntity)
                fn(e);
        }
    }

    /// @brief Returns the number of currently live entities.
    [[nodiscard]] std::size_t AliveCount() const;

    /// @brief Registers a component pool so Destroy() removes the entity from it.
    /// The pool must outlive the registry (or be unregistered before destruction).
    template <typename T> void RegisterPool(SparseSet<T> *pool) { _pools.push_back({pool, &RemoveFn<T>}); }

    /// @brief Unregisters a previously registered pool.
    void UnregisterPool(void *pool);

    /// @brief Resets all entity counters to zero.
    ///
    /// Clears the generation table and the free-slot list so the next Create()
    /// returns Entity{0, 0} again.  Caller is responsible for clearing all
    /// component pools before calling this (Scene::Clear() does both).
    void Reset()
    {
        _generations.clear();
        _freeSlots.clear();
        _aliveCount = 0;
    }

  private:
    template <typename T> static void RemoveFn(void *pool, Entity entity)
    {
        static_cast<SparseSet<T> *>(pool)->Remove(entity);
    }

    std::vector<uint32_t> _generations; ///< One generation counter per slot.
    std::vector<uint32_t> _freeSlots;   ///< Slots available for reuse.
    std::vector<PoolEntry> _pools;      ///< Registered component pools.

    std::size_t _aliveCount = 0;
};

} // namespace Assisi::ECS