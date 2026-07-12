/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Scene.hpp
/// @brief A self-contained ECS world — owns a Registry and all component pools.
///
/// Scene lazily creates a SparseSet<T> on the first Add<T> call and registers
/// it with the internal Registry so Destroy(entity) automatically removes the
/// entity from every pool it belongs to.

#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Assisi/ECS/Query.hpp>
#include <Assisi/ECS/Registry.hpp>

namespace Assisi::ECS
{

struct Scene
{
    Scene() = default;

    ~Scene()
    {
        for (auto &[type, storage] : _pools)
            storage.destroy(storage.pool);
    }

    // Non-copyable and non-movable: Scene owns its component pools as raw
    // pointers (_pools) and frees them in ~Scene. A shallow copy would let two
    // Scenes delete the same pools (double free); a move would leave the
    // moved-from Registry's pool back-registrations dangling. No caller copies
    // or moves a Scene — SceneRegistry owns them through std::unique_ptr.
    Scene(const Scene &)            = delete;
    Scene &operator=(const Scene &) = delete;
    Scene(Scene &&)                 = delete;
    Scene &operator=(Scene &&)      = delete;

    /// @brief Allocates a new entity.
    Entity Create() { return _registry.Create(); }

    /// @brief Queues an entity for destruction at the next FlushDestroyed().
    ///
    /// Deferred, not immediate: the entity stays fully alive — IsAlive() is
    /// true, Query still yields it, Get<T> still works — until FlushDestroyed()
    /// runs (once per frame from Application::Run, or explicitly in tests and
    /// custom loops). That is what makes it safe to Destroy() while iterating a
    /// Query: no component pool is touched until the flush, so no iterator is
    /// invalidated. A handle already dead — or destroyed twice this frame — is
    /// ignored; the slot is not reused until the flush actually applies it.
    void Destroy(Entity entity)
    {
        if (_registry.IsAlive(entity))
            _pendingDestroy.push_back(entity);
    }

    /// @brief Applies every Destroy() queued since the last flush.
    ///
    /// Removes each queued entity from the registry and all component pools and
    /// frees its slot for reuse, then clears the queue. Application::Run calls
    /// this once per frame after systems run; tests and custom loops call it
    /// directly. A no-op when nothing is queued.
    void FlushDestroyed()
    {
        for (Entity entity : _pendingDestroy)
            _registry.Destroy(entity);
        _pendingDestroy.clear();
    }

    /// @brief Returns true if the entity handle is still valid.
    bool IsAlive(Entity entity) const { return _registry.IsAlive(entity); }

    /// @brief Returns the number of currently live entities.
    std::size_t AliveCount() const { return _registry.AliveCount(); }

    /// @brief Resolves a slot index to its live entity, or NullEntity if empty.
    Entity EntityAt(uint32_t index) const { return _registry.EntityAt(index); }

    /// @brief Invokes fn(Entity) for every live entity in the scene (tooling helper).
    template <typename Fn> void ForEachEntity(Fn &&fn) const { _registry.ForEachLive(std::forward<Fn>(fn)); }

    /// @brief Removes all entities and components, resetting index counters to zero.
    ///
    /// After this call the scene is equivalent to a freshly constructed one:
    /// the next Create() returns Entity{0, 0}.  All component pools are kept
    /// alive (no allocations freed) so they can be refilled without realloc.
    void Clear()
    {
        for (auto &[type, storage] : _pools)
            storage.clear(storage.pool);
        _registry.Reset();
        _pendingDestroy.clear();
    }

    /// @brief Adds a component of type T to the entity.
    ///
    /// Creates the component pool on first use.
    /// @return Pointer to the new component, or nullptr if the add was rejected:
    ///         the entity is not alive (a stale handle — the pool-level check
    ///         only catches this once the live occupant has populated the same
    ///         pool, so guard here where liveness is authoritative), or the
    ///         entity already has this component.
    template <typename T> [[nodiscard]] T *Add(Entity entity, T component = {})
    {
        if (!IsAlive(entity))
            return nullptr;
        return GetOrCreatePool<T>().Add(entity, std::move(component));
    }

    /// @brief Returns a pointer to the entity's component of type T, or nullptr if not present.
    template <typename T> T *Get(Entity entity)
    {
        auto it = _pools.find(typeid(T));
        if (it == _pools.end())
            return nullptr;
        return static_cast<SparseSet<T> *>(it->second.pool)->Get(entity);
    }

    /// @brief Returns a const pointer to the entity's component of type T, or nullptr if not present.
    template <typename T> const T *Get(Entity entity) const
    {
        auto it = _pools.find(typeid(T));
        if (it == _pools.end())
            return nullptr;
        return static_cast<const SparseSet<T> *>(it->second.pool)->Get(entity);
    }

    /// @brief Returns true if the entity has a component of type T.
    template <typename T> bool Has(Entity entity) const
    {
        auto it = _pools.find(typeid(T));
        return it != _pools.end() && static_cast<const SparseSet<T> *>(it->second.pool)->Has(entity);
    }

