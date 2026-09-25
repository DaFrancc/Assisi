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

#include <array>
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

/// What applying one attribute did.
enum class Applied : uint8_t
{
    Unknown,  ///< not an attribute of this kind; the caller tries the next table
    Yes,      ///< read and applied
    BadValue, ///< the right name holding something it cannot read
    Count
};

/// The attributes ApplyAttributes reads itself rather than through a table.
/// The colours are the other such set, and ColorField answers for them.
inline constexpr std::array<std::string_view, 4> kDirectAttributes{"on_click", "pattern", "name", "focus"};

/// An error about @p attribute, placed where it was written.
[[nodiscard]] MarkupError At(const MarkupAttribute &attribute, std::string message);

/// An error about @p element, placed where it was written.
[[nodiscard]] MarkupError At(const MarkupElement &element, std::string message);

/// The built-in element called @p name, or null.
[[nodiscard]] const ElementKind *FindElement(std::string_view name);

/// Every element this build knows, for the message an unknown one prints.
[[nodiscard]] std::string KnownElements();

/// The style half of the attribute table: everything about how a node looks and
/// how it places what is inside it.
[[nodiscard]] Applied ApplyStyleAttribute(Style &style, std::string_view name, std::string_view value);

/// The attributes that belong to one kind of control and to no other: the
/// arguments its own call takes. Tried before the tables every node shares, so
/// a control spells its own arguments its own way.
[[nodiscard]] Applied ApplyWidgetAttribute(ScreenNode &node, std::string_view name, std::string_view value);

/// Everything about a node that is not its style and not its behaviour.
[[nodiscard]] Applied ApplyNodeAttribute(ScreenNode &node, std::string_view name, std::string_view value);

/// The attributes only the root carries: what the screen is, rather than what
/// any one node looks like.
[[nodiscard]] Applied ApplyScreenAttribute(ScreenDocument &document, std::string_view name, std::string_view value);

/// The colour field @p name sets, or null when @p name is no colour attribute.
[[nodiscard]] Color *ColorField(Style &style, std::string_view name);

/// Whether @p name is an attribute some element has, found by offering it to
/// every table as each kind of control would.
[[nodiscard]] bool IsMarkupAttribute(std::string_view name);

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
