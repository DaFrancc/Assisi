/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Binding.hpp>

#include <Assisi/Core/InternedString.hpp>
#include <Assisi/Core/ShortString.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>

namespace Assisi::Mondrian
{
namespace
{

using Core::Reflect::FieldMeta;
using Core::Reflect::FieldType;

/// How many floats each vector type holds.
constexpr std::size_t kVec2Floats = 2;
constexpr std::size_t kVec3Floats = 3;
constexpr std::size_t kVec4Floats = 4;

/// What goes between a vector's components: `1.0, 2.0, 3.0`.
constexpr std::string_view kVectorSeparator = ", ";

/// What a field's value is shown as, which decides the formats it takes.
enum class ShownAs : uint8_t
{
    Floating,
    Signed,
    Unsigned,
    Truth,
    Words,
    Vector,
    Nothing, ///< text cannot show it
    Count_
};

ShownAs KindOf(const FieldMeta &field)
{
    switch (field.type)
    {
    case FieldType::Float:
    case FieldType::Double:
        return ShownAs::Floating;
    case FieldType::Int8:
    case FieldType::Int16:
    case FieldType::Int32:
    case FieldType::Int64:
        return ShownAs::Signed;
    case FieldType::UInt8:
    case FieldType::UInt16:
    case FieldType::UInt32:
    case FieldType::UInt64:
        return ShownAs::Unsigned;
    case FieldType::Bool:
        return ShownAs::Truth;
    case FieldType::Enum:
    case FieldType::String:
    case FieldType::InternedString:
        return ShownAs::Words;
    case FieldType::Vec2:
    case FieldType::Vec3:
    case FieldType::Vec4:
        return ShownAs::Vector;
    default:
        // Every other type is a reference, a matrix, a container or a struct,
        // none of which reads as a line of a HUD.
        return ShownAs::Nothing;
    }
}

/// How many floats a vector field holds.
std::size_t FloatsIn(FieldType type)
{
    switch (type)
    {
    case FieldType::Vec2:
        return kVec2Floats;
    case FieldType::Vec3:
        return kVec3Floats;
    default:
        return kVec4Floats;
    }
}

bool IsDigit(char c)
{
    return c >= '0' && c <= '9';
}

bool IsNameCharacter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || IsDigit(c) || c == '_';
}

/// Whether @p text is a name: letters, digits and '_', not starting with a
/// digit.
bool IsName(std::string_view text)
{
    return !text.empty() && !IsDigit(text.front()) && std::ranges::all_of(text, IsNameCharacter);
}

/// What one pair of braces holds: `context:Component.field:format`, with the
/// context and the format optional.
std::expected<BindingSource, BindingError> ReadInside(std::string_view inside)
{
    const std::size_t dot = inside.find(kFieldSeparator);
    if (dot == std::string_view::npos)
    {
        return std::unexpected(BindingError::BadShape);
    }
    std::string_view head = inside.substr(0, dot);
    std::string_view tail = inside.substr(dot + 1);

    BindingSource source;
    if (const std::size_t colon = head.find(kBindingSeparator); colon != std::string_view::npos)
    {
        source.context = head.substr(0, colon);
        head = head.substr(colon + 1);
        if (!IsName(source.context))
        {
            return std::unexpected(BindingError::BadShape);
        }
    }
    if (const std::size_t colon = tail.find(kBindingSeparator); colon != std::string_view::npos)
    {
        source.format = tail.substr(colon + 1);
        tail = tail.substr(0, colon);
    }
    source.component = head;
    source.field = tail;
    if (!IsName(source.component) || !IsName(source.field))
    {
        return std::unexpected(BindingError::BadShape);
    }
    return source;
}

/// A @p T read from @p at, which need not be aligned for one.
template <typename T> T ReadAt(const std::byte *at)
{
    T value{};
    std::memcpy(&value, at, sizeof(T));
    return value;
}

template <typename T> std::string Show(const std::string &format, T value)
{
    return std::vformat(format, std::make_format_args(value));
}

/// @p value as @p format shows it, without the sign of a value that shows as
/// zero: a player reading a speed sees no difference between 0 and a hair
/// below it, and a "-0.0" flickering beside "0.0" reads as a fault.
template <typename T> std::string ShowReal(const std::string &format, T value)
{
    std::string shown = Show(format, value);
    if (std::signbit(value) && shown == Show(format, -T{0}))
    {
        return Show(format, T{0});
    }
    return shown;
}

/// Whether @p format can show a value of @p kind: tried on a stand-in of it,
/// which is the only test std::format offers.
bool FormatSuits(const std::string &format, ShownAs kind)
{
    try
    {
        switch (kind)
        {
        case ShownAs::Floating:
        case ShownAs::Vector:
            (void)Show(format, 0.0);
            break;
        case ShownAs::Signed:
            (void)Show(format, int64_t{0});
            break;
        case ShownAs::Unsigned:
            (void)Show(format, uint64_t{0});
            break;
        case ShownAs::Truth:
            (void)Show(format, false);
            break;
        case ShownAs::Words:
            (void)Show(format, std::string_view{});
            break;
        case ShownAs::Nothing:
        case ShownAs::Count_:
            return false;
        }
    }
    catch (const std::format_error &)
    {
        return false;
    }
    return true;
}

/// The enumerator @p field holds at @p at, by name, or its number when the
/// enum names none for it.
std::string EnumName(const FieldMeta &field, const std::byte *at)
{
    int64_t value = 0;
    switch (field.enumSize)
    {
    case sizeof(int8_t):
        value = field.enumSigned ? ReadAt<int8_t>(at) : int64_t{ReadAt<uint8_t>(at)};
        break;
    case sizeof(int16_t):
        value = field.enumSigned ? ReadAt<int16_t>(at) : int64_t{ReadAt<uint16_t>(at)};
        break;
    case sizeof(int32_t):
        value = field.enumSigned ? ReadAt<int32_t>(at) : int64_t{ReadAt<uint32_t>(at)};
        break;
    default:
        value = ReadAt<int64_t>(at);
        break;
    }
    const std::vector<Core::Reflect::EnumConstant>::const_iterator named =
        std::ranges::find(field.enumConstants, value, &Core::Reflect::EnumConstant::value);
    return named != field.enumConstants.end() ? named->name : std::to_string(value);
}

/// The words a Words field holds at @p at.
std::string WordsAt(const FieldMeta &field, const std::byte *at)
{
    switch (field.type)
    {
    case FieldType::Enum:
        return EnumName(field, at);
    case FieldType::String:
        return std::string{static_cast<const Core::ShortString *>(static_cast<const void *>(at))->View()};
    default:
        return std::string{static_cast<const Core::InternedString *>(static_cast<const void *>(at))->View()};
    }
}

std::string ShowVector(const std::string &format, const FieldMeta &field, const std::byte *at)
{
    std::string shown;
    const std::size_t floats = FloatsIn(field.type);
    for (std::size_t index = 0; index < floats; ++index)
    {
        if (index > 0)
        {
            shown += kVectorSeparator;
        }
        shown += ShowReal(format, ReadAt<float>(at + (index * sizeof(float))));
    }
    return shown;
}

std::string ShowSigned(const std::string &format, FieldType type, const std::byte *at)
{
    switch (type)
    {
    case FieldType::Int8:
        return Show(format, ReadAt<int8_t>(at));
    case FieldType::Int16:
        return Show(format, ReadAt<int16_t>(at));
    case FieldType::Int32:
        return Show(format, ReadAt<int32_t>(at));
    default:
        return Show(format, ReadAt<int64_t>(at));
    }
}

std::string ShowUnsigned(const std::string &format, FieldType type, const std::byte *at)
{
    switch (type)
    {
    case FieldType::UInt8:
        return Show(format, ReadAt<uint8_t>(at));
    case FieldType::UInt16:
        return Show(format, ReadAt<uint16_t>(at));
    case FieldType::UInt32:
        return Show(format, ReadAt<uint32_t>(at));
    default:
        return Show(format, ReadAt<uint64_t>(at));
    }
}

} // namespace

