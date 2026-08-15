/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Query.hpp
/// @brief Multi-component query view for iterating entities that match a component signature.
///
/// Returned by Scene::Query<Ts...>() and Scene::QueryMut<Ts...>(). Iterates the
/// smallest matching pool and skips entities absent from the others (or present
/// in an excluded pool), yielding (Entity, Ts&...) — or (Entity, Mut<Ts>...) for
/// the stamping variant — as a structured binding.
///
/// Example:
/// @code
///   for (auto [e, pos, vel] : scene.Query<Position, Velocity>())
///       pos.x += vel.x;
///   // entities with Position but not Frozen:
///   for (auto [e, pos] : scene.Query<Position>(Without<Frozen>{}))
///       pos.x += 1.0f;
/// @endcode
///
/// @warning Plain `Query` yields raw `Ts&`, which is the ergonomic path for
/// reads and for writing *untracked* components — but writing an ACOMP(tracked)
/// component through it is a **silently missed change**: nothing stamps the
/// pool's change tick, so `Scene::Changed<T>(e, since)` keeps reporting false
/// and every consumer that filters on it (transform propagation, network
/// replication) skips the entity. Use `Scene::QueryMut<Ts...>()` for the types
/// you write; it yields `Mut<T>` proxies that stamp exactly like
/// `Scene::GetMut`. Wrap only the types you actually write — a `Mut`-wrapped
/// read stamps too (safe over-reporting, but pointless traffic), so read the
/// rest through a plain `Query` alongside.

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <vector>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/ECS/SparseSet.hpp>

