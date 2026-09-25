/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "ScreenAttributes.hpp"

#include "MarkupValues.hpp"

#include <Assisi/Mondrian/Pattern.hpp>

#include <algorithm>
#include <array>
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

constexpr std::array<NamedEnum<ScreenInput>, 3> kInputs{{{"none", ScreenInput::NoConsume},
                                                         {"consume", ScreenInput::ConsumeInput},
                                                         {"locked", ScreenInput::LockedConsumeInput}}};
constexpr std::array<NamedEnum<ScreenBeneath>, 2> kBeneaths{
    {{"show", ScreenBeneath::NoHide}, {"hide", ScreenBeneath::HidesBeneath}}};

/// Reads @p value into @p field through @p parse, saying whether it could.
/// Most attributes are one of these.
template <typename T, typename Parse> bool Read(T &field, std::string_view value, Parse parse)
{
    const std::optional<T> parsed = parse(value);
    if (!parsed)
    {
        return false;
    }
    field = *parsed;
    return true;
}

bool ReadPause(const AttributeTarget &target, std::string_view value)
{
    const std::optional<bool> pauses = ParseBool(value);
    if (!pauses)
    {
        return false;
    }
    target.document.traits.pause = *pauses ? ScreenPause::Pause : ScreenPause::Run;
    return true;
}

bool ReadNeeds(const AttributeTarget &target, std::string_view value)
{
    for (const std::string_view system : SplitWords(value))
    {
        target.document.systems.emplace_back(system);
    }
    return true;
}

/// How many and how tall at once, since one word can say both.
bool ReadLines(const AttributeTarget &target, std::string_view value)
{
    const std::optional<LinesValue> lines = ParseLines(value);
    if (!lines)
    {
        return false;
    }
    target.node.lines = lines->kind;
    target.node.height = lines->height;
    target.node.lineLimit = lines->lines;
    return true;
}

bool ReadFloatOffset(const AttributeTarget &target, std::string_view value)
{
    const std::vector<std::string_view> words = SplitWords(value);
    if (words.size() != kAxisCount)
    {
        return false;
    }
    const std::optional<Length> x = ParseLength(words[0]);
    const std::optional<Length> y = ParseLength(words[1]);
    if (!x || !y)
    {
        return false;
    }
    target.node.style.floating.offset = {*x, *y};
    return true;
}

/// An attribute only the root has: what the screen is.
constexpr AttributeSpec ForScreen(std::string_view name, ApplyAttribute apply)
{
    return AttributeSpec{.name = name, .apply = apply, .owner = AttributeOwner::Screen};
}

/// An attribute only @p widget has: an argument its own call takes.
constexpr AttributeSpec ForControl(BuiltinWidget widget, std::string_view name, ApplyAttribute apply)
{
    return AttributeSpec{.name = name, .apply = apply, .owner = AttributeOwner::Control, .widget = widget};
}

/// An attribute every node has.
constexpr AttributeSpec ForAny(std::string_view name, ApplyAttribute apply)
{
    return AttributeSpec{.name = name, .apply = apply};
}

/// An attribute ApplyAttributes reads itself, on @p widget alone, or on every
/// node when @p widget is None.
constexpr AttributeSpec Handled(std::string_view name, HandledAttribute handled,
                                BuiltinWidget widget = BuiltinWidget::None)
{
    return AttributeSpec{.name = name,
                         .owner = widget == BuiltinWidget::None ? AttributeOwner::AnyNode : AttributeOwner::Control,
                         .widget = widget,
                         .handled = handled};
}

