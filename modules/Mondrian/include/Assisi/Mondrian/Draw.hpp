/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Draw.hpp
/// @brief A laid-out tree as quads: each node's box, image and text, parents
/// before children and floating children after the rest, so later draws cover
/// earlier ones.

#include <Assisi/Mondrian/DrawList.hpp>
#include <Assisi/Mondrian/Input.hpp>
#include <Assisi/Mondrian/Layout.hpp>
#include <Assisi/Mondrian/NodeTree.hpp>

namespace Assisi::Mondrian
{

/// The thinnest a border draws once scaled, so a one-pixel line survives a
/// small screen.
inline constexpr float kMinBorderDevicePixels = 1.f;

/// @brief Adds @p tree's quads to @p list as @p layout placed them, with text
/// from the font atlas registered as @p fontAtlas. Each control draws its own
/// parts over its node's, told by @p interaction which one is pressed or hovered.
void DrawTree(const NodeTree &tree, const LayoutResult &layout, DrawList &list, TextureId fontAtlas,
              const Interaction &interaction);

} // namespace Assisi::Mondrian
