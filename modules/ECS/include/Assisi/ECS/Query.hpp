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

template <typename... Ts> struct QueryView
{
    std::tuple<SparseSet<Ts> *...> _pools;
    const std::vector<Entity> *_primary; ///< Entity list of the smallest pool; nullptr = no results.

    struct Sentinel
    {
    };

    struct Iterator
    {
        const std::vector<Entity> *_entities;
        std::size_t _pos;
        std::tuple<SparseSet<Ts> *...> _pools;

        bool HasAll(Entity e) const { return (... && std::get<SparseSet<Ts> *>(_pools)->Has(e)); }

        void SkipInvalid()
        {
            while (_pos < _entities->size() && !HasAll((*_entities)[_pos]))
                ++_pos;
        }

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
    };

    Iterator begin()
    {
        static const std::vector<Entity> empty;
        const std::vector<Entity> *src = _primary ? _primary : &empty;
        Iterator it{src, 0, _pools};
        it.SkipInvalid();
        return it;
    }

    Sentinel end() const { return {}; }
};

} // namespace Assisi::ECS