/// Every attribute the markup has, in the order FindAttribute looks through
/// them. The one list: what a file may write, what a parameter may not be
/// named, and how each is read all come from here.
constexpr std::array kAttributes = std::to_array<AttributeSpec>({
    ForScreen("input",
              [](const AttributeTarget &t, std::string_view v) {
                  return Read(t.document.traits.input, v,
                              [](std::string_view text) { return LookUpEnum(text, kInputs); });
              }),
    ForScreen("beneath",
              [](const AttributeTarget &t, std::string_view v) {
                  return Read(t.document.traits.beneath, v,
                              [](std::string_view text) { return LookUpEnum(text, kBeneaths); });
              }),
    ForScreen("pause", ReadPause),
    ForScreen("sort",
              [](const AttributeTarget &t, std::string_view v) { return Read(t.document.sortKey, v, ParseSortKey); }),
    ForScreen("needs", ReadNeeds),

    ForControl(BuiltinWidget::Toggle, "on",
               [](const AttributeTarget &t, std::string_view v) { return Read(t.node.on, v, ParseBool); }),
    // A continuous slider: what its ends mean, how far one press moves it, and
    // where it starts.
    ForControl(BuiltinWidget::ContinuousSlider, "min",
               [](const AttributeTarget &t, std::string_view v) { return Read(t.node.range.min, v, ParseFloat); }),
    ForControl(BuiltinWidget::ContinuousSlider, "max",
               [](const AttributeTarget &t, std::string_view v) { return Read(t.node.range.max, v, ParseFloat); }),
    ForControl(BuiltinWidget::ContinuousSlider, "step",
               [](const AttributeTarget &t, std::string_view v) { return Read(t.node.range.step, v, ParseFloat); }),
    ForControl(BuiltinWidget::ContinuousSlider, "value",
               [](const AttributeTarget &t, std::string_view v) { return Read(t.node.value, v, ParseFloat); }),
    // A stepped one has no step: it moves one position per press whatever its
    // ends are, so `value` is which position it starts on.
    ForControl(BuiltinWidget::SteppedSlider, "min",
               [](const AttributeTarget &t, std::string_view v) { return Read(t.node.range.min, v, ParseFloat); }),
    ForControl(BuiltinWidget::SteppedSlider, "max",
               [](const AttributeTarget &t, std::string_view v) { return Read(t.node.range.max, v, ParseFloat); }),
    ForControl(BuiltinWidget::SteppedSlider, "steps",
               [](const AttributeTarget &t, std::string_view v) { return Read(t.node.steps, v, ParseInt); }),
    ForControl(BuiltinWidget::SteppedSlider, "value",
               [](const AttributeTarget &t, std::string_view v) { return Read(t.node.step, v, ParseInt); }),
    // Which axes scroll is a style field, so this writes the same place
    // `scroll_bars` does — and ApplyAttributes refuses that spelling on a
    // scroll, so the two can never disagree on one node.
    ForControl(BuiltinWidget::Scroll, "axes",
               [](const AttributeTarget &t, std::string_view v) {
                   return Read(t.node.style.enabledScrollBars, v, ParseAxes);
               }),
    ForControl(BuiltinWidget::TextField, "lines", ReadLines),
    ForControl(BuiltinWidget::TextField, "placeholder",
               [](const AttributeTarget &t, std::string_view v) {
                   t.node.placeholder = v;
                   return true;
               }),
    ForControl(BuiltinWidget::TextField, "mask",
               [](const AttributeTarget &t, std::string_view v) { return Read(t.node.mask, v, ParseTextMask); }),
    ForControl(BuiltinWidget::TextField, "max_length",
               [](const AttributeTarget &t, std::string_view v) { return Read(t.node.maxLength, v, ParseUInt); }),
    ForControl(BuiltinWidget::TextField, "check",
               [](const AttributeTarget &t, std::string_view v) { return Read(t.node.check, v, ParseTextCheck); }),
    Handled("pattern", HandledAttribute::Pattern, BuiltinWidget::TextField),

    Handled("on_click", HandledAttribute::Action),
    Handled("name", HandledAttribute::Name),
    Handled("focus", HandledAttribute::Focus),
    Handled("background", HandledAttribute::Background),
    Handled("border_color", HandledAttribute::BorderColor),
    Handled("text_color", HandledAttribute::TextColor),

    ForAny("style",
           [](const AttributeTarget &t, std::string_view v) {
               // Carried and not resolved: there is nowhere for a name to
               // resolve to yet, and a file written today should not need an
               // edit when there is.
               t.node.styleName = v;
               return true;
           }),
    ForAny("visible", [](const AttributeTarget &t, std::string_view v) { return Read(t.node.visible, v, ParseBool); }),
    ForAny("enabled", [](const AttributeTarget &t, std::string_view v) { return Read(t.node.enabled, v, ParseBool); }),
    ForAny("blocks_pointer",
           [](const AttributeTarget &t, std::string_view v) { return Read(t.node.blocksPointer, v, ParseBool); }),
    ForAny("takes_keyboard",
           [](const AttributeTarget &t, std::string_view v) { return Read(t.node.takesKeyboard, v, ParseBool); }),
    ForAny("selectable",
           [](const AttributeTarget &t, std::string_view v) { return Read(t.node.selectable, v, ParseBool); }),

    // Style: how a node looks, and how it places what is inside it.
    ForAny("width",
           [](const AttributeTarget &t, std::string_view v) {
               return Read(t.node.style.sizing[static_cast<std::size_t>(Axis::X)], v, ParseSizing);
           }),
    ForAny("height",
           [](const AttributeTarget &t, std::string_view v) {
               return Read(t.node.style.sizing[static_cast<std::size_t>(Axis::Y)], v, ParseSizing);
           }),
    ForAny("padding",
           [](const AttributeTarget &t, std::string_view v) { return Read(t.node.style.padding, v, ParsePadding); }),
    ForAny("gap", [](const AttributeTarget &t, std::string_view v) { return Read(t.node.style.gap, v, ParseLength); }),
    ForAny("text_size",
           [](const AttributeTarget &t, std::string_view v) { return Read(t.node.style.textSize, v, ParseLength); }),
    ForAny("border_width",
           [](const AttributeTarget &t, std::string_view v) { return Read(t.node.style.borderWidth, v, ParseLength); }),
    ForAny("corner_radius",
           [](const AttributeTarget &t, std::string_view v) {
               return Read(t.node.style.cornerRadius, v, ParseLength);
           }),
    ForAny("corner_style",
           [](const AttributeTarget &t, std::string_view v) {
               return Read(t.node.style.cornerStyle, v, ParseCornerStyle);
           }),
    ForAny("direction",
           [](const AttributeTarget &t, std::string_view v) {
               return Read(t.node.style.direction, v, ParseDirection);
           }),
    ForAny("align",
           [](const AttributeTarget &t, std::string_view v) {
               return Read(t.node.style.childAlign, v, ParseAlignPair);
           }),
    ForAny("text_align",
           [](const AttributeTarget &t, std::string_view v) {
               return Read(t.node.style.textAlign, v, ParseTextAlign);
           }),
    ForAny("scroll_smoothing",
           [](const AttributeTarget &t, std::string_view v) {
               return Read(t.node.style.scrollSmoothing, v, ParseFloat);
           }),
    ForAny("scroll_bar_min_length",
           [](const AttributeTarget &t, std::string_view v) {
               return Read(t.node.style.scrollBarMinLength, v, ParseLength);
           }),
    ForAny("scroll_bars",
           [](const AttributeTarget &t, std::string_view v) {
               return Read(t.node.style.enabledScrollBars, v, ParseAxes);
           }),
    ForAny("scroll_bar_visibility",
           [](const AttributeTarget &t, std::string_view v) {
               return Read(t.node.style.scrollBarVisibility, v, ParseScrollBarVisibility);
           }),
    ForAny("scroll_bar_drag",
           [](const AttributeTarget &t, std::string_view v) {
               return Read(t.node.style.scrollBarDrag, v, ParseScrollBarDrag);
           }),
    ForAny("float",
           [](const AttributeTarget &t, std::string_view v) {
               return Read(t.node.style.floating.enabled, v, ParseBool);
           }),
    ForAny("float_offset", ReadFloatOffset),
    ForAny("float_anchor",
           [](const AttributeTarget &t, std::string_view v) {
               return Read(t.node.style.floating.anchor, v, ParseAlignPair);
           }),
    ForAny("float_attach",
           [](const AttributeTarget &t, std::string_view v) {
               return Read(t.node.style.floating.attach, v, ParseAlignPair);
           }),
    ForAny("float_target",
           [](const AttributeTarget &t, std::string_view v) {
               return Read(t.node.style.floating.target, v, ParseFloatAnchor);
           }),
    ForAny("float_clip",
           [](const AttributeTarget &t, std::string_view v) {
               return Read(t.node.style.floating.clipToParent, v, ParseBool);
           }),
});

