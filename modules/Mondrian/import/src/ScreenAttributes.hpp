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
};

/// Which elements an attribute belongs to. In the order a name is looked up in:
/// a screen's own attributes first, then a control's, then those every node
/// shares, so a control spells its own arguments its own way.
enum class AttributeOwner : uint8_t
{
    Screen,  ///< the root only: what the screen is
    Control, ///< one kind of control: the arguments its own call takes
    AnyNode, ///< every node, the root included
    Count
};

/// An attribute ApplyAttributes reads itself, because what it does needs more
/// than one field and a value: the file around it, or a message of its own.
/// Every enumerator has a table entry and a case in ApplyAttributes, which the
/// compiler holds both to.
enum class HandledAttribute : uint8_t
{
    None,        ///< read through the entry's own function
    Action,      ///< `on_click`: reads the file's templates and names
    Name,        ///< `name`: qualified by the instance it is in, and unique
    Focus,       ///< `focus`: at most one node on a screen
    Pattern,     ///< `pattern`: a failed compile has a message of its own
    Background,  ///< a colour: each likely mistake has a message of its own
    BorderColor, ///< as Background
    TextColor,   ///< as Background
    Count
};

/// What an attribute is written onto: the screen, and the node it is on, which
/// for the root is the screen's first node.
struct AttributeTarget
{
    ScreenDocument &document;
    ScreenNode &node;
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

/// The colour field a colour attribute sets.
[[nodiscard]] Color &ColorField(Style &style, HandledAttribute colour);

/// The expression `pattern` names or spells, compiled once here to find out
/// whether it would.
///
/// What the file carries is the expression and never the name, so nothing after
/// the cook has a table to look one up in. A value that is neither a name nor
/// delimited is refused rather than taken for an expression: a misspelt name
/// would otherwise compile into a rule matching its own letters, which accepts
/// nothing a player could type and looks like the field is simply broken.
[[nodiscard]] std::expected<void, MarkupError> ApplyPattern(ScreenNode &node, const MarkupAttribute &attribute);

/// What a control's own call would have set before a file says anything.
///
/// A document is fully resolved and the loader applies its style whole, so
/// whatever the node API gives a control has to be here — otherwise a field
/// written in a file would be an invisible box where one built in code is not.
void Seed(ScreenNode &node);

/// What an element has to say for itself, as against what it may say.
/// @p stepWritten is whether `step` was written anywhere the node took its
/// attributes from: on an instance, that is its template's root as well as the
/// instance. Errors point at @p element.
[[nodiscard]] std::expected<void, MarkupError> Required(const MarkupElement &element, const ScreenNode &node,
                                                        bool stepWritten);

/// The verb a file spells @p name, or nullopt.
[[nodiscard]] std::optional<ScreenVerb> LookUpVerb(std::string_view name);

/// How @p verb is written with everything it takes, for a message saying so.
[[nodiscard]] std::string Signature(ScreenVerb verb);

/// Every verb as it is written, for the message an unknown one prints.
[[nodiscard]] std::string KnownVerbs();

} // namespace Assisi::Mondrian::Import
