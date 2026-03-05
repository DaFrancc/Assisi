#pragma once

/// @file Entity.hpp
/// @brief Entity identifier for the ECS.
///
/// An Entity is an opaque handle composed of a slot index and a generation
/// counter.  The generation detects stale handles: if an entity is destroyed
/// and a new one reuses the same slot, old handles will no longer match the
/// registry's current generation for that slot.

#include <cstdint>

namespace Assisi::ECS
{

struct Entity
{
    uint32_t index = 0;
    uint32_t generation = 0;

    bool operator==(const Entity &) const = default;
    bool operator!=(const Entity &) const = default;

    /// @brief Returns true if this entity is not null.
    explicit operator bool() const { return index != UINT32_MAX || generation != UINT32_MAX; }
};

/// @brief Sentinel value representing the absence of an entity.
inline constexpr Entity NullEntity = {UINT32_MAX, UINT32_MAX};

} // namespace Assisi::ECS