/// Each entry is read one way: through its own function, or by ApplyAttributes.
constexpr bool EachReadOneWay()
{
    return std::ranges::all_of(kAttributes, [](const AttributeSpec &spec)
                               { return (spec.handled == HandledAttribute::None) == (spec.apply != nullptr); });
}
static_assert(EachReadOneWay(), "an attribute has a function to read it or is handled, never both or neither");

/// A control's attribute names the control, and no other does.
constexpr bool ControlsNamed()
{
    return std::ranges::all_of(
        kAttributes, [](const AttributeSpec &spec)
        { return (spec.owner == AttributeOwner::Control) == (spec.widget != BuiltinWidget::None); });
}
static_assert(ControlsNamed(), "a control's attribute names its control, and only a control's does");

/// Whether the entries run screen, then control, then every node, which is
/// the precedence FindAttribute gets by taking the first that applies.
constexpr bool InLookupOrder()
{
    return std::ranges::is_sorted(kAttributes, {}, &AttributeSpec::owner);
}
static_assert(InLookupOrder(), "the table runs screen, then control, then every node");

/// Whether any two entries could both be what one name means on one node.
constexpr bool NoTwoAlike()
{
    for (std::size_t first = 0; first < kAttributes.size(); ++first)
    {
        for (std::size_t second = first + 1; second < kAttributes.size(); ++second)
        {
            const AttributeSpec &one = kAttributes[first];
            const AttributeSpec &other = kAttributes[second];
            if (one.name == other.name && one.owner == other.owner && one.widget == other.widget)
            {
                return false;
            }
        }
    }
    return true;
}
static_assert(NoTwoAlike(), "an attribute is listed once for each place it applies");