namespace Assisi::ECS
{

struct Scene; // sole constructor of QueryView — see friend declaration below

/// @brief Exclusion tag passed to Scene::Query to reject entities holding any Es.
///
/// `scene.Query<A>(Without<B>{})` yields entities with A but not B. An excluded
/// component whose pool has never been created excludes nobody — there are no
/// holders to reject — so a missing excluded pool is simply a no-op filter.
template <typename... Es> struct Without
{
};

// ── Query access policies ─────────────────────────────────────────────────────
// Tag types selecting what a QueryView's iterator yields per required component.
// They exist so both query flavours share one piece of iteration machinery (pool
// intersection, smallest-pool driving, the debug invalidation guard) and differ
// only in operator*'s return — a copy-pasted second view would drift.

/// @brief Yield raw `T&` (Scene::Query). No change-tick stamping.
struct RefAccess
{
};

/// @brief Yield `Mut<T>` write proxies (Scene::QueryMut). Stamps on mutable access.
struct MutAccess
{
};

/// @brief Placeholder for the scene change-tick back-pointer in a RefAccess view.
///
/// A plain Query has nothing to stamp, so it must not carry the pointer. Stored
/// via [[no_unique_address]], this empty type costs the iterator zero bytes, so
/// the non-stamping hot path is no wider for QueryMut's sake.
struct NoChangeTick
{
};

/// @brief The change-tick back-pointer a view of this access policy needs.
template <typename Access>
using ChangeTickPtr = std::conditional_t<std::is_same_v<Access, MutAccess>, uint64_t *, NoChangeTick>;

/// @brief Write proxy over one entity's component that stamps change detection.
///
/// Yielded by `Scene::QueryMut<Ts...>()` in place of a bare `T&`. It holds the
/// component, its pool, the owning entity, and a pointer to the Scene's change
/// tick counter — everything `Scene::GetMut` needs — so a mutable access through
/// the proxy is indistinguishable from a `GetMut` call: allocate the next tick
/// (`++tick`) and stamp it onto the pool's per-entity lane.
///
/// Which spellings stamp:
/// @code
///   for (auto [e, pos] : scene.QueryMut<Position>())
///   {
///       pos->x += 1.f;         // stamps (non-const operator->)
///       (*pos).x  = 2.f;       // stamps (non-const operator*)
///       Position &p = pos;     // stamps (implicit operator T&)
///       Move(pos);             // stamps if Move takes Position& — otherwise not
///
///       float x = pos.Get().x; // does NOT stamp (const accessor)
///       pos.MarkChanged();     // stamps without touching the value
///   }
/// @endcode
/// The rule is the ordinary const rule: every non-const accessor stamps, every
/// const accessor does not. Conversion to `const T&` from a non-const proxy
/// still picks the non-const `operator T&` (better implicit-object match) and so
/// stamps — deliberately conservative, matching `Scene::GetMut`, which stamps on
/// access rather than on an actual value change. Over-reporting is safe by the
/// engine's change-detection stance; a missed change is not.
///
/// Stamping is gated on `SparseSet::TracksChanges()`, so an untracked component
/// pays one already-hot bool load and never burns a tick — same gate, same cost
/// as `Scene::GetMut`.
///
/// @warning The proxy is a borrowed view, valid only for the loop iteration that
/// produced it: it caches the component pointer, which any structural change to
/// the pool (Add/Remove) can dangle. Do not store one past the loop body.
template <typename T> struct Mut
{
    // ── Mutable access: stamps ────────────────────────────────────────────────
    T &operator*() { Stamp(); return *_component; }
    T *operator->() { Stamp(); return _component; }

    /// @brief Implicit conversion to `T&`, so a `Mut<T>` passes to anything
    /// taking the component by mutable reference and reads as one in
    /// expressions. Stamps, like every other mutable accessor.
    operator T &() { Stamp(); return *_component; } // NOLINT(google-explicit-constructor)

    /// @brief Explicit mutable access — the spelling to reach for when the
    /// conversion is ambiguous or unclear at the call site. Stamps.
    T &GetMut() { Stamp(); return *_component; }

    /// @brief Stamps without touching the value. For a write the proxy cannot
    /// see (e.g. one made through a pointer captured earlier), or to force
    /// re-processing of an otherwise untouched component.
    void MarkChanged() { Stamp(); }

    // ── Const access: never stamps ────────────────────────────────────────────
    const T &operator*() const { return *_component; }
    const T *operator->() const { return _component; }
    operator const T &() const { return *_component; } // NOLINT(google-explicit-constructor)

    /// @brief Read-only access, the spelling that documents intent: `pos.Get().x`
    /// never stamps. Const-qualified, so it is also what a `const Mut<T>` sees.
    const T &Get() const { return *_component; }

    /// @brief The entity this proxy's component belongs to.
    Entity Owner() const { return _entity; }

private:
    // Only a query view builds proxies: doing so requires a mutable pool pointer,
    // which the views keep private precisely so nothing outside Scene can perform
    // structural changes behind its liveness gate.
    template <typename Required, typename Excluded, typename Access> friend struct QueryView;

    Mut(T *component, SparseSet<T> *pool, Entity entity, uint64_t *changeTick)
        : _component(component), _pool(pool), _entity(entity), _changeTick(changeTick)
    {
    }

    /// Mirrors Scene::GetMut exactly, including the order of operations: check
    /// the pool's opt-in first, and only then burn a tick. Bumping unconditionally
    /// would advance the scene's tick for untracked writes, inflating every
    /// consumer's bookmark against changes that were never recorded.
    void Stamp() const
    {
        if (_pool->TracksChanges())
            _pool->Stamp(_entity, ++*_changeTick);
    }

    T *_component;
    SparseSet<T> *_pool;
    Entity _entity;
    uint64_t *_changeTick; ///< &Scene::_changeTick — the sole allocator of ticks.
};

/// @brief Lazy view over entities matching a component signature.
///
/// Parameterised on a tuple of required component types, a tuple of excluded
/// ones, and an access policy deciding what iteration yields per required type:
/// `RefAccess` (Scene::Query) yields `Ts&`, `MutAccess` (Scene::QueryMut) yields
/// `Mut<Ts>` proxies that stamp change detection. Everything else — pool
/// intersection, driving from the smallest pool, the debug invalidation guard —
/// is shared; only `operator*` differs. Excluded types only gate membership and
/// are never yielded.
///
/// The view keeps its pool pointers private: they are the structural handles that
/// could add or remove components behind Scene's liveness gate. Only Scene
/// constructs a view (via `Scene::Query`/`Scene::QueryMut`), so there is no
/// public path to a mutable pool pointer.
///
/// @warning `RefAccess` yields *mutable* `Ts&`, but writing an ACOMP(tracked)
/// component through them does not stamp — see the file-header warning and use
/// `Scene::QueryMut` for tracked writes.
template <typename Required, typename Excluded, typename Access = RefAccess> struct QueryView;

template <typename... Ts, typename... Es, typename Access>
struct QueryView<std::tuple<Ts...>, std::tuple<Es...>, Access>
{
    /// What one required component yields per the access policy.
    template <typename T> using Yielded = std::conditional_t<std::is_same_v<Access, MutAccess>, Mut<T>, T &>;

