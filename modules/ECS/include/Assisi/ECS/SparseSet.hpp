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
#include <cstring>
#include <expected>
#include <type_traits>
#include <vector>

#include <Assisi/ECS/Entity.hpp>

namespace Assisi::ECS
{

enum class SparseSetError
{
    AlreadyExists, ///< Returned by Add() if the entity already has a component.
};

template <typename T> struct SparseSet
{
    static_assert(std::is_trivially_copyable_v<T>, "SparseSet<T>: components must be trivially copyable. "
                                                   "Use a handle/ID to reference heap-allocated data instead.");

    /// @brief Sentinel stored in the sparse array for slots with no component.
    static constexpr uint32_t Invalid = UINT32_MAX;

    /// @brief Adds a component for the given entity.
    ///
    /// @return Pointer to the new component on success, or
    ///         SparseSetError::AlreadyExists if the entity already has one.
    [[nodiscard]] std::expected<T *, SparseSetError> Add(Entity entity, T component = {})
    {
        if (Has(entity))
            return std::unexpected(SparseSetError::AlreadyExists);

        /* Grow the sparse array to accommodate the entity index if needed. */
        if (entity.index >= _sparse.size())
            _sparse.resize(entity.index + 1, Invalid);

        /* Record where in the dense array this entity's component will live. */
        _sparse[entity.index] = static_cast<uint32_t>(_dense.size());

        /* Append the entity index and the component value. */
        _entities.push_back(entity);
        return &_dense.emplace_back(component);
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
            /* Copy the last element into the removed slot. */
            std::memcpy(&_dense[removedPos], &_dense[lastPos], sizeof(T));
            _entities[removedPos] = _entities[lastPos];

            /* Update the sparse entry for the entity that was moved. */
            _sparse[_entities[removedPos].index] = removedPos;
        }

        /* Clear the sparse entry and shrink the dense arrays. */
        _sparse[entity.index] = Invalid;
        _dense.pop_back();
        _entities.pop_back();
    }

    /// @brief Returns true if the entity has a component in this set.
    bool Has(Entity entity) const { return entity.index < _sparse.size() && _sparse[entity.index] != Invalid; }

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
