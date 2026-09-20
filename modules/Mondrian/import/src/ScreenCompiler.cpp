/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Mondrian/Import/ScreenCompiler.hpp>

#include "MarkupValues.hpp"

#include <Assisi/Mondrian/ScreenBlob.hpp>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/EventCatalog.hpp>

#include <span>
#include <string>
#include <utility>

namespace Assisi::Mondrian::Import
{
namespace
{

/// The element every file's root is.
constexpr std::string_view kScreenElement = "screen";

/// The verb an `on_click` names when it acts on the screen and nothing else.
constexpr std::string_view kHideVerb = "hide";

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

constexpr std::array<ElementKind, 4> kElements{{
    {"row", BuiltinWidget::None, Direction::Row, false, true},
    {"column", BuiltinWidget::None, Direction::Column, false, true},
    {"text", BuiltinWidget::None, Direction::Row, true, false},
    {"button", BuiltinWidget::Button, Direction::Row, true, false},
}};

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

/// Everything about a node that is not its style and not its behaviour.
Applied ApplyNodeAttribute(ScreenNode &node, std::string_view name, std::string_view value)
{
    if (name == "name")
    {
        node.name = value;
        return Applied::Yes;
    }
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

/// What a control does when it fires. A verb is a closed set; anything else has
/// to be an event this build declares, which is the check the whole catalog
/// exists for.
std::expected<void, MarkupError> ApplyAction(ScreenNode &node, const MarkupAttribute &attribute,
                                             const Core::EventCatalog &catalog)
{
    if (attribute.value == kHideVerb)
    {
        node.action = ActionKind::Verb;
        node.verb = ScreenVerb::Hide;
        return {};
    }
    if (catalog.Find(attribute.value) == nullptr)
    {
        return std::unexpected(At(attribute, "'" + attribute.value + "' is not '" + std::string{kHideVerb} +
                                                 "' and names no event this build declares. An event is a "
                                                 "struct marked AEVENT() in a reflected header, named here "
                                                 "by its full C++ name."));
    }
    node.action = ActionKind::Event;
    node.eventName = attribute.value;
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

/// What compiling one element needs to know about the file around it.
struct Walk
{
    ScreenDocument &document;
    const Core::EventCatalog &catalog;
    /// Whether some node has already claimed focus, so a second can be refused
    /// rather than quietly winning.
    bool focusClaimed = false;
};

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
            if (const std::expected<void, MarkupError> action = ApplyAction(node, attribute, walk.catalog); !action)
            {
                return std::unexpected(action.error());
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

std::expected<void, MarkupError> CompileElement(Walk &walk, const MarkupElement &element, uint32_t parent)
{
    const ElementKind *const kind = FindElement(element.name);
    if (kind == nullptr)
    {
        return std::unexpected(
            At(element, "'" + element.name + "' is not an element this markup has. It has: " + KnownElements() + "."));
    }
    if (!kind->carriesText && !element.text.empty())
    {
        return std::unexpected(At(element, "'" + element.name +
                                               "' holds text, which only text and button "
                                               "carry. Put the words in a <text>."));
    }
    if (!kind->carriesChildren && !element.children.empty())
    {
        return std::unexpected(At(element, "'" + element.name + "' holds elements, and holds only text."));
    }

    ScreenNode node;
    node.parent = parent;
    node.widget = kind->widget;
    node.text = element.text;
    node.style.direction = kind->direction;

    const uint32_t index = static_cast<uint32_t>(walk.document.nodes.size());
    walk.document.nodes.push_back(std::move(node));

    if (const std::expected<void, MarkupError> attributes = ApplyAttributes(walk, element, index); !attributes)
    {
        return std::unexpected(attributes.error());
    }
    return CompileChildren(walk, element, index);
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

    Walk walk{.document = document, .catalog = catalog};
    if (const std::expected<void, MarkupError> attributes = ApplyAttributes(walk, root, 0); !attributes)
    {
        return std::unexpected(attributes.error());
    }
    if (const std::expected<void, MarkupError> children = CompileChildren(walk, root, 0); !children)
    {
        return std::unexpected(children.error());
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