/// Whether every attribute ApplyAttributes handles has exactly one entry, so no
/// case there is unreachable and none is reachable two ways.
constexpr bool EveryHandledOnce()
{
    for (uint32_t handled = 1; handled < static_cast<uint32_t>(HandledAttribute::Count); ++handled)
    {
        const std::ptrdiff_t entries =
            std::ranges::count(kAttributes, static_cast<HandledAttribute>(handled), &AttributeSpec::handled);
        if (entries != 1)
        {
            return false;
        }
    }
    return true;
}
static_assert(EveryHandledOnce(), "every handled attribute has exactly one entry");

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
    return MarkupError{.message = std::move(message), .line = attribute.line, .column = attribute.column};
}

MarkupError At(const MarkupElement &element, std::string message)
{
    return MarkupError{.message = std::move(message), .line = element.line, .column = element.column};
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

const AttributeSpec *FindAttribute(std::string_view name, BuiltinWidget widget, bool isRoot)
{
    for (const AttributeSpec &spec : kAttributes)
    {
        if (spec.name != name)
        {
            continue;
        }
        const bool applies = spec.owner == AttributeOwner::AnyNode ||
                             (spec.owner == AttributeOwner::Screen && isRoot) ||
                             (spec.owner == AttributeOwner::Control && spec.widget == widget);
        if (applies)
        {
            return &spec;
        }
    }
    return nullptr;
}

bool IsMarkupAttribute(std::string_view name)
{
    return std::ranges::find(kAttributes, name, &AttributeSpec::name) != kAttributes.end();
}

Color &ColorField(Style &style, HandledAttribute colour)
{
    switch (colour)
    {
    case HandledAttribute::BorderColor:
        return style.borderColor;
    case HandledAttribute::TextColor:
        return style.textColor;
    case HandledAttribute::Background:
    case HandledAttribute::None:
    case HandledAttribute::Action:
    case HandledAttribute::Name:
    case HandledAttribute::Focus:
    case HandledAttribute::Pattern:
    case HandledAttribute::Count:
        break;
    }
    return style.background;
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
