#pragma once

/// @file Scene.hpp
/// @brief A self-contained ECS world — owns a Registry and all component pools.
///
/// Scene lazily creates a SparseSet<T> on the first Add<T> call and registers
/// it with the internal Registry so Destroy(entity) automatically removes the
/// entity from every pool it belongs to.

#include <expected>
#include <typeindex>
#include <unordered_map>

#include <Assisi/ECS/Registry.hpp>

namespace Assisi::ECS
{

struct Scene
{
    ~Scene()
    {
        for (auto& [type, storage] : _pools)
            storage.destroy(storage.pool);
    }

    /// @brief Allocates a new entity.
    Entity Create() { return _registry.Create(); }

    /// @brief Releases an entity, removing it from all registered pools.
    void Destroy(Entity entity) { _registry.Destroy(entity); }

    /// @brief Returns true if the entity handle is still valid.
    bool IsAlive(Entity entity) const { return _registry.IsAlive(entity); }

    /// @brief Returns the number of currently live entities.
    std::size_t AliveCount() const { return _registry.AliveCount(); }

    /// @brief Adds a component of type T to the entity.
    ///
    /// Creates the component pool on first use.
    /// @return Pointer to the new component on success, or
    ///         SparseSetError::AlreadyExists if the entity already has one.
    template<typename T>
    [[nodiscard]] std::expected<T*, SparseSetError> Add(Entity entity, T component = {})
    {
        return GetOrCreatePool<T>().Add(entity, component);
    }

    /// @brief Returns a pointer to the entity's component of type T, or nullptr if not present.
    template<typename T>
    T* Get(Entity entity)
    {
        auto it = _pools.find(typeid(T));
        if (it == _pools.end()) return nullptr;
        return static_cast<SparseSet<T>*>(it->second.pool)->Get(entity);
    }

    /// @brief Returns a const pointer to the entity's component of type T, or nullptr if not present.
    template<typename T>
    const T* Get(Entity entity) const
    {
        auto it = _pools.find(typeid(T));
        if (it == _pools.end()) return nullptr;
        return static_cast<const SparseSet<T>*>(it->second.pool)->Get(entity);
    }

    /// @brief Returns true if the entity has a component of type T.
    template<typename T>
    bool Has(Entity entity) const
    {
        auto it = _pools.find(typeid(T));
        return it != _pools.end() && static_cast<const SparseSet<T>*>(it->second.pool)->Has(entity);
    }

    /// @brief Removes the component of type T from the entity.
    template<typename T>
    void Remove(Entity entity)
    {
        auto it = _pools.find(typeid(T));
        if (it != _pools.end())
            static_cast<SparseSet<T>*>(it->second.pool)->Remove(entity);
    }

  private:
    struct PoolStorage
    {
        void* pool;
        void (*remove)(void* pool, Entity entity);
        void (*destroy)(void* pool);
    };

    template<typename T>
    static void RemoveFn(void* pool, Entity entity)
    {
        static_cast<SparseSet<T>*>(pool)->Remove(entity);
    }

    template<typename T>
    static void DestroyFn(void* pool)
    {
        delete static_cast<SparseSet<T>*>(pool);
    }

    template<typename T>
    SparseSet<T>& GetOrCreatePool()
    {
        auto it = _pools.find(typeid(T));
        if (it != _pools.end())
            return *static_cast<SparseSet<T>*>(it->second.pool);

        auto* pool = new SparseSet<T>();
        _registry.RegisterPool(pool);
        _pools.emplace(typeid(T), PoolStorage{ pool, &RemoveFn<T>, &DestroyFn<T> });
        return *pool;
    }

    Registry                                         _registry;
    std::unordered_map<std::type_index, PoolStorage> _pools;
};

} // namespace Assisi::ECS
