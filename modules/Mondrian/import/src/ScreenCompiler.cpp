/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Mondrian/Import/ScreenCompiler.hpp>

#include "MarkupValues.hpp"

#include <Assisi/Mondrian/ScreenBlob.hpp>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/EventCatalog.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Assisi::Mondrian::Import
{
namespace
{

/// The element every file's root is.
constexpr std::string_view kScreenElement = "screen";

/// The element declaring a template, which only the root holds.
constexpr std::string_view kTemplateElement = "template";

/// What joins an instance's name to the names inside it: `music.slider`.
constexpr char kNameSeparator = '.';

/// How a file spells each verb. One entry per ScreenVerb, which the assert
/// below holds the table to: a verb with no spelling is one no file can call.
constexpr std::array<NamedEnum<ScreenVerb>, 2> kVerbs{{
    {"hide", ScreenVerb::Hide},
    {"step", ScreenVerb::Step},
}};
static_assert(kVerbs.size() == static_cast<std::size_t>(ScreenVerb::Count), "every verb needs a spelling");

/// What separates a call's name from its arguments, the arguments from each
/// other, and ends the call.
constexpr char kCallOpen = '(';
constexpr char kCallSeparator = ',';
constexpr char kCallClose = ')';

/// The whitespace an author may put around a call's name and arguments.
constexpr std::string_view kCallSpace = " \t\r\n";

/// What an element makes: a control, and the direction it lays its children out
/// in. `column` and `row` are one node with one field different, which is why
/// there is no Column widget.
struct ElementKind
{
    std::string_view name;
    BuiltinWidget widget;
    Direction direction;
    /// Whether the element's text content is its own. A container holding
    /// stray words is a mistake worth naming rather than dropping.
    bool carriesText;
    /// Whether it may hold elements. A leaf that holds one is the same mistake
    /// the other way round.
    bool carriesChildren;
};

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

/// What applying one attribute did.
enum class Applied : uint8_t
{
    Unknown,  ///< not an attribute of this kind; the caller tries the next table
    Yes,      ///< read and applied
    BadValue, ///< the right name holding something it cannot read
    Count
};

MarkupError At(const MarkupAttribute &attribute, std::string message)
{
    return MarkupError{.message = std::move(message), .line = attribute.line, .column = attribute.column};
}

MarkupError At(const MarkupElement &element, std::string message)
{
    return MarkupError{.message = std::move(message), .line = element.line, .column = element.column};
}

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

/// The style half of the attribute table: everything about how a node looks and
/// how it places what is inside it.
Applied ApplyStyleAttribute(Style &style, std::string_view name, std::string_view value)
{
    if (name == "background")
    {
        return Read(style.background, value, ParseColor);
    }
    if (name == "border_color")
    {
        return Read(style.borderColor, value, ParseColor);
    }
    if (name == "text_color")
    {
        return Read(style.textColor, value, ParseColor);
    }
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
        return Read(style.gap, value, ParseFloat);
    }
    if (name == "text_size")
    {
        return Read(style.textSize, value, ParseFloat);
    }
    if (name == "border_width")
    {
        return Read(style.borderWidth, value, ParseFloat);
    }
    if (name == "corner_radius")
    {
        return Read(style.cornerRadius, value, ParseFloat);
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
        return Read(style.scrollBarMinLength, value, ParseFloat);
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
        const std::optional<float> x = ParseFloat(words[0]);
        const std::optional<float> y = ParseFloat(words[1]);
        if (!x || !y)
        {
            return Applied::BadValue;
        }
        style.floating.offset = {.x = *x, .y = *y};
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

/// The attributes that belong to one kind of control and to no other: the
/// arguments its own call takes. Tried before the tables every node shares, so
/// a control spells its own arguments its own way.
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

/// Everything about a node that is not its style and not its behaviour.
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

/// The attributes only the root carries: what the screen is, rather than what
/// any one node looks like.
Applied ApplyScreenAttribute(ScreenDocument &document, std::string_view name, std::string_view value)
{
    static constexpr std::array<NamedEnum<ScreenInput>, 3> kInputs{{{"none", ScreenInput::NoConsume},
                                                                    {"consume", ScreenInput::ConsumeInput},
                                                                    {"locked", ScreenInput::LockedConsumeInput}}};
    static constexpr std::array<NamedEnum<ScreenBeneath>, 2> kBeneaths{
        {{"show", ScreenBeneath::NoHide}, {"hide", ScreenBeneath::HidesBeneath}}};

    if (name == "name")
    {
        document.name = value;
        return Applied::Yes;
    }
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

/// The expression `pattern` names or spells, compiled once here to find out
/// whether it would.
///
/// What the file carries is the expression and never the name, so nothing after
/// the cook has a table to look one up in. A value that is neither a name nor
/// delimited is refused rather than taken for an expression: a misspelt name
/// would otherwise compile into a rule matching its own letters, which accepts
/// nothing a player could type and looks like the field is simply broken.
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

/// What a control's own call would have set before a file says anything.
///
/// A document is fully resolved and the loader applies its style whole, so
/// whatever the node API gives a control has to be here — otherwise a field
/// written in a file would be an invisible box where one built in code is not.
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

/// What an element has to say for itself, as against what it may say.
/// @p stepWritten is whether `step` was written anywhere the node took its
/// attributes from: on an instance, that is its template's root as well as the
/// instance. Errors point at @p element.
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

/// Every element this build knows, for the message an unknown one prints.
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

/// A node a name was given to: where it sits in the table, and where the name
/// was written.
struct NamedNode
{
    uint32_t index = kNoNode;
    uint32_t line = 0;
    uint32_t column = 0;
};

/// A template a screen declares: the declaration, for where to point, and the
/// one element it holds, which every instance is built from. Both point into
/// the parsed file, which outlives the compile.
struct Template
{
    const MarkupElement *declaration = nullptr;
    const MarkupElement *root = nullptr;
};

/// One template instance being compiled: what names inside it are qualified
/// with, and where it was written, for an error found inside it to say so.
struct Scope
{
    /// What names inside this instance are qualified with: the instance's own
    /// qualified name, or its enclosing scope's prefix when it has no name.
    std::string prefix;
    std::string_view templateName;
    uint32_t line = 0;
    uint32_t column = 0;
    bool named = false;
    /// Whether this is the stand-in a template is checked through when the
    /// file may never use it, which was written nowhere.
    bool standIn = false;
};

/// A target a verb names, waiting for the walk to finish: a button may come
/// before the control it acts on, so a name can only be looked up once every
/// node has been read.
struct PendingTarget
{
    std::string name;
    uint32_t node = kNoNode; ///< the button whose verb names it
    uint32_t line = 0;
    uint32_t column = 0;
};

/// What compiling one element needs to know about the file around it.
struct Walk
{
    ScreenDocument &document;
    const Core::EventCatalog &catalog;
    /// Every node name written so far, so a second node carrying one is
    /// refused with the first's place in hand, and a target can be resolved.
    std::unordered_map<std::string, NamedNode> names;
    std::vector<PendingTarget> targets;
    /// The screen's templates, by name.
    std::unordered_map<std::string, Template> templates;
    /// The instances being compiled, outermost first.
    std::vector<Scope> scopes;
    /// Whether some node has already claimed focus, so a second can be refused
    /// rather than quietly winning.
    bool focusClaimed = false;
};

/// @p name as the screen knows it: qualified with the instance it was written
/// inside, if any.
std::string Qualify(const Walk &walk, std::string_view name)
{
    if (walk.scopes.empty() || walk.scopes.back().prefix.empty())
    {
        return std::string{name};
    }
    return walk.scopes.back().prefix + kNameSeparator + std::string{name};
}

/// Names node @p index, refusing a name another node on the screen has. A name
/// is what a target and a lookup mean a node by, so two nodes sharing one would
/// leave both meaning whichever came first.
std::expected<void, MarkupError> ApplyName(Walk &walk, const MarkupAttribute &attribute, uint32_t index)
{
    // The separator is how an instance qualifies what is inside it, so a name
    // written with one could be the same name an instance makes.
    if (attribute.value.find(kNameSeparator) != std::string::npos)
    {
        return std::unexpected(At(attribute, "'" + attribute.value + "' holds '" + kNameSeparator +
                                                 "', which is how a template instance qualifies the names inside "
                                                 "it. Name the instance instead."));
    }
    const std::string name = Qualify(walk, attribute.value);
    if (!name.empty())
    {
        const std::unordered_map<std::string, NamedNode>::const_iterator first = walk.names.find(name);
        if (first != walk.names.end())
        {
            // The same attribute reached twice is one template's name, made by
            // two instances of it that each left the name as it was written.
            const bool sameSource = first->second.line == attribute.line && first->second.column == attribute.column;
            if (sameSource && !walk.scopes.empty() && !walk.scopes.back().named)
            {
                const Scope &scope = walk.scopes.back();
                return std::unexpected(MarkupError{
                    .message = "template '" + std::string{scope.templateName} + "' names '" + attribute.value +
                               "' inside it, and an unnamed instance keeps the names it makes as they are written, "
                               "so a second one makes them twice. Name the instance.",
                    .line = scope.line,
                    .column = scope.column});
            }
            return std::unexpected(At(attribute, "'" + name + "' is already the name of a node on this screen (line " +
                                                     std::to_string(first->second.line) + "). A name means one node."));
        }
        walk.names.emplace(name, NamedNode{.index = index, .line = attribute.line, .column = attribute.column});
    }
    walk.document.nodes[index].name = name;
    return {};
}

/// @p text without the whitespace around it.
std::string_view Trimmed(std::string_view text)
{
    const std::size_t first = text.find_first_not_of(kCallSpace);
    if (first == std::string_view::npos)
    {
        return {};
    }
    const std::size_t last = text.find_last_not_of(kCallSpace);
    return text.substr(first, last - first + 1);
}

/// A call as a file writes it: the verb's name, and its arguments as written.
struct Call
{
    std::vector<std::string_view> arguments;
    std::string_view name;
};

/// @p value read as a call, or nullopt when it is a bare name. A call that
/// opens and does not close, or has anything after it, is refused rather than
/// read as the part that parsed.
std::expected<std::optional<Call>, MarkupError> ParseCall(const MarkupAttribute &attribute)
{
    const std::string_view value = attribute.value;
    const std::size_t open = value.find(kCallOpen);
    if (open == std::string_view::npos)
    {
        return std::optional<Call>{};
    }
    const std::size_t close = value.find(kCallClose, open);
    if (close == std::string_view::npos)
    {
        return std::unexpected(
            At(attribute, "'" + attribute.value + "' opens a call and never closes it with " + kCallClose + "."));
    }
    const std::string_view after = Trimmed(value.substr(close + 1));
    if (!after.empty())
    {
        return std::unexpected(At(attribute, "'" + std::string{after} + "' follows the call in '" + attribute.value +
                                                 "'. on_click holds one call."));
    }

    Call call;
    call.name = Trimmed(value.substr(0, open));
    const std::string_view inside = Trimmed(value.substr(open + 1, close - open - 1));
    // Nothing between the parens is no arguments, not one empty one.
    std::size_t start = 0;
    while (!inside.empty() && start <= inside.size())
    {
        std::size_t end = inside.find(kCallSeparator, start);
        end = end == std::string_view::npos ? inside.size() : end;
        call.arguments.push_back(Trimmed(inside.substr(start, end - start)));
        start = end + 1;
    }
    return std::optional<Call>{call};
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

/// How @p verb is written with everything it takes, for a message saying so.
std::string Signature(ScreenVerb verb)
{
    std::string signature{VerbName(verb)};
    signature += kCallOpen;
    signature += VerbTakesTarget(verb) ? "target" : "";
    signature += VerbTakesMoves(verb) ? ", moves" : "";
    signature += kCallClose;
    return signature;
}

/// Every verb as it is written, for the message an unknown one prints.
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

/// A bare name: an event this build declares. A verb's name written bare is
/// refused, so a verb has one spelling and it is a call.
std::expected<void, MarkupError> ApplyEvent(ScreenNode &node, const MarkupAttribute &attribute,
                                            const Core::EventCatalog &catalog)
{
    if (const std::optional<ScreenVerb> verb = LookUpEnum(attribute.value, kVerbs))
    {
        return std::unexpected(At(attribute, "'" + attribute.value + "' is a verb, and a verb is written as a call: " +
                                                 Signature(*verb) + "."));
    }
    if (catalog.Find(attribute.value) == nullptr)
    {
        return std::unexpected(At(attribute, "'" + attribute.value +
                                                 "' names no event this build declares. An event is a struct marked "
                                                 "AEVENT() in a reflected header, named here by its full C++ name; "
                                                 "a verb is a call, one of: " +
                                                 KnownVerbs() + "."));
    }
    node.action = ActionKind::Event;
    node.eventName = attribute.value;
    return {};
}

/// A call: a verb, checked against what it takes. A target is kept by name
/// until the walk is done, since the node it names may not have been read yet.
std::expected<void, MarkupError> ApplyVerb(Walk &walk, uint32_t index, const MarkupAttribute &attribute,
                                           const Call &call)
{
    const std::optional<ScreenVerb> verb = LookUpEnum(call.name, kVerbs);
    if (!verb)
    {
        return std::unexpected(At(attribute, "'" + std::string{call.name} +
                                                 "' is not a verb this markup has. It has: " + KnownVerbs() + "."));
    }

    const std::size_t wanted =
        static_cast<std::size_t>(VerbTakesTarget(*verb)) + static_cast<std::size_t>(VerbTakesMoves(*verb));
    if (call.arguments.size() != wanted)
    {
        return std::unexpected(At(attribute, "'" + attribute.value + "' gives " + std::string{call.name} + " " +
                                                 std::to_string(call.arguments.size()) + " arguments; it is written " +
                                                 Signature(*verb) + "."));
    }

    ScreenNode &node = walk.document.nodes[index];
    node.action = ActionKind::Verb;
    node.verb = *verb;

    std::size_t next = 0;
    if (VerbTakesTarget(*verb))
    {
        // Qualified here, where the scope it was written in is known: a target
        // inside a template means a part of the same instance.
        walk.targets.push_back(PendingTarget{.name = Qualify(walk, call.arguments[next]),
                                             .node = index,
                                             .line = attribute.line,
                                             .column = attribute.column});
        ++next;
    }
    if (VerbTakesMoves(*verb))
    {
        const std::string_view written = call.arguments[next];
        const std::optional<int32_t> moves = ParseInt(written);
        if (!moves)
        {
            return std::unexpected(At(attribute, "'" + std::string{written} + "' is not a whole number of moves."));
        }
        // A move of none swallows the press and changes nothing, which nobody
        // writes on purpose.
        if (*moves == 0)
        {
            return std::unexpected(At(attribute, "'" + attribute.value + "' makes 0 moves, which moves nothing."));
        }
        node.moves = *moves;
    }
    return {};
}

/// What a control does when it fires: a call is a verb, and a bare name is an
/// event.
std::expected<void, MarkupError> ApplyAction(Walk &walk, uint32_t index, const MarkupAttribute &attribute)
{
    const std::expected<std::optional<Call>, MarkupError> call = ParseCall(attribute);
    if (!call)
    {
        return std::unexpected(call.error());
    }
    if (!call->has_value())
    {
        return ApplyEvent(walk.document.nodes[index], attribute, walk.catalog);
    }
    return ApplyVerb(walk, index, attribute, **call);
}

/// Every target a verb named, looked up now that every node has been read.
std::expected<void, MarkupError> ResolveTargets(Walk &walk)
{
    for (const PendingTarget &pending : walk.targets)
    {
        const MarkupError where{.message = {}, .line = pending.line, .column = pending.column};
        const std::unordered_map<std::string, NamedNode>::const_iterator named = walk.names.find(pending.name);
        if (named == walk.names.end())
        {
            MarkupError error = where;
            error.message = "'" + pending.name + "' names no node on this screen.";
            return std::unexpected(std::move(error));
        }

        ScreenNode &node = walk.document.nodes[pending.node];
        if (!VerbActsOn(node.verb, walk.document.nodes[named->second.index].widget))
        {
            MarkupError error = where;
            error.message = "'" + pending.name + "' is not a slider, and " + Signature(node.verb) + " moves a slider.";
            return std::unexpected(std::move(error));
        }
        node.target = named->second.index;
    }
    return {};
}

std::expected<void, MarkupError> CompileElement(Walk &walk, const MarkupElement &element, uint32_t parent);

/// The attributes an element carries, in the order the tables are tried: the
/// node's own, then its style. The root adds the screen's before both.
std::expected<void, MarkupError> ApplyAttributes(Walk &walk, const MarkupElement &element, uint32_t index)
{
    const bool isRoot = index == 0;
    ScreenNode &node = walk.document.nodes[index];

    for (const MarkupAttribute &attribute : element.attributes)
    {
        if (attribute.name == "on_click")
        {
            if (isRoot)
            {
                return std::unexpected(At(attribute, "the screen itself cannot be clicked; put on_click on a "
                                                     "control inside it."));
            }
            if (node.widget != BuiltinWidget::Button)
            {
                return std::unexpected(At(attribute, "only a button is clicked. Every other control answers a "
                                                     "press itself, and what it holds is read rather than "
                                                     "announced."));
            }
            if (const std::expected<void, MarkupError> action = ApplyAction(walk, index, attribute); !action)
            {
                return std::unexpected(action.error());
            }
            continue;
        }

        if (attribute.name == "pattern" && node.widget == BuiltinWidget::TextField)
        {
            if (const std::expected<void, MarkupError> pattern = ApplyPattern(node, attribute); !pattern)
            {
                return std::unexpected(pattern.error());
            }
            continue;
        }

        // `scroll_bars` and `axes` write the same field, and a scroll spells it
        // `axes` after the argument its own call takes. Two spellings for one
        // thing is how a file comes to say two different things at once.
        if (attribute.name == "scroll_bars" && node.widget == BuiltinWidget::Scroll)
        {
            return std::unexpected(At(attribute, "a scroll says which axes it scrolls with 'axes', not "
                                                 "'scroll_bars'."));
        }

        // On the root, `name` is what the screen is called and not a node's
        // name, so the screen's table reads it below.
        if (attribute.name == "name" && !isRoot)
        {
            if (const std::expected<void, MarkupError> named = ApplyName(walk, attribute, index); !named)
            {
                return std::unexpected(named.error());
            }
            continue;
        }

        if (attribute.name == "focus")
        {
            const std::optional<bool> focused = ParseBool(attribute.value);
            if (!focused)
            {
                return std::unexpected(At(attribute, "focus is written true or false."));
            }
            if (*focused)
            {
                if (walk.focusClaimed)
                {
                    return std::unexpected(At(attribute, "a second node asks for focus; a screen starts with "
                                                         "the keys on one node."));
                }
                walk.focusClaimed = true;
                walk.document.focus = index;
            }
            continue;
        }

        Applied applied =
            isRoot ? ApplyScreenAttribute(walk.document, attribute.name, attribute.value) : Applied::Unknown;
        if (applied == Applied::Unknown)
        {
            applied = ApplyWidgetAttribute(node, attribute.name, attribute.value);
        }
        if (applied == Applied::Unknown)
        {
            applied = ApplyNodeAttribute(node, attribute.name, attribute.value);
        }
        if (applied == Applied::Unknown)
        {
            applied = ApplyStyleAttribute(node.style, attribute.name, attribute.value);
        }

        if (applied == Applied::BadValue)
        {
            return std::unexpected(
                At(attribute, "'" + attribute.value + "' is not a value '" + attribute.name + "' can hold."));
        }
        if (applied == Applied::Unknown)
        {
            return std::unexpected(At(attribute, "'" + attribute.name +
                                                     "' is not an attribute this markup "
                                                     "has."));
        }
    }
    return {};
}

std::expected<void, MarkupError> CompileChildren(Walk &walk, const MarkupElement &element, uint32_t index)
{
    for (const MarkupElement &child : element.children)
    {
        if (const std::expected<void, MarkupError> compiled = CompileElement(walk, child, index); !compiled)
        {
            return std::unexpected(compiled.error());
        }
    }
    return {};
}

/// Every template the screen declares, for the message an unknown element
/// prints, in no particular order.
std::string KnownTemplates(const Walk &walk)
{
    std::string names;
    for (const std::pair<const std::string, Template> &entry : walk.templates)
    {
        names += names.empty() ? "" : ", ";
        names += entry.first;
    }
    return names;
}

/// @p error, saying which instance it was found inside. A stand-in was written
/// nowhere, so an error inside one is left pointing at the template alone.
MarkupError Inside(MarkupError error, const Scope &scope)
{
    if (!scope.standIn)
    {
        error.message += " (inside template '" + std::string{scope.templateName} + "', used at line " +
                         std::to_string(scope.line) + " column " + std::to_string(scope.column) + ")";
    }
    return error;
}

/// The template root's attributes an instance leaves alone, as an element to
/// apply: every one the instance does not also write, since the instance's win.
MarkupElement TemplateBase(const MarkupElement &root, const MarkupElement &instance)
{
    MarkupElement base;
    base.name = root.name;
    base.line = root.line;
    base.column = root.column;
    for (const MarkupAttribute &attribute : root.attributes)
    {
        if (instance.Find(attribute.name) == nullptr)
        {
            base.attributes.push_back(attribute);
        }
    }
    return base;
}

/// Compiles an instance of @p used under @p parent: a node of the template's
/// root kind, the template's attributes and children compiled inside the
/// instance's scope, then the instance's own over them in the scope around it.
///
/// Scoped so that what the template writes means the template's parts — its
/// names and targets carry the instance's name — while what the instance
/// writes means what it does where the instance sits. @p standIn marks the
/// check a template gets when nothing may use it.
std::expected<void, MarkupError> CompileInstance(Walk &walk, const MarkupElement &instance, const Template &used,
                                                 uint32_t parent, bool standIn)
{
    if (walk.scopes.size() >= kMaxTemplateNesting)
    {
        return std::unexpected(At(instance, "templates nest more than " + std::to_string(kMaxTemplateNesting) +
                                                " deep here. Flatten one of them."));
    }

    const MarkupElement &root = *used.root;
    // A template's root is a built-in element; CollectTemplates holds it to that.
    const ElementKind &kind = *FindElement(root.name);
    if (!kind.carriesText && !instance.text.empty())
    {
        return std::unexpected(At(instance, "'" + instance.name + "' is a " + std::string{kind.name} +
                                                ", which carries no text. Put the words in a <text>."));
    }
    if (!kind.carriesChildren && !instance.children.empty())
    {
        return std::unexpected(
            At(instance, "'" + instance.name + "' is a " + std::string{kind.name} + ", which holds no elements."));
    }

    ScreenNode node;
    node.parent = parent;
    node.widget = kind.widget;
    // The instance's words replace the template's, and leave them when it has none.
    node.text = instance.text.empty() ? root.text : instance.text;
    Seed(node);
    node.style.direction = kind.direction;
    const uint32_t index = static_cast<uint32_t>(walk.document.nodes.size());
    walk.document.nodes.push_back(std::move(node));

    const MarkupAttribute *const instanceName = instance.Find("name");
    const bool named = instanceName != nullptr && !instanceName->value.empty();
    Scope scope{.prefix = named                 ? Qualify(walk, instanceName->value)
                          : walk.scopes.empty() ? std::string{}
                                                : walk.scopes.back().prefix,
                .templateName = instance.name,
                .line = instance.line,
                .column = instance.column,
                .named = named,
                .standIn = standIn};

    const MarkupElement base = TemplateBase(root, instance);
    walk.scopes.push_back(std::move(scope));
    std::expected<void, MarkupError> inside = ApplyAttributes(walk, base, index);
    if (inside)
    {
        inside = CompileChildren(walk, root, index);
    }
    const Scope left = std::move(walk.scopes.back());
    walk.scopes.pop_back();
    if (!inside)
    {
        return std::unexpected(Inside(std::move(inside.error()), left));
    }

    if (const std::expected<void, MarkupError> own = ApplyAttributes(walk, instance, index); !own)
    {
        return std::unexpected(own.error());
    }
    const bool stepWritten = base.Find("step") != nullptr || instance.Find("step") != nullptr;
    if (const std::expected<void, MarkupError> required = Required(instance, walk.document.nodes[index], stepWritten);
        !required)
    {
        return std::unexpected(required.error());
    }
    // After the template's, so an instance adds to what the template holds.
    return CompileChildren(walk, instance, index);
}

std::expected<void, MarkupError> CompileElement(Walk &walk, const MarkupElement &element, uint32_t parent)
{
    if (element.name == kTemplateElement)
    {
        return std::unexpected(At(element, "a template is declared directly inside the <" +
                                               std::string{kScreenElement} + ">, and nowhere else."));
    }
    const ElementKind *const kind = FindElement(element.name);
    if (kind == nullptr)
    {
        const std::unordered_map<std::string, Template>::const_iterator used = walk.templates.find(element.name);
        if (used != walk.templates.end())
        {
            return CompileInstance(walk, element, used->second, parent, false);
        }
        std::string message = "'" + element.name + "' is not an element this markup has. It has: " + KnownElements();
        message += walk.templates.empty() ? "." : "; and this screen's templates: " + KnownTemplates(walk) + ".";
        return std::unexpected(At(element, std::move(message)));
    }
    if (!kind->carriesText && !element.text.empty())
    {
        return std::unexpected(At(element, "'" + element.name +
                                               "' holds text, which it does not carry. Put the "
                                               "words in a <text>."));
    }
    if (!kind->carriesChildren && !element.children.empty())
    {
        return std::unexpected(At(element, "'" + element.name + "' holds elements, and holds none."));
    }

    ScreenNode node;
    node.parent = parent;
    node.widget = kind->widget;
    node.text = element.text;
    // Seeded first, because a control's own look is what a file's attributes
    // are written over.
    Seed(node);
    node.style.direction = kind->direction;

    const uint32_t index = static_cast<uint32_t>(walk.document.nodes.size());
    walk.document.nodes.push_back(std::move(node));

    if (const std::expected<void, MarkupError> attributes = ApplyAttributes(walk, element, index); !attributes)
    {
        return std::unexpected(attributes.error());
    }
    if (const std::expected<void, MarkupError> required =
            Required(element, walk.document.nodes[index], element.Find("step") != nullptr);
        !required)
    {
        return std::unexpected(required.error());
    }
    return CompileChildren(walk, element, index);
}

/// Reads the templates @p root declares into the walk, refusing any that is
/// not one name over one built-in element.
///
/// Read before anything is compiled, so a template may be used above where it
/// is declared.
std::expected<void, MarkupError> CollectTemplates(Walk &walk, const MarkupElement &root)
{
    for (const MarkupElement &declaration : root.children)
    {
        if (declaration.name != kTemplateElement)
        {
            continue;
        }
        const MarkupAttribute *const named = declaration.Find("name");
        if (named == nullptr || named->value.empty() || declaration.attributes.size() != 1)
        {
            return std::unexpected(At(declaration, "a template says its name and nothing else: <" +
                                                       std::string{kTemplateElement} + " name=\"...\">."));
        }
        const std::string &name = named->value;
        if (name.find(kNameSeparator) != std::string::npos)
        {
            return std::unexpected(At(*named, "'" + name + "' holds '" + kNameSeparator +
                                                  "', which is how an instance qualifies the names inside it."));
        }
        if (FindElement(name) != nullptr || name == kScreenElement || name == kTemplateElement)
        {
            return std::unexpected(At(*named, "'" + name +
                                                  "' is already an element this markup has, so a template "
                                                  "needs another name."));
        }
        if (const std::unordered_map<std::string, Template>::const_iterator first = walk.templates.find(name);
            first != walk.templates.end())
        {
            return std::unexpected(At(*named, "'" + name + "' is already a template on this screen (line " +
                                                  std::to_string(first->second.declaration->line) + ")."));
        }
        if (!declaration.text.empty() || declaration.children.size() != 1)
        {
            return std::unexpected(At(declaration, "template '" + name +
                                                       "' holds one element, which every instance is built "
                                                       "from, and nothing else."));
        }
        // One layer of overrides: a root that was itself an instance would put
        // this template's attributes over another's, under the instance's.
        const MarkupElement &body = declaration.children.front();
        if (FindElement(body.name) == nullptr)
        {
            return std::unexpected(At(body, "template '" + name + "' is built on <" + body.name +
                                                ">, and a template is built on an element this markup has. "
                                                "Use other templates inside it."));
        }
        walk.templates.emplace(name, Template{.declaration = &declaration, .root = &body});
    }
    return {};
}

/// Every instance anywhere beneath @p element, in the order the file writes them.
void CollectUses(const Walk &walk, const MarkupElement &element, std::vector<const MarkupElement *> &uses)
{
    for (const MarkupElement &child : element.children)
    {
        if (walk.templates.contains(child.name))
        {
            uses.push_back(&child);
        }
        CollectUses(walk, child, uses);
    }
}

/// Refuses a template that holds itself, through @p path — the templates being
/// followed, outermost first. Each template is followed once; @p done holds
/// those already cleared.
///
/// Stops following at the nesting bound: a chain that long is refused when it
/// is compiled, and a cycle inside it with it.
std::expected<void, MarkupError> RefuseCycles(const Walk &walk, std::vector<std::string_view> &path,
                                              std::unordered_set<std::string_view> &done)
{
    if (path.size() > kMaxTemplateNesting)
    {
        return {};
    }
    const std::string_view name = path.back();
    std::vector<const MarkupElement *> uses;
    CollectUses(walk, *walk.templates.at(std::string{name}).declaration, uses);

    for (const MarkupElement *const use : uses)
    {
        const std::vector<std::string_view>::const_iterator seen = std::ranges::find(path, use->name);
        if (seen != path.end())
        {
            std::string chain = "'" + std::string{*seen} + "'";
            for (std::vector<std::string_view>::const_iterator next = seen + 1; next != path.end(); ++next)
            {
                chain += ", which holds '" + std::string{*next} + "'";
            }
            chain += ", which holds '" + use->name + "'";
            return std::unexpected(At(*use, "a template may not hold itself, however far down: " + chain + "."));
        }
        if (done.contains(use->name))
        {
            continue;
        }
        path.push_back(use->name);
        if (const std::expected<void, MarkupError> inner = RefuseCycles(walk, path, done); !inner)
        {
            return inner;
        }
        path.pop_back();
    }
    done.insert(name);
    return {};
}

/// Compiles every template once on its own, as though an unnamed instance of
/// it stood where it is declared, so a mistake in one nothing uses still fails
/// the cook. What it builds is thrown away.
std::expected<void, MarkupError> CheckTemplates(const Walk &walk, const MarkupElement &root)
{
    for (const MarkupElement &declaration : root.children)
    {
        if (declaration.name != kTemplateElement)
        {
            continue;
        }
        const std::string &name = declaration.Find("name")->value;

        std::vector<std::string_view> path{name};
        std::unordered_set<std::string_view> done;
        if (const std::expected<void, MarkupError> cycles = RefuseCycles(walk, path, done); !cycles)
        {
            return cycles;
        }

        ScreenDocument scratch;
        scratch.nodes.emplace_back();
        Walk check{.document = scratch,
                   .catalog = walk.catalog,
                   .names = {},
                   .targets = {},
                   .templates = walk.templates,
                   .scopes = {}};
        MarkupElement standIn;
        standIn.name = name;
        standIn.line = declaration.line;
        standIn.column = declaration.column;
        if (const std::expected<void, MarkupError> compiled =
                CompileInstance(check, standIn, walk.templates.at(name), 0, true);
            !compiled)
        {
            return compiled;
        }
        if (const std::expected<void, MarkupError> targets = ResolveTargets(check); !targets)
        {
            return targets;
        }
    }
    return {};
}

} // namespace

std::expected<ScreenDocument, MarkupError> CompileScreen(const MarkupElement &root, const Core::EventCatalog &catalog)
{
    if (root.name != kScreenElement)
    {
        return std::unexpected(
            At(root, "a screen file starts with <" + std::string{kScreenElement} + ">, not <" + root.name + ">."));
    }
    if (!root.text.empty())
    {
        return std::unexpected(At(root, "the screen holds text, which only text and button carry."));
    }

    ScreenDocument document;
    // The root of the tree, which the loader applies to the tree's own root
    // rather than creating. Its direction is a column because a screen that
    // stacks what it holds is the common case, and a file says `direction`
    // when it wants otherwise.
    ScreenNode screenNode;
    screenNode.style.direction = Direction::Column;
    document.nodes.push_back(std::move(screenNode));

    Walk walk{.document = document, .catalog = catalog, .names = {}, .targets = {}, .templates = {}, .scopes = {}};
    if (const std::expected<void, MarkupError> attributes = ApplyAttributes(walk, root, 0); !attributes)
    {
        return std::unexpected(attributes.error());
    }
    if (const std::expected<void, MarkupError> templates = CollectTemplates(walk, root); !templates)
    {
        return std::unexpected(templates.error());
    }
    if (const std::expected<void, MarkupError> checked = CheckTemplates(walk, root); !checked)
    {
        return std::unexpected(checked.error());
    }
    for (const MarkupElement &child : root.children)
    {
        // Declarations make nothing themselves; their instances do.
        if (child.name == kTemplateElement)
        {
            continue;
        }
        if (const std::expected<void, MarkupError> compiled = CompileElement(walk, child, 0); !compiled)
        {
            return std::unexpected(compiled.error());
        }
    }
    if (const std::expected<void, MarkupError> targets = ResolveTargets(walk); !targets)
    {
        return std::unexpected(targets.error());
    }
    return document;
}

std::expected<std::vector<std::byte>, MarkupError> CompileScreenText(std::string_view text,
                                                                     const Core::EventCatalog &catalog)
{
    const std::expected<MarkupElement, MarkupError> parsed = ParseMarkup(text);
    if (!parsed)
    {
        return std::unexpected(parsed.error());
    }

    const std::expected<ScreenDocument, MarkupError> document = CompileScreen(*parsed, catalog);
    if (!document)
    {
        return std::unexpected(document.error());
    }

    Core::BitWriter writer;
    WriteCookedScreen(writer, *document);
    const std::span<const std::byte> bytes = writer.Data();
    return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

} // namespace Assisi::Mondrian::Import
