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

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <vector>

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
#ifndef NDEBUG
            CheckNotInvalidated();
#endif
            return std::tuple<Entity, Ts &...>{(*_entities)[_pos], *std::get<Ts *>(_components)...};
        }

        Iterator &operator++()
        {
#ifndef NDEBUG
            CheckNotInvalidated();
#endif
            ++_pos;
            SkipInvalid();
            return *this;
        }

        bool operator==(Sentinel) const { return _pos >= _entities->size(); }
        bool operator!=(Sentinel s) const { return !(*this == s); }

      private:
        friend QueryView; // only its enclosing view constructs iterators

        Iterator(const std::vector<Entity> *entities, std::size_t pos, std::tuple<SparseSet<Ts> *...> required,
                 std::tuple<const SparseSet<Es> *...> excluded)
            : _entities(entities), _pos(pos), _required(required), _excluded(excluded)
        {
#ifndef NDEBUG
            _versionSnapshot = CurrentStructureVersion();
#endif
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

#ifndef NDEBUG
        // Sum of the structural-version counters of every pool this iterator
        // reads. Versions only ever increase, so any Add/Remove/Clear on any of
        // these pools strictly raises the sum — one integer compare in
        // CheckNotInvalidated() then detects a mid-iteration mutation that would
        // have reallocated the dense/entity arrays out from under us. A missing
        // (null) required or excluded pool contributes nothing.
        uint32_t CurrentStructureVersion() const
        {
            uint32_t sum = 0;
            std::apply([&](auto *...ps) { ((sum += (ps != nullptr) ? ps->StructureVersion() : 0u), ...); },
                       _required);
            std::apply([&](auto *...ps) { ((sum += (ps != nullptr) ? ps->StructureVersion() : 0u), ...); },
                       _excluded);
            return sum;
        }

        void CheckNotInvalidated() const
        {
            assert(CurrentStructureVersion() == _versionSnapshot &&
                   "structural change (Add/Remove on a queried component pool) during Query "
                   "iteration invalidated the iterator. Destroy is already deferred and safe here; "
                   "for Add/Remove, collect the entities and apply the change after the loop.");
        }
#endif

        const std::vector<Entity> *_entities;
        std::size_t _pos;
        std::tuple<SparseSet<Ts> *...> _required;
        std::tuple<const SparseSet<Es> *...> _excluded;
        std::tuple<Ts *...> _components{}; ///< Cached by HasAll; valid only while the iterator is dereferenceable.
#ifndef NDEBUG
        uint32_t _versionSnapshot = 0; ///< Pool version sum at construction; see CheckNotInvalidated().
#endif
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
