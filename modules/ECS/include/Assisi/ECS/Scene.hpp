/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Scene.hpp
/// @brief A self-contained ECS world — owns a Registry and all component pools.
///
/// Scene lazily creates a SparseSet<T> on the first Add<T> call and registers
/// it with the internal Registry so Destroy(entity) automatically removes the
/// entity from every pool it belongs to.

#include <cstdint>
#include <utility>
#include <vector>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/Reflect/ComponentId.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/Query.hpp>
#include <Assisi/ECS/Registry.hpp>

namespace Assisi::ECS
{

struct Scene
{
    Scene() = default;

    ~Scene()
    {
        for (auto &storage : _pools)
            if (storage.pool)
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

    /// @brief Restores a freed entity to its *exact* prior (index, generation).
    ///
    /// Neutral capability (undo of delete, redo of create, prefab instantiation,
    /// netcode rollback): resurrects a specific handle so every stored reference to
    /// it stays valid, with no scanning. See Registry::ReviveAt for the invariant —
    /// valid only for a currently-free slot under a strictly linear history. The
    /// revived entity comes back with *no* components; the caller repopulates them.
    ///
    /// Also cancels any pending deferred Destroy() of this slot. If the entity was
    /// queued this frame (Destroy() then ReviveAt() before the next FlushDestroyed),
    /// it is still fully alive — the deferred destroy has not run — so dropping the
    /// queued entry is the *entire* operation: the slot never died, and re-reviving
    /// a live slot in the registry would trip its "slot must be free" assert. Only
    /// when the handle is genuinely freed do we restore its exact identity.
    void ReviveAt(Entity entity)
    {
        if (std::erase(_pendingDestroy, entity) != 0)
            return; // was queued-but-alive: cancelling the destroy is all that's needed
        _registry.ReviveAt(entity);
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
        for (auto &storage : _pools)
            if (storage.pool)
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
        SparseSet<T> &pool = GetOrCreatePool<T>();
        T *added = pool.Add(entity, std::move(component));
        if (added && pool.TracksChanges())
            pool.Stamp(entity, ++_changeTick); // a fresh component counts as changed
        return added;
    }

    /// @brief Returns a pointer to the entity's component of type T, or nullptr if not present.
    ///
    /// Read-only intent: this does NOT stamp change detection. Use GetMut to write
    /// a tracked component so the change is observed; writing through this pointer
    /// on a tracked type is a silent missed change.
    template <typename T> T *Get(Entity entity)
    {
        SparseSet<T> *pool = GetPool<T>();
        return pool ? pool->Get(entity) : nullptr;
    }

    /// @brief Like Get<T>, but marks the component changed (for ACOMP(tracked)
    /// types) so change-detection consumers see the write. Prefer this over Get<T>
    /// whenever you intend to modify a tracked component. Conservative: it stamps
    /// on access, whether or not you actually alter the value.
    template <typename T> T *GetMut(Entity entity)
    {
        SparseSet<T> *pool = GetPool<T>();
        if (!pool)
            return nullptr;
        T *component = pool->Get(entity);
        if (component && pool->TracksChanges())
            pool->Stamp(entity, ++_changeTick);
        return component;
    }

    /// @brief Returns a const pointer to the entity's component of type T, or nullptr if not present.
    template <typename T> const T *Get(Entity entity) const
    {
        const SparseSet<T> *pool = GetPool<T>();
        return pool ? pool->Get(entity) : nullptr;
    }

    /// @brief Returns true if the entity has a component of type T.
    template <typename T> bool Has(Entity entity) const
    {
        const SparseSet<T> *pool = GetPool<T>();
        return pool && pool->Has(entity);
    }

    /// @brief Removes the component of type T from the entity.
    template <typename T> void Remove(Entity entity)
    {
        if (SparseSet<T> *pool = GetPool<T>())
            pool->Remove(entity);
    }

    // ── Change detection ──────────────────────────────────────────────────────
    // A monotonic per-write tick, stamped onto a component whenever it is accessed
    // mutably (Add / GetMut / MarkChanged) for an ACOMP(tracked) type. A consumer
    // remembers the tick it last ran at; a component whose tick exceeds that has
    // changed since. Conservative (mutable access stamps even if the value is
    // unchanged) — safe over-reporting, never a missed change.

    /// @brief The scene's current change tick — the value the most recent mutable
    /// access stamped. Record it after a system runs; anything with a higher
    /// ChangeTick next time has changed since.
    uint64_t CurrentChangeTick() const { return _changeTick; }

    /// @brief The last-written tick of the entity's T, or 0 (untracked / absent /
    /// never written). Meaningful only for ACOMP(tracked) types.
    template <typename T> uint64_t ChangeTick(Entity entity) const
    {
        const SparseSet<T> *pool = GetPool<T>();
        return pool ? pool->ChangeTick(entity) : 0;
    }

    /// @brief True if the entity's T was written after `sinceTick`.
    template <typename T> bool Changed(Entity entity, uint64_t sinceTick) const
    {
        return ChangeTick<T>(entity) > sinceTick;
    }

    /// @brief Marks the entity's component changed by ComponentId rather than
    /// static type — for generic reflected writers (e.g. the inspector edits a
    /// component through its field offsets, with no compile-time T). No-op for
    /// untracked components or an entity without that component.
    void MarkChanged(Entity entity, Core::Reflect::ComponentId id)
    {
        if (id < _pools.size() && _pools[id].stamp)
            _pools[id].stamp(_pools[id].pool, entity, ++_changeTick);
    }

    /// @brief Removes the entity's component identified by ComponentId rather than
    /// static type — for generic tooling (e.g. the inspector's delete-component
    /// button), which has a ComponentMeta but no compile-time T. No-op if the
    /// scene has no pool for that id or the entity lacks that component.
    void RemoveById(Entity entity, Core::Reflect::ComponentId id)
    {
        if (id < _pools.size() && _pools[id].pool && _pools[id].remove)
            _pools[id].remove(_pools[id].pool, entity);
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
        void *pool                                          = nullptr;
        void (*remove)(void *pool, Entity entity)           = nullptr;
        void (*clear)(void *pool)                           = nullptr;
        void (*destroy)(void *pool)                         = nullptr;
        void (*stamp)(void *pool, Entity entity, uint64_t tick) = nullptr; // null unless the pool is tracked
    };

    template <typename T> static void RemoveFn(void *pool, Entity entity)
    {
        static_cast<SparseSet<T> *>(pool)->Remove(entity);
    }

    template <typename T> static void ClearFn(void *pool) { static_cast<SparseSet<T> *>(pool)->Clear(); }

    template <typename T> static void DestroyFn(void *pool) { delete static_cast<SparseSet<T> *>(pool); }

    template <typename T> static void StampFn(void *pool, Entity entity, uint64_t tick)
    {
        static_cast<SparseSet<T> *>(pool)->Stamp(entity, tick);
    }

    /// @brief Returns a pointer to the pool for T, or nullptr if it has never
    /// been created in this scene.
    ///
    /// Pools are keyed by T's stable Core::Reflect::ComponentId, so this is a
    /// bounds-checked array index — no hashing. An unreflected T resolves to
    /// kInvalidComponentId and yields nullptr (it can never have a pool because
    /// GetOrCreatePool asserts on it). const because the lookup does not create;
    /// the returned pool is mutable since _pools holds type-erased owners.
    template <typename T> SparseSet<T> *GetPool() const
    {
        const Core::Reflect::ComponentId id = Core::Reflect::ComponentIdOf<T>();
        if (id >= _pools.size() || !_pools[id].pool)
            return nullptr;
        return static_cast<SparseSet<T> *>(_pools[id].pool);
    }

    template <typename T> SparseSet<T> &GetOrCreatePool()
    {
        const Core::Reflect::ComponentId id = Core::Reflect::ComponentIdOf<T>();
        ASSISI_ASSERT(id != Core::Reflect::kInvalidComponentId,
                      "Scene component types must be registered with the reflection "
                      "system (ACOMP); this T has no ComponentId.");

        if (id >= _pools.size())
            _pools.resize(id + 1);

        PoolStorage &slot = _pools[id];
        if (!slot.pool)
        {
            auto *pool = new SparseSet<T>();
            _registry.RegisterPool(pool);
            slot = PoolStorage{pool, &RemoveFn<T>, &ClearFn<T>, &DestroyFn<T>, nullptr};

            // Wire change detection for ACOMP(tracked) types. The registry is
            // finalized by the time a component is first added at runtime, so the
            // meta lookup is valid here.
            const Core::Reflect::ComponentMeta *meta = Core::Reflect::ComponentRegistry::Instance().ById(id);
            if (meta && meta->tracksChanges)
            {
                pool->SetTracksChanges(true);
                slot.stamp = &StampFn<T>;
            }
        }
        return *static_cast<SparseSet<T> *>(slot.pool);
    }

    Registry _registry;
    /// Component pools indexed by Core::Reflect::ComponentId. Empty slots
    /// (pool == nullptr) are ids whose component has not been added to this
    /// scene — including gaps for components that belong to other modules.
    std::vector<PoolStorage> _pools;
    std::vector<Entity> _pendingDestroy; ///< Entities queued by Destroy(), drained by FlushDestroyed().

    /// Monotonic change-detection tick, bumped on each mutable access to a tracked
    /// component (Add / GetMut / MarkChanged). See the Change detection section.
    uint64_t _changeTick = 0;
};

} // namespace Assisi::ECS
