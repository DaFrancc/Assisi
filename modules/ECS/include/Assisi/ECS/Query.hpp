/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Query.hpp
/// @brief Multi-component query view for iterating entities that match a component signature.
///
/// Returned by Scene::Query<Ts...>(). Iterates the smallest matching pool and skips
/// entities absent from the others (or present in an excluded pool), yielding
/// (Entity, Ts&...) as a structured binding.
///
/// Example:
/// @code
///   for (auto [e, pos, vel] : scene.Query<Position, Velocity>())
///       pos.x += vel.x;
///   // entities with Position but not Frozen:
///   for (auto [e, pos] : scene.Query<Position>(Without<Frozen>{}))
///       pos.x += 1.0f;
/// @endcode

#include <cstddef>
#include <cstdint>
#include <tuple>
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

/// @brief Lazy view over entities matching a component signature.
///
/// Parameterised on a tuple of required component types and a tuple of excluded
/// ones. Iteration yields `(Entity, Ts&...)` for the required types — the
/// component references are mutable so systems can write component data in
/// place; excluded types only gate membership and are never yielded. The view
/// keeps its pool pointers private: they are the structural handles that could
/// add or remove components behind Scene's liveness gate. Only Scene constructs
/// a view (via `Scene::Query`), so there is no public path to a mutable pool
/// pointer.
template <typename Required, typename Excluded> struct QueryView;

template <typename... Ts, typename... Es> struct QueryView<std::tuple<Ts...>, std::tuple<Es...>>
{
    struct Sentinel
    {
    };

    struct Iterator
    {
        std::tuple<Entity, Ts &...> operator*() const
        {
            CheckNotInvalidated();
            return std::tuple<Entity, Ts &...>{(*_entities)[_pos], *std::get<Ts *>(_components)...};
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
                 std::tuple<const SparseSet<Es> *...> excluded)
            : _entities(entities), _pos(pos), _required(required), _excluded(excluded)
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
            return std::apply([&](auto *...ps) { return (false || ... || (ps && ps->Has(e))); }, _excluded);
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
            std::apply([&](auto *...ps) { ((sum += (ps != nullptr) ? ps->StructureVersion() : 0u), ...); },
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
        /// Required-pool version sum at construction; read only by the debug check.
        uint32_t _versionSnapshot = 0;
    };

    Iterator begin()
    {
        static const std::vector<Entity> empty;
        return Iterator{_primary ? _primary : &empty, 0, _required, _excluded};
    }

    Sentinel end() const { return {}; }

  private:
    friend struct Scene; // QueryView exists only to be returned by Scene::Query

    QueryView(std::tuple<SparseSet<Ts> *...> required, const std::vector<Entity> *primary,
              std::tuple<const SparseSet<Es> *...> excluded)
        : _required(required), _primary(primary), _excluded(excluded)
    {
    }

    std::tuple<SparseSet<Ts> *...> _required;
    const std::vector<Entity> *_primary; ///< Entity list of the smallest required pool; nullptr = no results.
    std::tuple<const SparseSet<Es> *...> _excluded;
};

} // namespace Assisi::ECS