std::string_view ToString(BindingError error) noexcept
{
    switch (error)
    {
    case BindingError::Unclosed:
        return "opens a binding with { that nothing closes; a { shown as it is is written {{";
    case BindingError::StrayClose:
        return "holds a } that closes no binding; a } shown as it is is written }}";
    case BindingError::BadShape:
        return "holds a binding that is not {Component.field}, {context:Component.field} or either with :format "
               "after it";
    case BindingError::NotOneBinding:
        return "holds something other than one binding, such as {Component.field}";
    case BindingError::UnknownComponent:
        return "names a component no reflected type has";
    case BindingError::UnknownField:
        return "names a field its component does not have";
    case BindingError::Unshowable:
        return "names a field text cannot show: numbers, bools, enums, strings and vectors can be";
    case BindingError::BadFormat:
        return "gives a format its field's type does not take";
    case BindingError::NotBool:
        return "binds a field that is not a bool";
    case BindingError::Count_:
        break;
    }
    return "unknown";
}

std::expected<std::vector<TextRun>, BindingError> SplitBoundText(std::string_view text)
{
    std::vector<TextRun> runs(1);
    std::size_t at = 0;
    while (at < text.size())
    {
        const char character = text[at];
        const bool doubled = at + 1 < text.size() && text[at + 1] == character;
        if (character != kBindingOpen && character != kBindingClose)
        {
            runs.back().words += character;
            ++at;
            continue;
        }
        if (doubled)
        {
            runs.back().words += character;
            at += 2;
            continue;
        }
        if (character == kBindingClose)
        {
            return std::unexpected(BindingError::StrayClose);
        }
        const std::size_t close = text.find(kBindingClose, at + 1);
        if (close == std::string_view::npos)
        {
            return std::unexpected(BindingError::Unclosed);
        }
        std::expected<BindingSource, BindingError> source = ReadInside(text.substr(at + 1, close - at - 1));
        if (!source)
        {
            return std::unexpected(source.error());
        }
        runs.back().binding = *std::move(source);
        runs.back().bound = true;
        runs.emplace_back();
        at = close + 1;
    }
    // Words after the last binding, or none: a run holding neither says nothing.
    if (runs.size() > 1 && runs.back().words.empty())
    {
        runs.pop_back();
    }
    return runs;
}