    struct Sentinel
    {
    };

    struct Iterator
    {
        std::tuple<Entity, Yielded<Ts>...> operator*() const
        {
            CheckNotInvalidated();
            const Entity entity = (*_entities)[_pos];
            if constexpr (std::is_same_v<Access, MutAccess>)
            {
                // Proxies are built per dereference from the pointers HasAll
                // already cached, so a stamping query costs the same lookups as a
                // plain one — the proxy is three extra pointer copies, and the
                // tick bump only happens if the loop body actually writes.
                return std::tuple<Entity, Mut<Ts>...>{
                    entity, Mut<Ts>{std::get<Ts *>(_components), std::get<SparseSet<Ts> *>(_required), entity,
                                    _changeTick} ...};
            }
            else
            {
                return std::tuple<Entity, Ts &...>{entity, *std::get<Ts *>(_components)...};
            }
        }

        Iterator &operator++()
        {
            CheckNotInvalidated();
            ++_pos;
            SkipInvalid();
            return *this;
        }

        // Deliberately no invalidation check here: _entities points at a stable
        // member vector of the pool, so reading its size is always safe. The
        // trade-off is that a structural change made right before the loop's
        // exit test can end the loop instead of asserting — a missed detection,
        // not UB; operator*/operator++ catch it on any continued use.
        bool operator==(Sentinel) const { return _pos >= _entities->size(); }
        bool operator!=(Sentinel s) const { return !(*this == s); }

private:
        friend QueryView; // only its enclosing view constructs iterators

        Iterator(const std::vector<Entity> *entities, std::size_t pos, std::tuple<SparseSet<Ts> *...> required,
                 std::tuple<const SparseSet<Es> *...> excluded, ChangeTickPtr<Access> changeTick)
            : _entities(entities), _pos(pos), _required(required), _excluded(excluded), _changeTick(changeTick)
        {
            _versionSnapshot = CurrentStructureVersion();
            SkipInvalid();
        }

        /// Membership test and component fetch fused into one pass: Get() is
        /// Has() plus the dense lookup, so asking Has() here and Get() again in
        /// operator* would run every sparse lookup twice. The fetched pointers
        /// are cached for operator* to dereference; they stay valid because the
        /// queried pools must not be structurally mutated during iteration
        /// (the documented Scene::Query contract).
        bool HasAll(Entity e)
        {
            return (... && ((std::get<Ts *>(_components) = std::get<SparseSet<Ts> *>(_required)->Get(e)) != nullptr));
        }

        /// A null excluded pool has no holders, so it rejects nobody.
        bool HasExcluded(Entity e) const
        {
            return std::apply([&](auto *... ps) { return (false || ... || (ps && ps->Has(e))); }, _excluded);
        }

