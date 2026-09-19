/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Navigation.hpp
/// @brief Where focus goes: in tree order for Tab, and across the screen for
/// the directions, as the last layout placed the tree.

#include <Assisi/Mondrian/Layout.hpp>
#include <Assisi/Mondrian/NodeTree.hpp>

#include <cstdint>

namespace Assisi::Mondrian
{

/// How much a candidate's sideways distance counts against it, relative to its
/// distance in the direction moved. Above one, a node straight ahead beats a
/// nearer one off to the side.
inline constexpr float kNavSidewaysWeight = 2.f;

/// @brief Whether moving past the last node in a direction comes round to the
/// first on the far side.
enum class NavWrap : uint8_t
{
    Stop,
    Around,
    Count
};

/// @brief Which way Tab order runs.
enum class TabOrder : uint8_t
{
    Forward,
    Backward,
    Count
};

/// @brief Whether @p id can take focus now: alive, placed, focusable and enabled.
[[nodiscard]] bool CanFocus(const NodeTree &tree, const LayoutResult &layout, NodeId id);

/// @brief The first node, in tree order, that can take focus, or null.
[[nodiscard]] NodeId FirstFocusable(const NodeTree &tree, const LayoutResult &layout);

/// @brief The node after @p from in tree order that can take focus, wrapping
/// at the ends; the first one when @p from is null. Null when none can.
[[nodiscard]] NodeId NextFocusable(const NodeTree &tree, const LayoutResult &layout, NodeId from, TabOrder order);

/// @brief Where focus goes moving @p direction from @p from: its override if
/// that can take focus, otherwise the nearest node that can, ahead of it in
/// that direction. The nearest is by distance ahead plus kNavSidewaysWeight
/// times the gap sideways, with ties going to the closer centre. With nothing
/// ahead, @p wrap decides between null and the nearest node from the far side.
[[nodiscard]] NodeId Neighbour(const NodeTree &tree, const LayoutResult &layout, NodeId from, NavDirection direction,
                               NavWrap wrap);

/// @brief Scrolls every scrolling ancestor of @p id just far enough to show it.
void ScrollIntoView(NodeTree &tree, const LayoutResult &layout, NodeId id);

} // namespace Assisi::Mondrian
