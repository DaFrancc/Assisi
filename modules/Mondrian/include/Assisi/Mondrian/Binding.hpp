/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Binding.hpp
/// @brief A field of a screen's context, shown in a node: `{Component.field}`
/// in its text, or a bool deciding whether it is visible.
///
/// Reflection says what `CharacterState.groundSpeed` is; the screen's context
/// says whose. Both the cook and the loader read a binding with what is here,
/// so the two cannot disagree about what one says.

#include <Assisi/Core/Reflect/ComponentMeta.hpp>
#include <Assisi/Core/Reflect/FieldMeta.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::Mondrian
{

/// @brief What opens and closes a binding in text. Written twice, each is
/// itself: `{{` shows one `{`.
inline constexpr char kBindingOpen = '{';
inline constexpr char kBindingClose = '}';

/// @brief What separates a binding's context from its component, and its field
/// from its format: `{boss:Health.current:.0f}`.
inline constexpr char kBindingSeparator = ':';

/// @brief What separates a component from its field.
inline constexpr char kFieldSeparator = '.';

/// @brief One binding as a file writes it.
struct BindingSource
{
    /// Empty for the screen's unnamed context.
    std::string context;
    std::string component;
    std::string field;
    /// A std::format specification, without its colon; empty for the field's
    /// own way of showing.
    std::string format;
};

/// @brief One stretch of a bound text: words shown as they are, then the
/// binding that follows them, when there is one.
struct TextRun
{
    std::string words;
    BindingSource binding;
    bool bound = false;
};

/// @brief Why a binding was refused.
enum class BindingError : uint8_t
{
    Unclosed,         ///< a `{` that nothing closes
    StrayClose,       ///< a `}` that closes nothing; `}}` is a brace
    BadShape,         ///< not `context:Component.field:format`, each part a name
    NotOneBinding,    ///< `visible` holding more than one binding, or words
    UnknownComponent, ///< no component of that name is reflected
    UnknownField,     ///< the component has no field of that name
    Unshowable,       ///< text cannot show a field of its type
    BadFormat,        ///< the format does not suit the field's type
    NotBool,          ///< `visible` bound to a field that is not a bool
    Count_
};

[[nodiscard]] std::string_view ToString(BindingError error) noexcept;

/// @brief @p text as its stretches, each `{{` and `}}` read as one brace.
[[nodiscard]] std::expected<std::vector<TextRun>, BindingError> SplitBoundText(std::string_view text);

/// @brief @p text read as one binding and nothing around it: what `visible`
/// takes.
[[nodiscard]] std::expected<BindingSource, BindingError> ReadSoleBinding(std::string_view text);

/// @brief What a binding feeds: the words of a text, or a yes or no.
enum class BindingUse : uint8_t
{
    Text,
    Bool,
    Count_
};

/// @brief A binding found among the components that exist: where its value
/// is, and how it is shown.
struct FieldBinding
{
    std::string context;
    /// The whole std::format string a value is shown with, braces and all.
    std::string format;
    /// Owned by whatever it was looked up in, which outlives every screen.
    const Core::Reflect::ComponentMeta *component = nullptr;
    const Core::Reflect::FieldMeta *field = nullptr;
};

/// @brief Finds a component by the name a binding writes, or null when none is
/// called that. What the cook and a screen's world each look components up by.
using ComponentLookup = std::function<const Core::Reflect::ComponentMeta *(std::string_view name)>;

/// @brief @p source found in @p component, the component its name looked up,
/// for @p use: refused when there is no component or no such field in it, or
/// when @p use cannot take the field.
[[nodiscard]] std::expected<FieldBinding, BindingError> ResolveBinding(const BindingSource &source,
                                                                       const Core::Reflect::ComponentMeta *component,
                                                                       BindingUse use);

/// @brief The field @p binding names, in @p component, as text.
[[nodiscard]] std::string ShowField(const FieldBinding &binding, const void *component);

/// @brief The bool field @p binding names, in @p component.
[[nodiscard]] bool ReadBoolField(const FieldBinding &binding, const void *component);

} // namespace Assisi::Mondrian
