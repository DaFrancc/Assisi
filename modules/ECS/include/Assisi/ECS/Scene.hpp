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
    Scene(const Scene &) = delete;
    Scene &operator=(const Scene &) = delete;
    Scene(Scene &&) = delete;
    Scene &operator=(Scene &&) = delete;

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
    /// on access, whether or not you actually alter the value. The query-shaped
    /// equivalent is QueryMut<Ts...>, whose Mut proxies stamp the same way.
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
    // mutably (Add / GetMut / MarkChanged / a Mut proxy from QueryMut) for an
    // ACOMP(tracked) type. A consumer remembers the tick it last ran at; a
    // component whose tick exceeds that has changed since. Conservative (mutable
    // access stamps even if the value is unchanged) — safe over-reporting, never a
    // missed change. The one way to lose a change is to write through an access
    // path that cannot stamp: Get<T> or a plain Query — see QueryMut.

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

    /// @brief Appends every entity whose T was written after `sinceTick`.
    ///
    /// What a system asks when it must react to a change it cannot predict the
    /// location of — the shadow atlas's cache invalidation is the case: it needs
    /// the entities that moved, and asking Changed<Transform> of every caster
    /// would cost the whole scene on a frame where nothing moved at all.
    ///
    /// @p out is appended to rather than cleared, so one set may collect several
    /// component types' changes.
    template <typename T> void ChangedSince(uint64_t sinceTick, std::vector<Entity> &out) const
    {
        if (const SparseSet<T> *pool = GetPool<T>())
            pool->ChangedSince(sinceTick, out);
    }

    /// @brief How many entities in this scene carry the component with @p id.
    ///
    /// By ComponentId rather than static type so a caller holding only a runtime
    /// id can ask — notably the system registry's activation gate, which decides
    /// whether a system has anything to do before calling it. Cheap by construction:
    /// ids index a bounds-checked array, so this is two loads and a compare, with
    /// no hashing and no iteration.
    ///
    /// A component never added to this scene has no pool and reports 0 — which is
    /// the same answer as an emptied pool, deliberately, since "nothing to do"
    /// covers both. Note this counts entities queued for destruction too (they
    /// leave their pools at FlushDestroyed), so a gate can be one frame late to
    /// close. That over-runs a system rather than skipping one, which is the safe
    /// direction.
    [[nodiscard]] std::size_t ComponentCount(Core::Reflect::ComponentId id) const
    {
        // .value throughout this function: array index into _pools.
        if (id.value >= _pools.size() || !_pools[id.value].pool || !_pools[id.value].size)
            return 0;
        return _pools[id.value].size(_pools[id.value].pool);
    }

    /// @brief The last-written tick of the entity's component identified by
    /// ComponentId rather than static type — the generic counterpart of
    /// ChangeTick<T>.
    ///
    /// For reflected walkers that hold a ComponentMeta and no compile-time T:
    /// the replication codec decides what to put in a delta snapshot by asking
    /// this for every component it might send. Returns 0 for an untracked
    /// component, a pool this scene has never created, or an entity that does
    /// not have that component — all of which read as "not changed since", which
    /// is the correct answer in each case.
    uint64_t ChangeTickById(Entity entity, Core::Reflect::ComponentId id) const
    {
        // .value: array index into _pools.
        if (id.value >= _pools.size() || !_pools[id.value].changeTick)
            return 0;
        return _pools[id.value].changeTick(_pools[id.value].pool, entity);
    }

    /// @brief True if the entity's component @p id was written after @p sinceTick.
    bool ChangedById(Entity entity, Core::Reflect::ComponentId id, uint64_t sinceTick) const
    {
        return ChangeTickById(entity, id) > sinceTick;
    }

    /// @brief Marks the entity's component changed by ComponentId rather than
    /// static type — for generic reflected writers (e.g. the inspector edits a
    /// component through its field offsets, with no compile-time T). No-op for
    /// untracked components or an entity without that component.
    void MarkChanged(Entity entity, Core::Reflect::ComponentId id)
    {
        // .value: array index into _pools.
        if (id.value < _pools.size() && _pools[id.value].stamp)
            _pools[id.value].stamp(_pools[id.value].pool, entity, ++_changeTick);
    }

    /// @brief Removes the entity's component identified by ComponentId rather than
    /// static type — for generic tooling (e.g. the inspector's delete-component
    /// button), which has a ComponentMeta but no compile-time T. No-op if the
    /// scene has no pool for that id or the entity lacks that component.
    void RemoveById(Entity entity, Core::Reflect::ComponentId id)
    {
        // .value: array index into _pools.
        if (id.value < _pools.size() && _pools[id.value].pool && _pools[id.value].remove)
            _pools[id.value].remove(_pools[id.value].pool, entity);
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
    ///
    /// @warning The yielded `Ts&` are mutable, but writing an ACOMP(tracked)
    /// component through them does **not** stamp change detection — exactly as
    /// Get<T> does not, and for the same reason: this view cannot tell a read
    /// from a write. `Changed<T>` would keep reporting false and every consumer
    /// filtering on it would skip the entity. Use QueryMut<Ts...> for the types
    /// you write.
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
    template <typename... Ts, typename... Es>
    QueryView<std::tuple<Ts...>, std::tuple<Es...>> Query(Without<Es...> without)
    {
        return MakeView<RefAccess, Ts...>(without, NoChangeTick{});
    }

    /// @brief Like Query<Ts...>, but yields Mut<T> write proxies that stamp
    /// change detection — the query-shaped counterpart of GetMut.
    ///
    /// `for (auto [e, pos] : scene.QueryMut<Position>()) pos->x += 1.f;` records
    /// the write on an ACOMP(tracked) Position, so `Changed<Position>(e, since)`
    /// reports it. Const access through the proxy (`pos.Get().x`) does not stamp;
    /// see Mut in Query.hpp for the exact list of spellings that do.
    ///
    /// Wrap only the components you write. A Mut-wrapped read stamps too (the
    /// proxy cannot tell), which is safe but pointless replication traffic — read
    /// the rest through a plain Query alongside. Untracked components cost
    /// nothing here beyond the same TracksChanges() bool GetMut already checks.
    ///
    /// Every warning on Query applies unchanged: same iteration machinery, same
    /// mid-loop structural-mutation hazard, same debug invalidation guard.
    template <typename... Ts> QueryMutView<std::tuple<Ts...>, std::tuple<>> QueryMut()
    {
        return QueryMut<Ts...>(Without<>{});
    }

    /// @brief QueryMut with exclusions — `scene.QueryMut<A>(Without<B>{})`.
    /// Yields exactly the entity set Query<A>(Without<B>{}) does, as Mut proxies.
    template <typename... Ts, typename... Es>
    QueryMutView<std::tuple<Ts...>, std::tuple<Es...>> QueryMut(Without<Es...> without)
    {
        return MakeView<MutAccess, Ts...>(without, &_changeTick);
    }

private:
    /// @brief Shared body of Query/QueryMut: resolve the pools, reject the
    /// no-match case, and pick the pool that drives iteration.
    ///
    /// The two query flavours differ only in what their iterator yields (the
    /// Access policy) and in whether they carry the change-tick back-pointer, so
    /// pool intersection lives here once — a second copy would be free to drift
    /// away from this one's smallest-pool and null-pool handling.
    template <typename Access, typename... Ts, typename... Es>
    QueryView<std::tuple<Ts...>, std::tuple<Es...>, Access> MakeView(Without<Es...>, ChangeTickPtr<Access> changeTick)
    {
        using View = QueryView<std::tuple<Ts...>, std::tuple<Es...>, Access>;

        std::tuple<SparseSet<Ts> *...> pools = {GetPool<Ts>()...};
        std::tuple<const SparseSet<Es> *...> excluded = {GetPool<Es>()...};

        /* If any required pool is missing, there are no matching entities. A
           missing excluded pool is fine — it simply excludes nobody. */
        bool anyNull = false;
        std::apply([&](auto *... ps) { anyNull = (... || (ps == nullptr)); }, pools);
        if (anyNull)
        {
            return View{pools, nullptr, excluded, changeTick};
        }

        /* Drive iteration from the smallest required pool to minimise skipped
           entities; excluded pools never drive iteration. */
        const std::vector<Entity> *primary = nullptr;
        std::size_t minSize = SIZE_MAX;
        std::apply(
            [&](auto *... ps)
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

        return View{pools, primary, excluded, changeTick};
    }

    struct PoolStorage
    {
        void *pool = nullptr;
        void (*remove)(void *pool, Entity entity) = nullptr;
        void (*clear)(void *pool) = nullptr;
        void (*destroy)(void *pool) = nullptr;
        void (*stamp)(void *pool, Entity entity, uint64_t tick) = nullptr; // null unless the pool is tracked
        std::size_t (*size)(const void *pool) = nullptr;
        uint64_t (*changeTick)(const void *pool, Entity entity) = nullptr; // ditto; reads what stamp wrote
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

    template <typename T> static std::size_t SizeFn(const void *pool)
    {
        return static_cast<const SparseSet<T> *>(pool)->Size();
    }

    template <typename T> static uint64_t ChangeTickFn(const void *pool, Entity entity)
    {
        return static_cast<const SparseSet<T> *>(pool)->ChangeTick(entity);
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
        // .value: array index into _pools.
        if (id.value >= _pools.size() || !_pools[id.value].pool)
            return nullptr;
        return static_cast<SparseSet<T> *>(_pools[id.value].pool);
    }

    template <typename T> SparseSet<T> &GetOrCreatePool()
    {
        const Core::Reflect::ComponentId id = Core::Reflect::ComponentIdOf<T>();
        ASSISI_ASSERT(id != Core::Reflect::kInvalidComponentId,
                      "Scene component types must be registered with the reflection "
                      "system (ACOMP); this T has no ComponentId.");

        // .value throughout: array index into / size of _pools.
        if (id.value >= _pools.size())
            _pools.resize(id.value + 1);

        PoolStorage &slot = _pools[id.value];
        if (!slot.pool)
        {
            auto *pool = new SparseSet<T>();
            _registry.RegisterPool(pool);
            slot = PoolStorage{pool, &RemoveFn<T>, &ClearFn<T>, &DestroyFn<T>, nullptr, &SizeFn<T>};

            // Wire change detection for ACOMP(tracked) types. The registry is
            // finalized by the time a component is first added at runtime, so the
            // meta lookup is valid here.
            const Core::Reflect::ComponentMeta *meta = Core::Reflect::ComponentRegistry::Instance().ById(id);
            if (meta && meta->tracksChanges)
            {
                pool->SetTracksChanges(true);
                slot.stamp = &StampFn<T>;
                slot.changeTick = &ChangeTickFn<T>;
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
