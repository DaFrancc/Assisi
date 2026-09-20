/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Layout.hpp
/// @brief Where every node goes: a pure function of the tree, the viewport and
/// the scale, recomputed in full whenever it runs.
///
/// The passes run in this order, and the order is the design: widths are fitted
/// bottom-up and distributed top-down, text wraps at the widths that produced,
/// and only then are heights fitted and distributed, because wrapped text needs
/// its width before its height. Positions follow, and edges snap to whole
/// device pixels last, so a row of fractional sizes does not drift.

#include <Assisi/Mondrian/DrawList.hpp>
#include <Assisi/Mondrian/Font.hpp>
#include <Assisi/Mondrian/NodeTree.hpp>
#include <Assisi/Mondrian/Text.hpp>

#include <cstdint>
#include <limits>
#include <vector>

namespace Assisi::Mondrian
{

/// The screen that logical pixels are measured against.
inline constexpr float kReferenceWidth = 1920.f;
inline constexpr float kReferenceHeight = 1080.f;

/// @brief Device pixels per logical pixel for @p viewport: the reference screen
/// fitted inside it, times the player's @p userScale.
[[nodiscard]] float UiScale(Extent viewport, float userScale);

/// @brief One node's result.
struct LayoutNode
{
    static constexpr uint32_t kNoText = std::numeric_limits<uint32_t>::max();

    Rect rect;           ///< device pixels, edges whole
    Rect clip = kNoClip; ///< what the node's own quads are clipped to
    Point contentSize;   ///< the extent of its content, which exceeds rect where it scrolls
    Point minSize;       ///< the least it can shrink to without clipping its content
    /// How far a single line of text is shifted left to keep its caret in
    /// sight, in device pixels. Zero for everything that is not a field.
    float textScroll = 0.f;
    uint32_t text = kNoText; ///< index into LayoutResult::texts
    uint32_t generation = 0;
    bool placed = false; ///< false for a free slot and a hidden subtree
};

/// @brief Every node's result, by slot. Kept and refilled rather than rebuilt,
/// so a frame that lays out allocates only for its text.
struct LayoutResult
{
    std::vector<LayoutNode> nodes;
    std::vector<TextLayout> texts;
    float scale = 0.f;

    /// @brief @p id's result, or null when it was not placed.
    [[nodiscard]] const LayoutNode *Get(NodeId id) const;
};

/// @brief Lays @p tree out in @p viewport at @p scale into @p out. Text is set in
/// @p font; with no font, text takes no space and draws nothing.
void ComputeLayout(const NodeTree &tree, Extent viewport, float scale, const Font *font, LayoutResult &out);

/// @brief The width @p placed's text wraps to, in device pixels: what is left
/// inside its padding, and never less than a pixel so that a node squeezed to
/// nothing still sets its text a glyph to a line.
///
/// Anything asking what text would do in a node goes through this, so that the
/// answer cannot differ from what layout itself used.
[[nodiscard]] float TextWrapWidth(const LayoutNode &placed, const Style &style, float scale);

/// @brief Where @p placed's text begins, in device pixels: inside its padding,
/// on whole pixels so the layout's own whole-pixel lines land on them, and
/// shifted by however far a single line has scrolled.
///
/// Drawing and hit testing both go through this, which is what keeps a caret
/// under the character it was clicked on.
[[nodiscard]] Point TextOrigin(const LayoutNode &placed, const Style &style, float scale);

} // namespace Assisi::Mondrian
