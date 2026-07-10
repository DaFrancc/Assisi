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
            Entity e = (*_entities)[_pos];
            return std::tuple<Entity, Ts &...>{e, *std::get<SparseSet<Ts> *>(_required)->Get(e)...};
        }

        Iterator &operator++()
        {
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
            SkipInvalid();
        }

        bool HasAll(Entity e) const { return (... && std::get<SparseSet<Ts> *>(_required)->Has(e)); }

        /// A null excluded pool has no holders, so it rejects nobody.
        bool HasExcluded(Entity e) const
        {
            return std::apply([&](auto *...ps) { return (false || ... || (ps && ps->Has(e))); }, _excluded);
        }

        bool Matches(Entity e) const { return HasAll(e) && !HasExcluded(e); }

        void SkipInvalid()
        {
            while (_pos < _entities->size() && !Matches((*_entities)[_pos]))
                ++_pos;
        }

        const std::vector<Entity> *_entities;
        std::size_t _pos;
        std::tuple<SparseSet<Ts> *...> _required;
        std::tuple<const SparseSet<Es> *...> _excluded;
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
