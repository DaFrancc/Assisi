/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Style.hpp
/// @brief How a node sizes, arranges its children and looks.
///
/// Every length is a value and a unit: UI pixels, each 1/1080 of the viewport's
/// shorter side unless the game matches another (UiScale), or a share of
/// something layout knows — the parent, the viewport, the text size. Layout
/// resolves each one once per node into device pixels, on the LayoutNode, and
/// everything after layout reads those.

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

/// What a percentage is out of: a length of `Percent(50)` is half.
inline constexpr float kPercentOf = 100.f;

/// @brief What a length is measured in.
enum class LengthUnit : uint8_t
{
    Px,      ///< UI pixels
    Percent, ///< of the parent's content size on the same axis; see Length
    Vw,      ///< hundredths of the viewport's width
    Vh,      ///< hundredths of the viewport's height
    Em,      ///< multiples of the node's own text size
    Count
};

/// @brief A length and what it is measured in.
///
/// `%` is of the parent's content size on the length's axis, and counts as
/// nothing while that parent is still being sized to fit its content — a share
/// of a size that depends on the share has no answer. Lengths with no axis of
/// their own (border width, corner radius, scroll bar length) take `%` of the
/// node's own shorter side, so a corner radius of 50% makes a pill. A text
/// size's own `%` and `em` are of its parent's text size, as in CSS.
struct Length
{
    float value = 0.f;
    LengthUnit unit = LengthUnit::Px;

    [[nodiscard]] friend constexpr bool operator==(const Length &, const Length &) = default;
};

[[nodiscard]] constexpr Length Px(float value)
{
    return {.value = value, .unit = LengthUnit::Px};
}
[[nodiscard]] constexpr Length Percent(float value)
{
    return {.value = value, .unit = LengthUnit::Percent};
}
[[nodiscard]] constexpr Length Vw(float value)
{
    return {.value = value, .unit = LengthUnit::Vw};
}
[[nodiscard]] constexpr Length Vh(float value)
{
    return {.value = value, .unit = LengthUnit::Vh};
}
[[nodiscard]] constexpr Length Em(float value)
{
    return {.value = value, .unit = LengthUnit::Em};
}

/// Text size a style starts with, and what the root's text size is relative
/// to, in UI pixels.
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
    Fit,   ///< just large enough for its content
    Grow,  ///< its content, then a share of whatever its parent has left
    Fixed, ///< exactly its value, in whatever unit it is written
    Count
};

/// @brief A node's size on one axis, clamped to [min, max] whatever its kind.
struct Sizing
{
    Length value; ///< Fixed: the length. Otherwise unused.
    Length min;
    Length max = Px(kUnbounded);
    SizingKind kind = SizingKind::Fit;

    [[nodiscard]] static constexpr Sizing Fit() { return {}; }
    [[nodiscard]] static constexpr Sizing Grow()
    {
        return {.value = {}, .min = {}, .max = Px(kUnbounded), .kind = SizingKind::Grow};
    }
    [[nodiscard]] static constexpr Sizing Fixed(Length length)
    {
        return {.value = length, .min = {}, .max = Px(kUnbounded), .kind = SizingKind::Fixed};
    }

    [[nodiscard]] friend constexpr bool operator==(const Sizing &, const Sizing &) = default;
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
    Length left;
    Length top;
    Length right;
    Length bottom;

    [[nodiscard]] static constexpr Padding All(Length length) { return {length, length, length, length}; }

    [[nodiscard]] friend constexpr bool operator==(const Padding &, const Padding &) = default;
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
    /// Across, then down.
    std::array<Length, kAxisCount> offset{};
    std::array<Alignment, kAxisCount> anchor{Alignment::Start, Alignment::Start};
    std::array<Alignment, kAxisCount> attach{Alignment::Start, Alignment::Start};
    FloatAnchor target = FloatAnchor::Parent;
    bool enabled = false;
    /// Clipped by its parent's clip as an in-flow child is; otherwise unclipped.
    bool clipToParent = false;

    [[nodiscard]] friend constexpr bool operator==(const Floating &, const Floating &) = default;
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
    Length gap; ///< between consecutive in-flow children
    Length textSize = Px(kDefaultTextSize);
    Length borderWidth; ///< at least one device pixel once resolved, unless zero
    Length cornerRadius;
    CornerStyle cornerStyle = CornerStyle::Square;
    Direction direction = Direction::Row;
    std::array<Alignment, kAxisCount> childAlign{Alignment::Start, Alignment::Start};
    TextAlign textAlign = TextAlign::Left;
    /// How long a scrolling node takes to reach where it was sent, in seconds.
    /// Zero arrives at once; a small fraction glides instead of jumping.
    float scrollSmoothing = 0.f;
    /// The shortest its scroll bar's thumb draws, so a long list keeps
    /// something to see and to grab.
    Length scrollBarMinLength = Px(kDefaultScrollBarMinLength);
    /// Scrolling on an axis lets children overflow on it rather than shrink,
    /// clips them to the node, and gives it a bar on that axis.
    std::array<bool, kAxisCount> enabledScrollBars{false, false};
    ScrollBarVisibility scrollBarVisibility = ScrollBarVisibility::Never;
    ScrollBarDrag scrollBarDrag = ScrollBarDrag::FollowsPointer;

    /// Member by member, so a field added here is compared without anyone
    /// remembering to.
    [[nodiscard]] friend bool operator==(const Style &, const Style &) = default;
};

} // namespace Assisi::Mondrian
