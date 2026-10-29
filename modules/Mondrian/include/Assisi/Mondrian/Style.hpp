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

#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/Math/Color.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace Assisi::Mondrian
{

/// A maximum that never binds.
inline constexpr float kUnbounded = std::numeric_limits<float>::max();

/// What a percentage is out of: a length of `Percent(50)` is half.
inline constexpr float kPercentOf = 100.f;

/// @brief What a length is measured in.
AENUM()
enum class LengthUnit : uint8_t
{
    Px,      ///< UI pixels
    Percent, ///< of the parent's content size on the same axis; see Length
    Vw,      ///< hundredths of the viewport's width
    Vh,      ///< hundredths of the viewport's height
    Em,      ///< multiples of the node's own text size
    Count_
};

/// @brief A length and what it is measured in.
///
/// `%` is of the parent's content size on the length's axis, and counts as
/// nothing while that parent is still being sized to fit its content — a share
/// of a size that depends on the share has no answer. Lengths with no axis of
/// their own (border width, corner radius, scroll bar length) take `%` of the
/// node's own shorter side, so a corner radius of 50% makes a pill. A text
/// size's own `%` and `em` are of its parent's text size, as in CSS.
ASTRUCT()
struct Length
{
    AFIELD() float value = 0.f;
    AFIELD() LengthUnit unit = LengthUnit::Px;

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

/// What the root's text size is relative to, in UI pixels. A style's own text
/// size starts at its parent's, so this is every node's until one says
/// otherwise.
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
    Count_
};

inline constexpr std::size_t kAxisCount = static_cast<std::size_t>(Axis::Count_);

/// @brief How a node's size on one axis is decided.
AENUM()
enum class SizingKind : uint8_t
{
    Fit,   ///< just large enough for its content
    Grow,  ///< its content, then a share of whatever its parent has left
    Fixed, ///< exactly its value, in whatever unit it is written
    Count_
};

/// @brief A node's size on one axis, clamped to [min, max] whatever its kind.
ASTRUCT()
struct Sizing
{
    AFIELD() Length value; ///< Fixed: the length. Otherwise unused.
    AFIELD() Length min;
    AFIELD() Length max = Px(kUnbounded);
    AFIELD() SizingKind kind = SizingKind::Fit;

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
AENUM()
enum class Direction : uint8_t
{
    Row,    ///< left to right
    Column, ///< top to bottom
    Count_
};

/// @brief Where on an axis something sits within the space it has.
AENUM()
enum class Alignment : uint8_t
{
    Start,
    Center,
    End,
    Count_
};

/// @brief Space inside a node's edges, around its content.
ASTRUCT()
struct Padding
{
    AFIELD() Length left;
    AFIELD() Length top;
    AFIELD() Length right;
    AFIELD() Length bottom;

    [[nodiscard]] static constexpr Padding All(Length length) { return {length, length, length, length}; }

    [[nodiscard]] friend constexpr bool operator==(const Padding &, const Padding &) = default;
};

/// @brief When a scrolling node shows the bar that says where in its content it
/// is, and which a player may drag.
AENUM()
enum class ScrollBarVisibility : uint8_t
{
    Never,
    WhenNeeded, ///< only while some of the content is out of sight
    Always,
    Count_
};

/// @brief How the content keeps up with a scroll bar being dragged.
AENUM()
enum class ScrollBarDrag : uint8_t
{
    FollowsPointer, ///< the content is where the thumb is, so the hand moving it never leads
    Smoothed,       ///< the content glides after the thumb, as the rest of the scrolling does
    Count_
};

/// @brief What a floating node is placed against.
AENUM()
enum class FloatAnchor : uint8_t
{
    Parent,
    Root,
    Count_
};

/// @brief Placement outside the parent's flow: the point @p attach of the node
/// lands on the point @p anchor of what it floats against, then moves by
/// @p offset. A floating node takes no space in its parent and draws over its
/// siblings.
ASTRUCT()
struct Floating
{
    /// Across, then down.
    AFIELD() std::array<Length, kAxisCount> offset {};
    AFIELD() std::array<Alignment, kAxisCount> anchor { Alignment::Start, Alignment::Start };
    AFIELD() std::array<Alignment, kAxisCount> attach { Alignment::Start, Alignment::Start };
    AFIELD() FloatAnchor target = FloatAnchor::Parent;
    AFIELD() bool enabled = false;
    /// Clipped by its parent's clip as an in-flow child is; otherwise unclipped.
    AFIELD() bool clipToParent = false;

    [[nodiscard]] friend constexpr bool operator==(const Floating &, const Floating &) = default;
};

/// @brief Everything about a node that is not its content.
ASTRUCT()
struct Style
{
    AFIELD() Math::Color4<Math::ColorSpace::Srgb> background { 0.f, 0.f, 0.f, 0.f };
    AFIELD() Math::Color4<Math::ColorSpace::Srgb> borderColor { 0.f, 0.f, 0.f, 0.f };
    AFIELD() Math::Color4<Math::ColorSpace::Srgb> textColor { 1.f, 1.f, 1.f, 1.f };
    AFIELD() std::array<Sizing, kAxisCount> sizing {};
    AFIELD() Padding padding;
    AFIELD() Floating floating;
    AFIELD() Length gap; ///< between consecutive in-flow children
    /// Its parent's until set, as `1em` is.
    AFIELD() Length textSize = Em(1.f);
    AFIELD() Length borderWidth; ///< at least one device pixel once resolved, unless zero
    AFIELD() Length cornerRadius;
    AFIELD() CornerStyle cornerStyle = CornerStyle::Square;
    AFIELD() Direction direction = Direction::Row;
    AFIELD() std::array<Alignment, kAxisCount> childAlign { Alignment::Start, Alignment::Start };
    AFIELD() TextAlign textAlign = TextAlign::Left;
    /// How long a scrolling node takes to reach where it was sent, in seconds.
    /// Zero arrives at once; a small fraction glides instead of jumping.
    AFIELD() float scrollSmoothing = 0.f;
    /// The shortest its scroll bar's thumb draws, so a long list keeps
    /// something to see and to grab.
    AFIELD() Length scrollBarMinLength = Px(kDefaultScrollBarMinLength);
    /// Scrolling on an axis lets children overflow on it rather than shrink,
    /// clips them to the node, and gives it a bar on that axis.
    AFIELD() std::array<bool, kAxisCount> enabledScrollBars { false, false };
    AFIELD() ScrollBarVisibility scrollBarVisibility = ScrollBarVisibility::Never;
    AFIELD() ScrollBarDrag scrollBarDrag = ScrollBarDrag::FollowsPointer;

    /// Member by member, so a field added here is compared without anyone
    /// remembering to.
    [[nodiscard]] friend bool operator==(const Style &, const Style &) = default;
};

/// @brief A condition a node can be in that changes how it looks.
///
/// In the order they stack, weakest first: where two hold and both say
/// something about one property, the later one's wins. A disabled control
/// always looks disabled, and a press shows over a hover.
AENUM()
enum class StyleState : uint8_t
{
    Focused,  ///< has the keys, while they were used last or on a node that takes the keyboard
    Hover,    ///< under the pointer, or holding what is
    Pressed,  ///< the control held down, or holding it
    Disabled, ///< not enabled
    Count_
};

/// @brief Which states hold for one node, indexed by StyleState.
using StateSet = std::array<bool, static_cast<std::size_t>(StyleState::Count_)>;

/// @brief One thing a file can say about how a node looks or is laid out. Each
/// is one attribute, and reaches one field of Style.
AENUM()
enum class StyleProperty : uint8_t
{
    Background,
    BorderColor,
    TextColor,
    Width,
    Height,
    Padding,
    Gap,
    TextSize,
    BorderWidth,
    CornerRadius,
    CornerStyle,
    Direction,
    Align,
    TextAlign,
    ScrollSmoothing,
    ScrollBarMinLength,
    ScrollBars,
    ScrollBarVisibility,
    ScrollBarDrag,
    Float,
    FloatOffset,
    FloatAnchor,
    FloatAttach,
    FloatTarget,
    FloatClip,
    Count_
};

/// Bits in StylePatch::written, one per property.
inline constexpr uint32_t kStylePropertyBits = 64;
static_assert(static_cast<uint32_t>(StyleProperty::Count_) <= kStylePropertyBits,
              "a look records what it writes as the bits of one word");

/// @brief @p property's bit in a set of properties written.
[[nodiscard]] constexpr uint64_t PropertyBit(StyleProperty property)
{
    return uint64_t{1} << static_cast<uint32_t>(property);
}

/// @brief Every property this build has, as a set of properties written. A
/// shift by the word's whole width is undefined, so a build with exactly that
/// many properties sets every bit instead.
inline constexpr uint64_t kEveryProperty = static_cast<uint32_t>(StyleProperty::Count_) == kStylePropertyBits
                                               ? ~uint64_t{0}
                                               : PropertyBit(StyleProperty::Count_) - 1;

/// @brief Some properties of a style and their values: what a node looks like
/// in one state, put on over whatever it looks like otherwise, and also what a
/// node looks like at rest and what a theme's token holds.
ASTRUCT()
struct StylePatch
{
    /// Meaningful only for the properties written.
    AFIELD() Style values;
    /// One bit per StyleProperty.
    AFIELD() uint64_t written = 0;
    /// The state this is the look for. Unused where the patch is a resting look
    /// or a token's values.
    AFIELD() StyleState state = StyleState::Focused;

    [[nodiscard]] bool Writes(StyleProperty property) const;

    /// @brief @p property as @p from has it, written.
    void Take(StyleProperty property, const Style &from);

    /// @brief Every property written, put on @p style. The rest stay as they are.
    void ApplyTo(Style &style) const;

    /// @brief Every property @p over writes, as it writes it, over this one's.
    void Add(const StylePatch &over);

    /// @brief Whether every bit written names a property this build has.
    [[nodiscard]] bool WritesOnlyKnown() const;

    /// @brief The look for @p state that turns @p resting into @p look: every
    /// property in which the two differ. How a screen built in C++ says what a
    /// state changes, by writing the whole style it wants.
    [[nodiscard]] static StylePatch Changes(StyleState state, const Style &resting, const Style &look);

    [[nodiscard]] friend bool operator==(const StylePatch &, const StylePatch &) = default;
};

/// @brief @p resting with each of @p looks whose state @p holds put on over it,
/// weakest state first, whatever order @p looks keeps them in. What it writes is
/// what @p resting writes and what each of those looks does.
[[nodiscard]] StylePatch Restyled(const StylePatch &resting, std::span<const StylePatch> looks,
                                  const StateSet &holds);

} // namespace Assisi::Mondrian
