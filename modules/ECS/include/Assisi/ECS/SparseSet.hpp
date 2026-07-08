/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SparseSet.hpp
/// @brief Cache-friendly component storage for the ECS.
///
/// Maps entity indices to components using a sparse array for O(1) lookup
/// and a dense packed array for cache-friendly iteration.
///
/// Layout:
///   sparse[entity.index]  → position in dense array (INVALID if not present)
///   dense[pos]            → component value
///   entities[pos]         → entity index that owns dense[pos]
///
/// Remove() swaps the target element with the last one and pops, keeping
/// the dense array gap-free at all times.

#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

#include <Assisi/ECS/Entity.hpp>

namespace Assisi::ECS
{

template <typename T> struct SparseSet
{

    /// @brief Sentinel stored in the sparse array for slots with no component.
    static constexpr uint32_t Invalid = UINT32_MAX;

    /// @brief Adds a component for the given entity.
    ///
    /// @return Pointer to the new component, or nullptr if the add was rejected:
    ///         the entity's index slot is already occupied, either by this same
    ///         entity (duplicate) or by a different generation (a stale handle
    ///         whose slot was reused). Both mean the component was not added and
    ///         the caller can do nothing different about the distinction.
    [[nodiscard]] T *Add(Entity entity, T component = {})
    {
        /* Any occupied slot is a rejection.  Has() alone won't catch the stale
           case — it matches the full handle, so a stale handle whose slot was
           reused by a newer generation would slip past and overwrite the live
           occupant's sparse entry, orphaning its dense slot and desyncing the
           set.  Inspect the slot directly instead. */
        if (entity.index < _sparse.size())
        {
            if (_sparse[entity.index] != Invalid)
                return nullptr;
        }
        else
        {
            _sparse.resize(entity.index + 1, Invalid);
        }

        _sparse[entity.index] = static_cast<uint32_t>(_dense.size());
        _entities.push_back(entity);
        return &_dense.emplace_back(std::move(component));
    }

    /// @brief Removes the component for the given entity.
    ///
    /// Swaps the target with the last element to keep the dense array packed,
    /// then pops the last slot.  Does nothing if the entity has no component.
    void Remove(Entity entity)
    {
        if (!Has(entity))
            return;

        const uint32_t removedPos = _sparse[entity.index];
        const uint32_t lastPos = static_cast<uint32_t>(_dense.size()) - 1;

        if (removedPos != lastPos)
        {
            _dense[removedPos]    = std::move(_dense[lastPos]);
            _entities[removedPos] = _entities[lastPos];

            /* Re-point the sparse entry of the entity that was moved into the gap. */
            _sparse[_entities[removedPos].index] = removedPos;
        }

        _sparse[entity.index] = Invalid;
        _dense.pop_back();
        _entities.pop_back();
    }

    /// @brief Returns true if the entity has a component in this set.
    ///
    /// Compares the full handle (index *and* generation): a stale handle whose
    /// slot was reused by a newer entity will not match the generation stored
    /// in the dense array, so it correctly reports false instead of aliasing
    /// the new occupant's component.
    bool Has(Entity entity) const
    {
        if (entity.index >= _sparse.size())
            return false;
        const uint32_t pos = _sparse[entity.index];
        return pos != Invalid && _entities[pos].generation == entity.generation;
    }

    /// @brief Returns a pointer to the entity's component, or nullptr if not present.
    T *Get(Entity entity)
    {
        if (!Has(entity))
            return nullptr;
        return &_dense[_sparse[entity.index]];
    }

    /// @brief Returns a const pointer to the entity's component, or nullptr if not present.
    const T *Get(Entity entity) const
    {
        if (!Has(entity))
            return nullptr;
        return &_dense[_sparse[entity.index]];
    }

    /// @brief Returns the number of components currently stored.
    std::size_t Size() const { return _dense.size(); }

    /// @brief Returns true if no components are stored.
    bool Empty() const { return _dense.empty(); }

    /// @brief Iterators over the dense component array for cache-friendly iteration.
    std::vector<T>::iterator begin() { return _dense.begin(); }
    std::vector<T>::iterator end() { return _dense.end(); }
    std::vector<T>::const_iterator begin() const { return _dense.begin(); }
    std::vector<T>::const_iterator end() const { return _dense.end(); }

    /// @brief Removes all components, resetting the set to an empty state.
    void Clear()
    {
        _sparse.clear();
        _dense.clear();
        _entities.clear();
    }

    /// @brief Direct access to the packed entity array (parallel to dense).
    const std::vector<Entity> &Entities() const { return _entities; }

  private:
    std::vector<uint32_t> _sparse; ///< Indexed by entity index → dense position.
    std::vector<T> _dense;         ///< Packed component values.
    std::vector<Entity> _entities; ///< Entity that owns each dense slot.
};

} // namespace Assisi::ECS
