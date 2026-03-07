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