    /// @brief Removes the component of type T from the entity.
    template <typename T> void Remove(Entity entity)
    {
        auto it = _pools.find(typeid(T));
        if (it != _pools.end())
            static_cast<SparseSet<T> *>(it->second.pool)->Remove(entity);
    }

    /// @brief Returns a lazy view over all entities that have every component in Ts.
    ///
    /// Iterates the smallest matching pool and skips entities absent from the others.
    /// Supports structured bindings: `for (auto [e, pos, vel] : scene.Query<Position, Velocity>())`
    ///
    /// @warning The view holds a pointer into the driving pool's internal entity
    /// array. A structural change to a *queried* component pool during iteration
    /// — Add<T> or Remove<T> on one of the Ts — may reallocate or swap-remove
    /// that array and invalidate the iterator. Debug builds turn this into a loud
    /// assert via a per-pool version counter (see SparseSet::StructureVersion);
    /// release builds do not, so it is silent UB — don't do it. Destroy() is safe
    /// here: it is deferred to FlushDestroyed() and touches no pool mid-loop. To
    /// Add/Remove a queried component for entities found while iterating, collect
    /// them into a local vector and apply the change after the loop.
    template <typename... Ts> QueryView<std::tuple<Ts...>, std::tuple<>> Query() { return Query<Ts...>(Without<>{}); }

    /// @brief Returns a lazy view over entities that have every Ts but none of the Es.
    ///
    /// `scene.Query<A>(Without<B>{})` yields entities with A and not B. Exclusion
    /// is a single-pass filter folded into the same iteration as the positive
    /// match — it never runs a second loop. The plain overload's mid-loop mutation
    /// warning applies to the *required* pools (the Ts). Mutating an *excluded*
    /// pool (an Es) mid-loop is safe: it is re-probed each step through its stable
    /// pool address, nothing cached points into it, so a change there cannot
    /// dangle the iterator (and the debug check does not count excluded pools).
    template <typename... Ts, typename... Es> QueryView<std::tuple<Ts...>, std::tuple<Es...>> Query(Without<Es...>)
    {
        std::tuple<SparseSet<Ts> *...> pools = {GetPool<Ts>()...};
        std::tuple<const SparseSet<Es> *...> excluded = {GetPool<Es>()...};

        /* If any required pool is missing, there are no matching entities. A
           missing excluded pool is fine — it simply excludes nobody. */
        bool anyNull = false;
        std::apply([&](auto *...ps) { anyNull = (... || (ps == nullptr)); }, pools);
        if (anyNull)
        {
            return QueryView<std::tuple<Ts...>, std::tuple<Es...>>{pools, nullptr, excluded};
        }

        /* Drive iteration from the smallest required pool to minimise skipped
           entities; excluded pools never drive iteration. */
        const std::vector<Entity> *primary = nullptr;
        std::size_t minSize = SIZE_MAX;
        std::apply(
            [&](auto *...ps)
            {
                auto check = [&](auto *p)
                {
                    if (p->Size() < minSize)
                    {
                        minSize = p->Size();
                        primary = &p->Entities();
                    }
                };
                (check(ps), ...);
            },
            pools);

        return QueryView<std::tuple<Ts...>, std::tuple<Es...>>{pools, primary, excluded};
    }

  private:
    struct PoolStorage
    {
        void *pool;
        void (*remove)(void *pool, Entity entity);
        void (*clear)(void *pool);
        void (*destroy)(void *pool);
    };

    template <typename T> static void RemoveFn(void *pool, Entity entity)
    {
        static_cast<SparseSet<T> *>(pool)->Remove(entity);
    }

    template <typename T> static void ClearFn(void *pool) { static_cast<SparseSet<T> *>(pool)->Clear(); }

    template <typename T> static void DestroyFn(void *pool) { delete static_cast<SparseSet<T> *>(pool); }

    /// @brief Returns a pointer to the pool for T, or nullptr if it has never been created.
    template <typename T> SparseSet<T> *GetPool()
    {
        auto it = _pools.find(typeid(T));
        if (it == _pools.end())
            return nullptr;
        return static_cast<SparseSet<T> *>(it->second.pool);
    }

    template <typename T> SparseSet<T> &GetOrCreatePool()
    {
        auto it = _pools.find(typeid(T));
        if (it != _pools.end())
            return *static_cast<SparseSet<T> *>(it->second.pool);

        auto *pool = new SparseSet<T>();
        _registry.RegisterPool(pool);
        _pools.emplace(typeid(T), PoolStorage{pool, &RemoveFn<T>, &ClearFn<T>, &DestroyFn<T>});
        return *pool;
    }

    Registry _registry;
    std::unordered_map<std::type_index, PoolStorage> _pools;
    std::vector<Entity> _pendingDestroy; ///< Entities queued by Destroy(), drained by FlushDestroyed().
};

} // namespace Assisi::ECS
