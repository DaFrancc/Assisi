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
#include <Assisi/Mondrian/ScaleMatch.hpp>
#include <Assisi/Mondrian/Text.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace Assisi::Mondrian
{

/// The screen UI pixels are measured against. Its height is also the length a
/// shorter side is measured against.
inline constexpr float kReferenceWidth = 1920.f;
inline constexpr float kReferenceHeight = 1080.f;

/// @brief Device pixels per UI pixel for @p viewport: the side @p match names,
/// measured against the reference, times the player's @p userScale.
[[nodiscard]] float UiScale(Extent viewport, float userScale, ScaleMatch match);

/// @brief Space inside a node's edges, resolved to device pixels.
struct Insets
{
    float left = 0.f;
    float top = 0.f;
    float right = 0.f;
    float bottom = 0.f;
};

/// @brief One node's result.
///
/// Every length the node's style gives is resolved here once, in device
/// pixels, so that nothing after layout needs the parent, the viewport or the
/// scale to know what a length meant.
struct LayoutNode
{
    static constexpr uint32_t kNoText = std::numeric_limits<uint32_t>::max();

    Rect rect;           ///< device pixels, edges whole
    Rect clip = kNoClip; ///< what the node's own quads are clipped to
    Insets padding;
    Point contentSize; ///< the extent of its content, which exceeds rect where it scrolls
    Point minSize;     ///< the least it can shrink to without clipping its content
    /// What its content measured, and the least that content shrinks to, per
    /// axis, before padding and gaps. Kept so the node's length can be worked
    /// out again once a `%` padding or gap has a parent to be a share of.
    Point content;
    Point minContent;
    /// The least and the most its sizing allows on each axis; kUnbounded where
    /// no maximum binds.
    std::array<float, kAxisCount> minLength{};
    std::array<float, kAxisCount> maxLength{kUnbounded, kUnbounded};
    float gap = 0.f;
    float textSize = 0.f;
    float borderWidth = 0.f;
    float cornerRadius = 0.f;
    float scrollBarMinLength = 0.f;
    /// How far a single line of text is shifted left to keep its caret in
    /// sight, in device pixels. Zero for everything that is not a field.
    float textScroll = 0.f;
    uint32_t text = kNoText; ///< index into LayoutResult::texts
    uint32_t generation = 0;
    uint32_t inFlowChildren = 0;
    /// Whether what was laid out is the field's placeholder rather than its
    /// text, which is drawn fainter to say it is not there yet.
    bool placeholder = false;
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
[[nodiscard]] float TextWrapWidth(const LayoutNode &placed);

/// @brief Where @p placed's text begins, in device pixels: inside its padding,
/// on whole pixels so the layout's own whole-pixel lines land on them, and
/// shifted by however far a single line has scrolled.
///
/// Drawing and hit testing both go through this, which is what keeps a caret
/// under the character it was clicked on.
[[nodiscard]] Point TextOrigin(const LayoutNode &placed);

} // namespace Assisi::Mondrian
