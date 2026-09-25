/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "ScreenAttributes.hpp"

#include "MarkupValues.hpp"

#include <Assisi/Mondrian/Pattern.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace Assisi::Mondrian::Import
{
namespace
{

/// How a file spells each verb. One entry per ScreenVerb, which the assert
/// below holds the table to: a verb with no spelling is one no file can call.
constexpr std::array<NamedEnum<ScreenVerb>, 2> kVerbs{{
    {"hide", ScreenVerb::Hide},
    {"step", ScreenVerb::Step},
}};
static_assert(kVerbs.size() == static_cast<std::size_t>(ScreenVerb::Count), "every verb needs a spelling");

/// What opens and closes a call, for the signatures a message spells out.
constexpr char kCallOpen = '(';
constexpr char kCallClose = ')';

constexpr std::array<ElementKind, 9> kElements{{
    {"row", BuiltinWidget::None, Direction::Row, false, true},
    {"column", BuiltinWidget::None, Direction::Column, false, true},
    {"text", BuiltinWidget::None, Direction::Row, true, false},
    {"button", BuiltinWidget::Button, Direction::Row, true, false},
    {"toggle", BuiltinWidget::Toggle, Direction::Row, false, false},
    {"slider", BuiltinWidget::ContinuousSlider, Direction::Row, false, false},
    {"stepped_slider", BuiltinWidget::SteppedSlider, Direction::Row, false, false},
    // A column, because what a scroll usually holds is a list.
    {"scroll", BuiltinWidget::Scroll, Direction::Column, false, true},
    // Its text is what it starts holding, which is the one control whose text
    // a player can then change.
    {"text_field", BuiltinWidget::TextField, Direction::Row, true, false},
}};

/// How a file marks a value as an expression rather than the name of one.
constexpr char kPatternDelimiter = '/';

/// The shortest a delimited pattern can be: both delimiters. One slash alone is
/// not an empty expression, it is half a pair.
constexpr std::size_t kDelimitedPatternLength = 2;

/// Reads @p value into @p field through @p parse, saying which of the three
/// things happened. Every style attribute is one of these.
template <typename T, typename Parse> Applied Read(T &field, std::string_view value, Parse parse)
{
    const std::optional<T> parsed = parse(value);
    if (!parsed)
    {
        return Applied::BadValue;
    }
    field = *parsed;
    return Applied::Yes;
}

/// What a continuous slider's ends mean, how far one press moves it, and where
/// it starts.
Applied ApplySliderAttribute(ScreenNode &node, std::string_view name, std::string_view value)
{
    if (name == "min")
    {
        return Read(node.range.min, value, ParseFloat);
    }
    if (name == "max")
    {
        return Read(node.range.max, value, ParseFloat);
    }
    if (name == "step")
    {
        return Read(node.range.step, value, ParseFloat);
    }
    if (name == "value")
    {
        return Read(node.value, value, ParseFloat);
    }
    return Applied::Unknown;
}

/// The same for a stepped one, which has no step: it moves one position per
/// press whatever its ends are, so `value` is which position it starts on.
Applied ApplySteppedAttribute(ScreenNode &node, std::string_view name, std::string_view value)
{
    if (name == "min")
    {
        return Read(node.range.min, value, ParseFloat);
    }
    if (name == "max")
    {
        return Read(node.range.max, value, ParseFloat);
    }
    if (name == "steps")
    {
        return Read(node.steps, value, ParseInt);
    }
    if (name == "value")
    {
        return Read(node.step, value, ParseInt);
    }
    return Applied::Unknown;
}

/// Everything a field carries beyond being one. `pattern` is missing on
/// purpose: it is the one attribute whose value can fail to compile, so it is
/// read where the line and column are still to hand.
Applied ApplyFieldAttribute(ScreenNode &node, std::string_view name, std::string_view value)
{
    if (name == "lines")
    {
        const std::optional<LinesValue> lines = ParseLines(value);
        if (!lines)
        {
            return Applied::BadValue;
        }
        node.lines = lines->kind;
        node.height = lines->height;
        node.lineLimit = lines->lines;
        return Applied::Yes;
    }
    if (name == "placeholder")
    {
        node.placeholder = value;
        return Applied::Yes;
    }
    if (name == "mask")
    {
        return Read(node.mask, value, ParseTextMask);
    }
    if (name == "max_length")
    {
        return Read(node.maxLength, value, ParseUInt);
    }
    if (name == "check")
    {
        return Read(node.check, value, ParseTextCheck);
    }
    return Applied::Unknown;
}

/// How a file spells @p verb.
std::string_view VerbName(ScreenVerb verb)
{
    for (const NamedEnum<ScreenVerb> &entry : kVerbs)
    {
        if (entry.value == verb)
        {
            return entry.name;
        }
    }
    return {};
}

} // namespace

MarkupError At(const MarkupAttribute &attribute, std::string message)
{
    return MarkupError{.message = std::move(message), .file = {}, .line = attribute.line, .column = attribute.column};
}

MarkupError At(const MarkupElement &element, std::string message)
{
    return MarkupError{.message = std::move(message), .file = {}, .line = element.line, .column = element.column};
}

const ElementKind *FindElement(std::string_view name)
{
    for (const ElementKind &kind : kElements)
    {
        if (kind.name == name)
        {
            return &kind;
        }
    }
    return nullptr;
}

std::string KnownElements()
{
    std::string names;
    for (const ElementKind &kind : kElements)
    {
        names += names.empty() ? "" : ", ";
        names += kind.name;
    }
    return names;
}

Applied ApplyStyleAttribute(Style &style, std::string_view name, std::string_view value)
{
    if (name == "width")
    {
        return Read(style.sizing[static_cast<std::size_t>(Axis::X)], value, ParseSizing);
    }
    if (name == "height")
    {
        return Read(style.sizing[static_cast<std::size_t>(Axis::Y)], value, ParseSizing);
    }
    if (name == "padding")
    {
        return Read(style.padding, value, ParsePadding);
    }
    if (name == "gap")
    {
        return Read(style.gap, value, ParseLength);
    }
    if (name == "text_size")
    {
        return Read(style.textSize, value, ParseLength);
    }
    if (name == "border_width")
    {
        return Read(style.borderWidth, value, ParseLength);
    }
    if (name == "corner_radius")
    {
        return Read(style.cornerRadius, value, ParseLength);
    }
    if (name == "corner_style")
    {
        return Read(style.cornerStyle, value, ParseCornerStyle);
    }
    if (name == "direction")
    {
        return Read(style.direction, value, ParseDirection);
    }
    if (name == "align")
    {
        return Read(style.childAlign, value, ParseAlignPair);
    }
    if (name == "text_align")
    {
        return Read(style.textAlign, value, ParseTextAlign);
    }
    if (name == "scroll_smoothing")
    {
        return Read(style.scrollSmoothing, value, ParseFloat);
    }
    if (name == "scroll_bar_min_length")
    {
        return Read(style.scrollBarMinLength, value, ParseLength);
    }
    if (name == "scroll_bars")
    {
        return Read(style.enabledScrollBars, value, ParseAxes);
    }
    if (name == "scroll_bar_visibility")
    {
        return Read(style.scrollBarVisibility, value, ParseScrollBarVisibility);
    }
    if (name == "scroll_bar_drag")
    {
        return Read(style.scrollBarDrag, value, ParseScrollBarDrag);
    }
    if (name == "float")
    {
        return Read(style.floating.enabled, value, ParseBool);
    }
    if (name == "float_offset")
    {
        const std::vector<std::string_view> words = SplitWords(value);
        if (words.size() != kAxisCount)
        {
            return Applied::BadValue;
        }
        const std::optional<Length> x = ParseLength(words[0]);
        const std::optional<Length> y = ParseLength(words[1]);
        if (!x || !y)
        {
            return Applied::BadValue;
        }
        style.floating.offset = {*x, *y};
        return Applied::Yes;
    }
    if (name == "float_anchor")
    {
        return Read(style.floating.anchor, value, ParseAlignPair);
    }
    if (name == "float_attach")
    {
        return Read(style.floating.attach, value, ParseAlignPair);
    }
    if (name == "float_target")
    {
        return Read(style.floating.target, value, ParseFloatAnchor);
    }
    if (name == "float_clip")
    {
        return Read(style.floating.clipToParent, value, ParseBool);
    }
    return Applied::Unknown;
}

Applied ApplyWidgetAttribute(ScreenNode &node, std::string_view name, std::string_view value)
{
    switch (node.widget)
    {
    case BuiltinWidget::Toggle:
        return name == "on" ? Read(node.on, value, ParseBool) : Applied::Unknown;
    case BuiltinWidget::ContinuousSlider:
        return ApplySliderAttribute(node, name, value);
    case BuiltinWidget::SteppedSlider:
        return ApplySteppedAttribute(node, name, value);
    case BuiltinWidget::Scroll:
        // Which axes scroll is a style field, so this writes the same place
        // `scroll_bars` does — and the element refuses that spelling, so the
        // two can never disagree on one node.
        return name == "axes" ? Read(node.style.enabledScrollBars, value, ParseAxes) : Applied::Unknown;
    case BuiltinWidget::TextField:
        return ApplyFieldAttribute(node, name, value);
    case BuiltinWidget::None:
    case BuiltinWidget::Button:
    case BuiltinWidget::Count:
        break;
    }
    return Applied::Unknown;
}

Applied ApplyNodeAttribute(ScreenNode &node, std::string_view name, std::string_view value)
{
    if (name == "style")
    {
        // Carried and not resolved: there is nowhere for a name to resolve to
        // yet, and a file written today should not need an edit when there is.
        node.styleName = value;
        return Applied::Yes;
    }
    if (name == "visible")
    {
        return Read(node.visible, value, ParseBool);
    }
    if (name == "enabled")
    {
        return Read(node.enabled, value, ParseBool);
    }
    if (name == "blocks_pointer")
    {
        return Read(node.blocksPointer, value, ParseBool);
    }
    if (name == "takes_keyboard")
    {
        return Read(node.takesKeyboard, value, ParseBool);
    }
    if (name == "selectable")
    {
        return Read(node.selectable, value, ParseBool);
    }
    return Applied::Unknown;
}

Applied ApplyScreenAttribute(ScreenDocument &document, std::string_view name, std::string_view value)
{
    static constexpr std::array<NamedEnum<ScreenInput>, 3> kInputs{{{"none", ScreenInput::NoConsume},
                                                                    {"consume", ScreenInput::ConsumeInput},
                                                                    {"locked", ScreenInput::LockedConsumeInput}}};
    static constexpr std::array<NamedEnum<ScreenBeneath>, 2> kBeneaths{
        {{"show", ScreenBeneath::NoHide}, {"hide", ScreenBeneath::HidesBeneath}}};

    if (name == "input")
    {
        return Read(document.traits.input, value, [](std::string_view text) { return LookUpEnum(text, kInputs); });
    }
    if (name == "beneath")
    {
        return Read(document.traits.beneath, value, [](std::string_view text) { return LookUpEnum(text, kBeneaths); });
    }
    if (name == "pause")
    {
        const std::optional<bool> pauses = ParseBool(value);
        if (!pauses)
        {
            return Applied::BadValue;
        }
        document.traits.pause = *pauses ? ScreenPause::Pause : ScreenPause::Run;
        return Applied::Yes;
    }
    if (name == "sort")
    {
        return Read(document.sortKey, value, ParseSortKey);
    }
    if (name == "needs")
    {
        for (std::string_view system : SplitWords(value))
        {
            document.systems.emplace_back(system);
        }
        return Applied::Yes;
    }
    return Applied::Unknown;
}

Color *ColorField(Style &style, std::string_view name)
{
    if (name == "background")
    {
        return &style.background;
    }
    if (name == "border_color")
    {
        return &style.borderColor;
    }
    if (name == "text_color")
    {
        return &style.textColor;
    }
    return nullptr;
}

bool IsMarkupAttribute(std::string_view name)
{
    Style style;
    if (std::ranges::find(kDirectAttributes, name) != kDirectAttributes.end() || ColorField(style, name) != nullptr)
    {
        return true;
    }
    ScreenDocument screen;
    if (ApplyScreenAttribute(screen, name, {}) != Applied::Unknown ||
        ApplyStyleAttribute(style, name, {}) != Applied::Unknown)
    {
        return true;
    }
    for (uint32_t widget = 0; widget < static_cast<uint32_t>(BuiltinWidget::Count); ++widget)
    {
        ScreenNode node;
        node.widget = static_cast<BuiltinWidget>(widget);
        if (ApplyWidgetAttribute(node, name, {}) != Applied::Unknown ||
            ApplyNodeAttribute(node, name, {}) != Applied::Unknown)
        {
            return true;
        }
    }
    return false;
}

std::expected<void, MarkupError> ApplyPattern(ScreenNode &node, const MarkupAttribute &attribute)
{
    const std::string_view value = attribute.value;
    if (value.empty())
    {
        node.pattern.clear();
        return {};
    }

    std::string_view expression;
    if (value.size() >= kDelimitedPatternLength && value.starts_with(kPatternDelimiter) &&
        value.ends_with(kPatternDelimiter))
    {
        expression = value.substr(1, value.size() - kDelimitedPatternLength);
    }
    else if (const std::optional<std::string_view> named = LookUpPattern(value))
    {
        expression = *named;
    }
    else
    {
        return std::unexpected(At(attribute, "'" + attribute.value + "' names no pattern this build has, and is not " +
                                                 "written between " + kPatternDelimiter +
                                                 " as an expression of its own. The names are: " + KnownPatterns() +
                                                 "."));
    }

    const std::expected<std::shared_ptr<const Pattern>, PatternError> compiled = CompilePattern(expression);
    if (!compiled)
    {
        return std::unexpected(At(attribute, "the pattern " + compiled.error().message));
    }
    node.pattern = expression;
    return {};
}

void Seed(ScreenNode &node)
{
    switch (node.widget)
    {
    case BuiltinWidget::TextField:
        node.style = TextFieldStyle();
        // A focused field has the keyboard even where the game otherwise has
        // it: typing into a box must not also drive the player's character.
        node.takesKeyboard = true;
        return;
    case BuiltinWidget::Scroll:
        // Its own background is what a drag scrolls, and a press on it is the
        // UI's rather than the game's.
        node.blocksPointer = true;
        return;
    case BuiltinWidget::None:
    case BuiltinWidget::Button:
    case BuiltinWidget::Toggle:
    case BuiltinWidget::ContinuousSlider:
    case BuiltinWidget::SteppedSlider:
    case BuiltinWidget::Count:
        return;
    }
}

std::expected<void, MarkupError> Required(const MarkupElement &element, const ScreenNode &node, bool stepWritten)
{
    if (node.widget != BuiltinWidget::ContinuousSlider)
    {
        return {};
    }
    // Left out, a slider would take a step sized for a 0-to-1 range whatever
    // its own ends are: on 0 to 100 that is a thousand presses end to end.
    if (!stepWritten)
    {
        return std::unexpected(At(element, "a slider says how far one press moves it, with step."));
    }
    // A step of none moves nothing, so the slider swallows the key and stays
    // where it is.
    if (node.range.step <= 0.f)
    {
        return std::unexpected(At(element, "a slider's step is what moves it, so it cannot be none."));
    }
    return {};
}

std::optional<ScreenVerb> LookUpVerb(std::string_view name)
{
    return LookUpEnum(name, kVerbs);
}

std::string Signature(ScreenVerb verb)
{
    std::string signature{VerbName(verb)};
    signature += kCallOpen;
    signature += VerbTakesTarget(verb) ? "target" : "";
    signature += VerbTakesMoves(verb) ? ", moves" : "";
    signature += kCallClose;
    return signature;
}

std::string KnownVerbs()
{
    std::string verbs;
    for (const NamedEnum<ScreenVerb> &entry : kVerbs)
    {
        verbs += verbs.empty() ? "" : ", ";
        verbs += Signature(entry.value);
    }
    return verbs;
}

} // namespace Assisi::Mondrian::Import