std::expected<BindingSource, BindingError> ReadSoleBinding(std::string_view text)
{
    std::expected<std::vector<TextRun>, BindingError> runs = SplitBoundText(text);
    if (!runs)
    {
        return std::unexpected(runs.error());
    }
    if (runs->size() != 1 || !runs->front().bound || !runs->front().words.empty())
    {
        return std::unexpected(BindingError::NotOneBinding);
    }
    return std::move(runs->front().binding);
}

std::expected<FieldBinding, BindingError> ResolveBinding(const BindingSource &source,
                                                         const Core::Reflect::ComponentMeta *component, BindingUse use)
{
    if (component == nullptr)
    {
        return std::unexpected(BindingError::UnknownComponent);
    }
    const std::vector<FieldMeta>::const_iterator field =
        std::ranges::find(component->fields, source.field, &FieldMeta::name);
    if (field == component->fields.end())
    {
        return std::unexpected(BindingError::UnknownField);
    }

    FieldBinding binding{.context = source.context,
                         .format = "{:" + source.format + "}",
                         .component = component,
                         .field = &*field};
    switch (use)
    {
    case BindingUse::Bool:
        if (field->type != FieldType::Bool)
        {
            return std::unexpected(BindingError::NotBool);
        }
        if (!source.format.empty())
        {
            return std::unexpected(BindingError::BadFormat);
        }
        return binding;
    case BindingUse::Text:
    case BindingUse::Count_:
        break;
    }
    const ShownAs kind = KindOf(*field);
    if (kind == ShownAs::Nothing)
    {
        return std::unexpected(BindingError::Unshowable);
    }
    if (!FormatSuits(binding.format, kind))
    {
        return std::unexpected(BindingError::BadFormat);
    }
    return binding;
}

std::string ShowField(const FieldBinding &binding, const void *component)
{
    const FieldMeta &field = *binding.field;
    const std::byte *const at = static_cast<const std::byte *>(component) + field.offset;
    switch (KindOf(field))
    {
    case ShownAs::Floating:
        return field.type == FieldType::Float ? ShowReal(binding.format, ReadAt<float>(at))
                                              : ShowReal(binding.format, ReadAt<double>(at));
    case ShownAs::Signed:
        return ShowSigned(binding.format, field.type, at);
    case ShownAs::Unsigned:
        return ShowUnsigned(binding.format, field.type, at);
    case ShownAs::Truth:
        return Show(binding.format, ReadAt<bool>(at));
    case ShownAs::Words:
        return Show(binding.format, std::string_view{WordsAt(field, at)});
    case ShownAs::Vector:
        return ShowVector(binding.format, field, at);
    case ShownAs::Nothing:
    case ShownAs::Count_:
        break;
    }
    return {};
}

bool ReadBoolField(const FieldBinding &binding, const void *component)
{
    return ReadAt<bool>(static_cast<const std::byte *>(component) + binding.field->offset);
}

} // namespace Assisi::Mondrian
