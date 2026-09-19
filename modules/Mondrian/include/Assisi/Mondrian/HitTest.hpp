/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file HitTest.hpp
/// @brief Which node the pointer is over, as the last layout placed the tree.

#include <Assisi/Mondrian/DrawList.hpp>
#include <Assisi/Mondrian/Layout.hpp>
#include <Assisi/Mondrian/NodeTree.hpp>

namespace Assisi::Mondrian
{

/// @brief The topmost node at @p point, in device pixels, that stops the
/// pointer — a focusable one, disabled or not, or one that blocks — or null.
///
/// Topmost is the reverse of draw order. A node is hit inside its rect as
/// clipped by its ancestors, so a floating node escapes their clip unless it
/// clips to its parent, and content scrolled out of view is not hit. Nodes
/// that stop nothing are looked through, to whatever lies beneath.
[[nodiscard]] NodeId HitTest(const NodeTree &tree, const LayoutResult &layout, Point point);

} // namespace Assisi::Mondrian
