/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ScreenAttributes.hpp
/// @brief What each element and attribute of the screen markup means, one
/// element or one attribute at a time, with nothing about the file around it.
///
/// Internal to the import library.

#include "MarkupValues.hpp"

#include <Assisi/Mondrian/Import/Markup.hpp>

#include <Assisi/Mondrian/ScreenDocument.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace Assisi::Mondrian::Import
{

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
    /// Whether its name says which way it lays its children out, as row and
    /// column do, so that direction is the node's own and a style it names
    /// gives way to it.
    bool namesDirection;
};

/// Which elements an attribute belongs to. In the order a name is looked up in:
/// a screen's own attributes first, then a control's, then those every node
/// shares, so a control spells its own arguments its own way.
enum class AttributeOwner : uint8_t
{
    Screen,  ///< the root only: what the screen is
    Control, ///< one kind of control: the arguments its own call takes
    AnyNode, ///< every node, the root included
    Count_
};

/// An attribute ApplyAttributes reads itself, because what it does needs more
/// than one field and a value: the file around it, or a message of its own.
/// Every enumerator has a table entry and a case in ApplyAttributes, which the
/// compiler holds both to.
enum class HandledAttribute : uint8_t
{
    None,        ///< read through the entry's own function
    Action,      ///< `on_click`: reads the file's templates and names
    Change,      ///< `on_change`: an event that must take what the control holds
    Name,        ///< `name`: qualified by the instance it is in, and unique
    Focus,       ///< `focus`: at most one node on a screen
    Pattern,     ///< `pattern`: a failed compile has a message of its own
    StyleName,   ///< `style`: checked against every theme the project lists
    Sprite,      ///< `sprite`: checked against every sheet the project lists
    Texture,     ///< `texture`: checked against the images the project has
    Background,  ///< a colour: each likely mistake has a message of its own
    BorderColor, ///< as Background
    TextColor,   ///< as Background
    Tint,        ///< as Background
    Count_
};

/// What an attribute is written onto: the screen, the node it is on, which for
/// the root is the screen's first node, and the style an attribute about how
/// the node looks writes. That is the node's own, or, for an attribute given a
/// state, the look the node has in it; an entry writing a style writes only
/// here, so the one entry serves both.
struct AttributeTarget
{
    ScreenDocument &document;
    ScreenNode &node;
    Style &style;
};

/// Reads @p value into what @p target holds, saying whether it could.
using ApplyAttribute = bool (*)(const AttributeTarget &target, std::string_view value);

/// One attribute the markup has.
struct AttributeSpec
{
    std::string_view name;
    /// How it is read, for one ApplyAttributes does not handle itself.
    ApplyAttribute apply = nullptr;
    AttributeOwner owner = AttributeOwner::AnyNode;
    /// The control it belongs to, for AttributeOwner::Control.
    BuiltinWidget widget = BuiltinWidget::None;
    HandledAttribute handled = HandledAttribute::None;
    /// The part of the node's style it writes, for one about how the node
    /// looks or is laid out, which is what a state may change. Empty for one
    /// about what the node is or does. The `{}` is what lets an entry be built
    /// without naming it: GCC's missing-initializer warning passes over a
    /// member that has an initializer of its own.
    std::optional<StyleProperty> property{};
};

/// An error about @p attribute, placed where it was written.
[[nodiscard]] MarkupError At(const MarkupAttribute &attribute, std::string message);

/// An error about @p element, placed where it was written.
[[nodiscard]] MarkupError At(const MarkupElement &element, std::string message);

/// The built-in element called @p name, or null.
[[nodiscard]] const ElementKind *FindElement(std::string_view name);

/// Every element this build knows, for the message an unknown one prints.
[[nodiscard]] std::string KnownElements();

/// The attribute @p name means on a node of @p widget, or null when it means
/// nothing there. @p isRoot adds the screen's own.
[[nodiscard]] const AttributeSpec *FindAttribute(std::string_view name, BuiltinWidget widget, bool isRoot);

/// Whether @p name is an attribute any element has.
[[nodiscard]] bool IsMarkupAttribute(std::string_view name);

/// Reads the colour @p attribute writes into @p style. A colour's likeliest
/// mistakes each have one fix to name, which a table entry saying only "bad
/// value" could not.
[[nodiscard]] std::expected<void, MarkupError> ApplyColor(Style &style, HandledAttribute colour,
                                                          const MarkupAttribute &attribute);

/// The attribute every node has that writes @p property.
[[nodiscard]] const AttributeSpec &AttributeFor(StyleProperty property);

/// Reads @p attribute, which @p spec says writes a property of a style, into
/// @p style, and nothing else.
[[nodiscard]] std::expected<void, MarkupError> ApplyStyleAttribute(const AttributeSpec &spec,
                                                                   const MarkupAttribute &attribute, Style &style);

/// The state @p attribute is written for, or none for one written with no
/// prefix. A prefix that is no state, or more than one, is refused.
[[nodiscard]] std::expected<std::optional<StyleState>, MarkupError> StateOf(const MarkupAttribute &attribute);

/// What @p name says after its prefix, or all of it when it has none.
[[nodiscard]] std::string_view AfterPrefix(std::string_view name);

/// The state a file spells @p name, or nullopt.
[[nodiscard]] std::optional<StyleState> LookUpState(std::string_view name);

/// Every state as it is written, in the order they stack, for the message an
/// unknown one prints.
[[nodiscard]] std::string KnownStates();

/// The expression `pattern` names or spells, compiled once here to find out
/// whether it would.
///
/// What the file carries is the expression and never the name, so nothing after
/// the cook has a table to look one up in. A value that is neither a name nor
/// delimited is refused rather than taken for an expression: a misspelt name
/// would otherwise compile into a rule matching its own letters, which accepts
/// nothing a player could type and looks like the field is simply broken.
[[nodiscard]] std::expected<void, MarkupError> ApplyPattern(ScreenDocument &document, ScreenNode &node,
                                                            const MarkupAttribute &attribute);

/// What a control's own call would have set before a file says anything.
///
/// A document is fully resolved and the loader applies its style whole, so
/// whatever the node API gives a control has to be here — otherwise a field
/// written in a file would be an invisible box where one built in code is not.
void Seed(ScreenNode &node);

/// What an element has to say for itself, as against what it may say.
/// @p stepWritten is whether `step` was written anywhere the node took its
/// attributes from: on an instance, that is its template's root as well as the
/// instance. @p stepKnown is whether that step has a value yet: in a template's
/// own check, one naming a parameter with no default has none, and is checked
/// per instance instead. Errors point at @p element.
[[nodiscard]] std::expected<void, MarkupError> Required(const MarkupElement &element, const ScreenNode &node,
                                                        bool stepWritten, bool stepKnown);

/// The verb a file spells @p name, or nullopt.
[[nodiscard]] std::optional<ScreenVerb> LookUpVerb(std::string_view name);

/// How @p verb is written with everything it takes, for a message saying so.
[[nodiscard]] std::string Signature(ScreenVerb verb);

/// Every verb as it is written, for the message an unknown one prints.
[[nodiscard]] std::string KnownVerbs();

} // namespace Assisi::Mondrian::Import
