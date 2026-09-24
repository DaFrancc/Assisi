/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Style.hpp
/// @brief How a node sizes, arranges its children and looks.
///
/// Every length is in UI pixels, each 1/1080 of the viewport's shorter side
/// unless the game matches another (UiScale); layout multiplies by the UI scale
/// to reach device pixels.

#include <Assisi/Mondrian/DrawList.hpp>
#include <Assisi/Mondrian/Text.hpp>

#include <Assisi/Math/Color.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace Assisi::Mondrian
{

/// A maximum that never binds.
inline constexpr float kUnbounded = std::numeric_limits<float>::max();

/// Text size a style starts with, in UI pixels.
inline constexpr float kDefaultTextSize = 24.f;

/// How short a scroll bar's thumb may draw before it stops shrinking, in
/// UI pixels: enough to see, and enough to take hold of.
inline constexpr float kDefaultScrollBarMinLength = 24.f;

/// @brief The two directions a size is resolved in, and what arrays of per-axis
/// values are indexed by.
enum class Axis : uint8_t
{
    X,
    Y,
    Count
};

inline constexpr std::size_t kAxisCount = static_cast<std::size_t>(Axis::Count);

/// @brief How a node's size on one axis is decided.
enum class SizingKind : uint8_t
{
    Fit,     ///< just large enough for its content
    Grow,    ///< its content, then a share of whatever its parent has left
    Fixed,   ///< exactly its value
    Percent, ///< a fraction of its parent's content size; zero while that parent is being fitted
    Count
};

/// @brief A node's size on one axis, clamped to [min, max] whatever its kind.
struct Sizing
{
    float value = 0.f; ///< Fixed: the length. Percent: the fraction, 0 to 1. Otherwise unused.
    float min = 0.f;
    float max = kUnbounded;
    SizingKind kind = SizingKind::Fit;

    [[nodiscard]] static constexpr Sizing Fit() { return {}; }
    [[nodiscard]] static constexpr Sizing Grow() { return {.kind = SizingKind::Grow}; }
    [[nodiscard]] static constexpr Sizing Fixed(float length) { return {.value = length, .kind = SizingKind::Fixed}; }
    [[nodiscard]] static constexpr Sizing Percent(float fraction)
    {
        return {.value = fraction, .kind = SizingKind::Percent};
    }
};

/// @brief Which axis a container places its children along.
enum class Direction : uint8_t
{
    Row,    ///< left to right
    Column, ///< top to bottom
    Count
};

/// @brief Where on an axis something sits within the space it has.
enum class Alignment : uint8_t
{
    Start,
    Center,
    End,
    Count
};

/// @brief Space inside a node's edges, around its content.
struct Padding
{
    float left = 0.f;
    float top = 0.f;
    float right = 0.f;
    float bottom = 0.f;

    [[nodiscard]] static constexpr Padding All(float length) { return {length, length, length, length}; }
};

/// @brief When a scrolling node shows the bar that says where in its content it
/// is, and which a player may drag.
enum class ScrollBarVisibility : uint8_t
{
    Never,
    WhenNeeded, ///< only while some of the content is out of sight
    Always,
    Count
};

/// @brief How the content keeps up with a scroll bar being dragged.
enum class ScrollBarDrag : uint8_t
{
    FollowsPointer, ///< the content is where the thumb is, so the hand moving it never leads
    Smoothed,       ///< the content glides after the thumb, as the rest of the scrolling does
    Count
};

/// @brief What a floating node is placed against.
enum class FloatAnchor : uint8_t
{
    Parent,
    Root,
    Count
};

/// @brief Placement outside the parent's flow: the point @p attach of the node
/// lands on the point @p anchor of what it floats against, then moves by
/// @p offset. A floating node takes no space in its parent and draws over its
/// siblings.
struct Floating
{
    Point offset;
    std::array<Alignment, kAxisCount> anchor{Alignment::Start, Alignment::Start};
    std::array<Alignment, kAxisCount> attach{Alignment::Start, Alignment::Start};
    FloatAnchor target = FloatAnchor::Parent;
    bool enabled = false;
    /// Clipped by its parent's clip as an in-flow child is; otherwise unclipped.
    bool clipToParent = false;
};

/// @brief Everything about a node that is not its content.
struct Style
{
    Math::Color4<Math::ColorSpace::Srgb> background{0.f, 0.f, 0.f, 0.f};
    Math::Color4<Math::ColorSpace::Srgb> borderColor{0.f, 0.f, 0.f, 0.f};
    Math::Color4<Math::ColorSpace::Srgb> textColor{1.f, 1.f, 1.f, 1.f};
    std::array<Sizing, kAxisCount> sizing{};
    Padding padding;
    Floating floating;
    float gap = 0.f; ///< between consecutive in-flow children
    float textSize = kDefaultTextSize;
    float borderWidth = 0.f; ///< at least one device pixel once scaled, unless zero
    float cornerRadius = 0.f;
    CornerStyle cornerStyle = CornerStyle::Square;
    Direction direction = Direction::Row;
    std::array<Alignment, kAxisCount> childAlign{Alignment::Start, Alignment::Start};
    TextAlign textAlign = TextAlign::Left;
    /// How long a scrolling node takes to reach where it was sent, in seconds.
    /// Zero arrives at once; a small fraction glides instead of jumping.
    float scrollSmoothing = 0.f;
    /// The shortest its scroll bar's thumb draws, in UI pixels, so a long
    /// list keeps something to see and to grab.
    float scrollBarMinLength = kDefaultScrollBarMinLength;
    /// Scrolling on an axis lets children overflow on it rather than shrink,
    /// clips them to the node, and gives it a bar on that axis.
    std::array<bool, kAxisCount> enabledScrollBars{false, false};
    ScrollBarVisibility scrollBarVisibility = ScrollBarVisibility::Never;
    ScrollBarDrag scrollBarDrag = ScrollBarDrag::FollowsPointer;
};

} // namespace Assisi::Mondrian
