/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Style.hpp>

#include <cstddef>

namespace Assisi::Mondrian
{
namespace
{

constexpr std::size_t kX = static_cast<std::size_t>(Axis::X);
constexpr std::size_t kY = static_cast<std::size_t>(Axis::Y);

/// Hands @p visit the field @p property names in @p a and in @p b, and returns
/// what it answers.
///
/// The one place a property is tied to its field, so copying one and comparing
/// one cannot disagree about which field that is. A switch with no default: a
/// property added without a case here is a warning, and warnings are errors.
template <typename From, typename To, typename Visit>
bool OnField(StyleProperty property, From &a, To &b, const Visit &visit)
{
    switch (property)
    {
    case StyleProperty::Background:
        return visit(a.background, b.background);
    case StyleProperty::BorderColor:
        return visit(a.borderColor, b.borderColor);
    case StyleProperty::TextColor:
        return visit(a.textColor, b.textColor);
    case StyleProperty::Width:
        return visit(a.sizing[kX], b.sizing[kX]);
    case StyleProperty::Height:
        return visit(a.sizing[kY], b.sizing[kY]);
    case StyleProperty::Padding:
        return visit(a.padding, b.padding);
    case StyleProperty::Gap:
        return visit(a.gap, b.gap);
    case StyleProperty::TextSize:
        return visit(a.textSize, b.textSize);
    case StyleProperty::BorderWidth:
        return visit(a.borderWidth, b.borderWidth);
    case StyleProperty::CornerRadius:
        return visit(a.cornerRadius, b.cornerRadius);
    case StyleProperty::CornerStyle:
        return visit(a.cornerStyle, b.cornerStyle);
    case StyleProperty::Direction:
        return visit(a.direction, b.direction);
    case StyleProperty::Align:
        return visit(a.childAlign, b.childAlign);
    case StyleProperty::TextAlign:
        return visit(a.textAlign, b.textAlign);
    case StyleProperty::ScrollSmoothing:
        return visit(a.scrollSmoothing, b.scrollSmoothing);
    case StyleProperty::ScrollBarMinLength:
        return visit(a.scrollBarMinLength, b.scrollBarMinLength);
    case StyleProperty::ScrollBars:
        return visit(a.enabledScrollBars, b.enabledScrollBars);
    case StyleProperty::ScrollBarVisibility:
        return visit(a.scrollBarVisibility, b.scrollBarVisibility);
    case StyleProperty::ScrollBarDrag:
        return visit(a.scrollBarDrag, b.scrollBarDrag);
    case StyleProperty::Float:
        return visit(a.floating.enabled, b.floating.enabled);
    case StyleProperty::FloatOffset:
        return visit(a.floating.offset, b.floating.offset);
    case StyleProperty::FloatAnchor:
        return visit(a.floating.anchor, b.floating.anchor);
    case StyleProperty::FloatAttach:
        return visit(a.floating.attach, b.floating.attach);
    case StyleProperty::FloatTarget:
        return visit(a.floating.target, b.floating.target);
    case StyleProperty::FloatClip:
        return visit(a.floating.clipToParent, b.floating.clipToParent);
    case StyleProperty::Count_:
        break;
    }
    return false;
}

struct CopyField
{
    template <typename T> bool operator()(const T &from, T &to) const
    {
        to = from;
        return true;
    }
};

struct SameField
{
    template <typename T> bool operator()(const T &a, const T &b) const { return a == b; }
};

uint64_t Bit(StyleProperty property)
{
    return uint64_t{1} << static_cast<uint32_t>(property);
}

constexpr std::size_t kPropertyCount = static_cast<std::size_t>(StyleProperty::Count_);

} // namespace

bool StylePatch::Writes(StyleProperty property) const
{
    return property != StyleProperty::Count_ && (written & Bit(property)) != 0;
}

void StylePatch::Take(StyleProperty property, const Style &from)
{
    if (OnField(property, from, values, CopyField{}))
    {
        written |= Bit(property);
    }
}

void StylePatch::ApplyTo(Style &style) const
{
    for (std::size_t index = 0; index < kPropertyCount; ++index)
    {
        const StyleProperty property = static_cast<StyleProperty>(index);
        if (Writes(property))
        {
            (void)OnField(property, values, style, CopyField{});
        }
    }
}

bool StylePatch::WritesOnlyKnown() const
{
    // A shift by the word's whole width is undefined, so a build with exactly
    // that many properties knows every bit instead.
    const uint64_t known = kPropertyCount == kStylePropertyBits ? ~uint64_t{0} : Bit(StyleProperty::Count_) - 1;
    return (written & ~known) == 0;
}

StylePatch StylePatch::Changes(StyleState state, const Style &resting, const Style &look)
{
    StylePatch patch;
    patch.state = state;
    for (std::size_t index = 0; index < kPropertyCount; ++index)
    {
        const StyleProperty property = static_cast<StyleProperty>(index);
        if (!OnField(property, resting, look, SameField{}))
        {
            patch.Take(property, look);
        }
    }
    return patch;
}

Style Restyled(const Style &resting, std::span<const StylePatch> looks, const StateSet &holds)
{
    Style style = resting;
    for (std::size_t state = 0; state < holds.size(); ++state)
    {
        if (!holds[state])
        {
            continue;
        }
        for (const StylePatch &look : looks)
        {
            if (look.state == static_cast<StyleState>(state))
            {
                look.ApplyTo(style);
            }
        }
    }
    return style;
}

} // namespace Assisi::Mondrian
