/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Entity.hpp
/// @brief Entity identifier for the ECS.
///
/// An Entity is an opaque handle composed of a slot index and a generation
/// counter.  The generation detects stale handles: if an entity is destroyed
/// and a new one reuses the same slot, old handles will no longer match the
/// registry's current generation for that slot.

#include <cstdint>
#include <functional>

namespace Assisi::ECS
{

/// @brief Reserved all-ones value used as the sentinel in an Entity's index and
/// generation fields. NullEntity is built from it, and component pools reject an
/// index equal to it — SparseSet::Add would overflow computing `index + 1`.
inline constexpr uint32_t InvalidEntityIndex = UINT32_MAX;

struct Entity
{
    uint32_t index = 0;
    uint32_t generation = 0;

    bool operator==(const Entity &) const = default;
    bool operator!=(const Entity &) const = default;

    /// @brief Returns true if this entity is not null.
    explicit operator bool() const
    {
        return index != InvalidEntityIndex || generation != InvalidEntityIndex;
    }
};

/// @brief Sentinel value representing the absence of an entity.
inline constexpr Entity NullEntity = {InvalidEntityIndex, InvalidEntityIndex};

} // namespace Assisi::ECS

/// @brief Lets an Entity be a hash-map key directly.
///
/// Both halves are part of the identity, so the two must be combined rather than
/// hashed on the index alone: a slot reused after a destroy is a *different*
/// entity, and hashing only the index would put every life of one slot in the
/// same bucket. Packing them into one 64-bit value is exact — the fields are
/// 32 bits each — so this loses nothing and collides no more than the underlying
/// integer hash does.
template <> struct std::hash<Assisi::ECS::Entity>
{
    std::size_t operator()(const Assisi::ECS::Entity &entity) const noexcept
    {
        return std::hash<std::uint64_t>{}(static_cast<std::uint64_t>(entity.index) |
                                          (static_cast<std::uint64_t>(entity.generation) << 32));
    }
};