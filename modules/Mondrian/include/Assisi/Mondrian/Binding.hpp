/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Binding.hpp
/// @brief A field shown in a node: `{Component.field}` in its text, a bool
/// deciding whether it is visible, or what a control shows, and with
/// `{=Component.field}` writes.
///
/// Reflection says what `CharacterState.groundSpeed` is; the screen's context
/// says whose. A field can also sit in a registered source, a struct on no
/// entity, which is written the same way and needs no context. Both the cook
/// and the loader read a binding with what is here, so the two cannot disagree
/// about what one says.

#include <Assisi/Mondrian/Widget.hpp>

#include <Assisi/Core/Reflect/ComponentMeta.hpp>
#include <Assisi/Core/Reflect/FieldMeta.hpp>
#include <Assisi/Core/Reflect/StructMeta.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
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

/// @brief What starts a binding a control also writes: `{=Light.intensity}`.
inline constexpr char kTwoWayMark = '=';

/// @brief What joins a binding's key to the value it labels: the key
/// `options:quality` shows the enumerator High as the entry `quality.High`.
inline constexpr char kLabelSeparator = '.';

/// @brief What labels each value of a bool, after that separator.
inline constexpr std::string_view kTrueLabel = "true";
inline constexpr std::string_view kFalseLabel = "false";

/// @brief One binding as a file writes it.
struct BindingSource
{
    /// Empty for the screen's unnamed context.
    std::string context;
    /// The component, or the struct of a registered source.
    std::string component;
    std::string field;
    /// A std::format specification, without its colon; empty for the field's
    /// own way of showing. On an enum or a bool, a `#` starts a string table's
    /// key instead.
    std::string format;
    /// Whether it was written `{=...}`, for a control to write as well as show.
    bool twoWay = false;
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
    NotOneBinding,    ///< `visible` or a control holding more than one binding, or words
    UnknownComponent, ///< no component or struct of that name is reflected
    AmbiguousName,    ///< a component and a struct share the name
    UnknownField,     ///< the component or struct has no field of that name
    Unshowable,       ///< text cannot show a field of its type
    BadFormat,        ///< the format does not suit the field's type or its place
    BadKey,           ///< an enum's or a bool's key that is not `#table:key`
    NotBool,          ///< `visible` bound to a field that is not a bool
    TwoWayElsewhere,  ///< `{=...}` anywhere but a control
    NotAControl,      ///< a control's binding on a node that holds no value
    WrongControl,     ///< a control bound to a field of a type it cannot hold
    SourceContext,    ///< a registered source read through a context
    Count_
};

[[nodiscard]] std::string_view ToString(BindingError error) noexcept;

/// @brief @p text as its stretches, each `{{` and `}}` read as one brace.
[[nodiscard]] std::expected<std::vector<TextRun>, BindingError> SplitBoundText(std::string_view text);

/// @brief @p text read as one binding and nothing around it: what `visible`
/// and a control take.
[[nodiscard]] std::expected<BindingSource, BindingError> ReadSoleBinding(std::string_view text);

/// @brief What a binding feeds: the words of a text, a yes or no, or a control.
enum class BindingUse : uint8_t
{
    Text,
    Bool,
    Toggle,
    Slider,
    Stepped,
    Count_
};

/// @brief What a binding on a @p widget feeds, or nothing for a node that holds
/// no value for one to feed.
[[nodiscard]] std::optional<BindingUse> ControlUse(BuiltinWidget widget);

/// @brief What a binding's name found.
struct BindingOwner
{
    const Core::Reflect::ComponentMeta *component = nullptr;
    /// The struct of a registered source.
    const Core::Reflect::StructSpec *structure = nullptr;
};

/// @brief A binding found among the fields that exist: where its value is, and
/// how it is shown.
struct FieldBinding
{
    std::string context;
    /// The whole std::format string a value is shown with, braces and all.
    std::string format;
    /// The `table:key` this field's values are labelled from, or empty when
    /// they show as themselves.
    std::string labelKey;
    /// Exactly one of these two. Each is owned by whatever it was looked up in,
    /// which outlives every screen, as is field.
    const Core::Reflect::ComponentMeta *component = nullptr;
    const Core::Reflect::StructSpec *structure = nullptr;
    const Core::Reflect::FieldMeta *field = nullptr;
    BindingUse use = BindingUse::Text;
    /// Whether a control writes this field as well as showing it.
    bool twoWay = false;
};

/// @brief Finds a component by the name a binding writes, or null when none is
/// called that. What the cook and a screen's world each look components up by.
using ComponentLookup = std::function<const Core::Reflect::ComponentMeta *(std::string_view name)>;

/// @brief The same for the struct a registered source would be.
using StructLookup = std::function<const Core::Reflect::StructSpec *(std::string_view name)>;

/// @brief @p source found in @p owner, what its name looked up, for @p use:
/// refused when there is no such owner or no such field in it, or when @p use
/// cannot take the field.
[[nodiscard]] std::expected<FieldBinding, BindingError> ResolveBinding(const BindingSource &source, BindingOwner owner,
                                                                       BindingUse use);

/// @brief The field @p binding names, in @p owner, as text.
[[nodiscard]] std::string ShowField(const FieldBinding &binding, const void *owner);

/// @brief The `table:key` holding the words for what the field @p binding names
/// holds in @p owner. Nothing when it has no key, or for an enum holding a value
/// with no enumerator, which ShowField shows as a number.
[[nodiscard]] std::optional<std::string> LabelOf(const FieldBinding &binding, const void *owner);

/// @brief Every `table:key` the field @p binding names can be labelled from:
/// what a table has to hold for it, one per enumerator or both of a bool's.
[[nodiscard]] std::vector<std::string> LabelsOf(const FieldBinding &binding);

/// @brief Whether text showing @p binding shows a word from the program: an enum
/// or a bool with no key, which no table can translate.
[[nodiscard]] bool ShowsProgramWords(const FieldBinding &binding);

/// @brief The bool field @p binding names, in @p owner.
[[nodiscard]] bool ReadBoolField(const FieldBinding &binding, const void *owner);

/// @brief The field @p binding names, in @p owner, as the control it feeds
/// carries it. Nothing for an enum holding a value with no enumerator, which no
/// position of a stepped slider stands for.
[[nodiscard]] std::optional<WidgetValue> ReadControlField(const FieldBinding &binding, const void *owner);

/// @brief Puts @p value, what a control carries, into the field @p binding
/// names, in @p owner: rounded for a whole number, and held to what it can store.
void WriteControlField(const FieldBinding &binding, void *owner, const WidgetValue &value);

} // namespace Assisi::Mondrian
