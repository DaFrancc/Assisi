/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Query.hpp
/// @brief Multi-component query view for iterating entities that match a component signature.
///
/// Returned by Scene::Query<Ts...>(). Iterates the smallest matching pool and skips
/// entities absent from the others, yielding (Entity, Ts&...) as a structured binding.
///
/// Example:
/// @code
///   for (auto [e, pos, vel] : scene.Query<Position, Velocity>())
///       pos.x += vel.x;
/// @endcode

#include <cstddef>
#include <tuple>
#include <vector>

#include <Assisi/ECS/SparseSet.hpp>

namespace Assisi::ECS
{

struct Scene; // sole constructor of QueryView — see friend declaration below

/// @brief Lazy view over entities matching a component signature.
///
/// Iteration yields `(Entity, Ts&...)` — the component references are mutable
/// so systems can write component data in place. The view keeps its pool
/// pointers private: they are the structural handles that could add or remove
/// components behind Scene's liveness gate. Only Scene constructs a view (via
/// `Scene::Query`), so there is no public path to a mutable pool pointer.
template <typename... Ts> struct QueryView
{
    struct Sentinel
    {
    };

    struct Iterator
    {
        std::tuple<Entity, Ts &...> operator*() const
        {
            Entity e = (*_entities)[_pos];
            return std::tuple<Entity, Ts &...>{e, *std::get<SparseSet<Ts> *>(_pools)->Get(e)...};
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
        friend struct QueryView; // only its enclosing view constructs iterators

        Iterator(const std::vector<Entity> *entities, std::size_t pos,
                 std::tuple<SparseSet<Ts> *...> pools)
            : _entities(entities), _pos(pos), _pools(pools)
        {
            SkipInvalid();
        }

        bool HasAll(Entity e) const { return (... && std::get<SparseSet<Ts> *>(_pools)->Has(e)); }

        void SkipInvalid()
        {
            while (_pos < _entities->size() && !HasAll((*_entities)[_pos]))
                ++_pos;
        }

        const std::vector<Entity> *_entities;
        std::size_t _pos;
        std::tuple<SparseSet<Ts> *...> _pools;
    };

    Iterator begin()
    {
        static const std::vector<Entity> empty;
        return Iterator{_primary ? _primary : &empty, 0, _pools};
    }

    Sentinel end() const { return {}; }

  private:
    friend struct Scene; // QueryView exists only to be returned by Scene::Query

    QueryView(std::tuple<SparseSet<Ts> *...> pools, const std::vector<Entity> *primary)
        : _pools(pools), _primary(primary)
    {
    }

    std::tuple<SparseSet<Ts> *...> _pools;
    const std::vector<Entity> *_primary; ///< Entity list of the smallest pool; nullptr = no results.
};

} // namespace Assisi::ECS