/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file NodeId.hpp
/// @brief What names a node, on its own so the pieces that pass ids around do
/// not have to include the tree that owns them.

#include <cstdint>
#include <limits>

namespace Assisi::Mondrian
{

/// @brief A node, by slot and generation. The generation moves on every destroy,
/// so an id that outlived its node matches nothing.
struct NodeId
{
    static constexpr uint32_t kNullIndex = std::numeric_limits<uint32_t>::max();

    uint32_t index = kNullIndex;
    uint32_t generation = 0;

    bool operator==(const NodeId &) const = default;
    [[nodiscard]] explicit operator bool() const { return index != kNullIndex; }
};

} // namespace Assisi::Mondrian