        /// HasAll must run first: operator* trusts the pointer cache only when
        /// the entity matched, and short-circuiting skips the exclusion probe
        /// for entities that already failed the positive match.
        bool Matches(Entity e) { return HasAll(e) && !HasExcluded(e); }

        void SkipInvalid()
        {
            while (_pos < _entities->size() && !Matches((*_entities)[_pos]))
                ++_pos;
        }

        // Sum of the structural-version counters of the *required* pools — the
        // only ones whose reallocation can invalidate this iterator: it drives
        // iteration over a required pool's entity array (_entities) and caches
        // component pointers from the required pools (_components). Excluded
        // pools are deliberately NOT summed: HasExcluded re-probes them each step
        // through their stable pool address, so mutating an excluded pool
        // mid-iteration is safe and must not trip the check (false positive).
        // Versions only ever increase, so any Add/Remove/Clear on a required pool
        // strictly raises the sum, and one integer compare catches it. The
        // per-pool counters exist only in debug (see SparseSet::StructureVersion),
        // so this returns 0 in release, where CheckNotInvalidated's ASSISI_ASSERT
        // compiles away and never calls it from the hot path — only the ctor does.
        uint32_t CurrentStructureVersion() const
        {
#ifndef NDEBUG
            uint32_t sum = 0;
            std::apply([&](auto *... ps) { ((sum += (ps != nullptr) ? ps->StructureVersion() : 0u), ...); },
                       _required);
            return sum;
#else
            return 0;
#endif
        }

        // ASSISI_ASSERT evaluates nothing in release (its arguments sit under an
        // unevaluated sizeof), so this is an empty call there — hence no #ifndef
        // at the call sites in operator*/operator++.
        void CheckNotInvalidated() const
        {
            ASSISI_ASSERT(CurrentStructureVersion() == _versionSnapshot,
                          "structural change (Add/Remove on a queried component pool) during Query "
                          "iteration invalidated the iterator. Destroy is already deferred and safe "
                          "here; for Add/Remove, collect the entities and apply the change after the "
                          "loop.");
        }

        const std::vector<Entity> *_entities;
        std::size_t _pos;
        std::tuple<SparseSet<Ts> *...> _required;
        std::tuple<const SparseSet<Es> *...> _excluded;
        std::tuple<Ts *...> _components{}; ///< Cached by HasAll; valid only while the iterator is dereferenceable.
        /// Scene's change-tick counter, for the proxies operator* mints. Empty
        /// (and free) under RefAccess — see NoChangeTick.
        [[no_unique_address]] ChangeTickPtr<Access> _changeTick;
        /// Required-pool version sum at construction; read only by the debug check.
        uint32_t _versionSnapshot = 0;
    };

    Iterator begin()
    {
        static const std::vector<Entity> empty;
        return Iterator{_primary ? _primary : &empty, 0, _required, _excluded, _changeTick};
    }

    Sentinel end() const { return {}; }

private:
    friend struct Scene; // QueryView exists only to be returned by Scene::Query/QueryMut

    QueryView(std::tuple<SparseSet<Ts> *...> required, const std::vector<Entity> *primary,
              std::tuple<const SparseSet<Es> *...> excluded, ChangeTickPtr<Access> changeTick)
        : _required(required), _primary(primary), _excluded(excluded), _changeTick(changeTick)
    {
    }

    std::tuple<SparseSet<Ts> *...> _required;
    const std::vector<Entity> *_primary; ///< Entity list of the smallest required pool; nullptr = no results.
    std::tuple<const SparseSet<Es> *...> _excluded;
    [[no_unique_address]] ChangeTickPtr<Access> _changeTick; ///< Passed to each Iterator; empty under RefAccess.
};

/// @brief Shorthand for the view type `Scene::QueryMut` returns.
template <typename Required, typename Excluded> using QueryMutView = QueryView<Required, Excluded, MutAccess>;

} // namespace Assisi::ECS
