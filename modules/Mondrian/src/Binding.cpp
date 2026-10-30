/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Binding.hpp>

#include <Assisi/Core/InternedString.hpp>
#include <Assisi/Core/ShortString.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <limits>
#include <type_traits>
#include <variant>

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
/// context and the format optional, after a mark when a control writes it.
std::expected<BindingSource, BindingError> ReadInside(std::string_view inside)
{
    BindingSource source;
    if (inside.starts_with(kTwoWayMark))
    {
        source.twoWay = true;
        inside.remove_prefix(1);
    }
    const std::size_t dot = inside.find(kFieldSeparator);
    if (dot == std::string_view::npos)
    {
        return std::unexpected(BindingError::BadShape);
    }
    std::string_view head = inside.substr(0, dot);
    std::string_view tail = inside.substr(dot + 1);

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

/// The value of the enum @p field holds at @p at.
int64_t EnumAt(const FieldMeta &field, const std::byte *at)
{
    switch (field.enumSize)
    {
    case sizeof(int8_t):
        return field.enumSigned ? ReadAt<int8_t>(at) : int64_t{ReadAt<uint8_t>(at)};
    case sizeof(int16_t):
        return field.enumSigned ? ReadAt<int16_t>(at) : int64_t{ReadAt<uint16_t>(at)};
    case sizeof(int32_t):
        return field.enumSigned ? ReadAt<int32_t>(at) : int64_t{ReadAt<uint32_t>(at)};
    default:
        return ReadAt<int64_t>(at);
    }
}

/// Stores @p value at @p at, narrowed to the @p T the field is.
template <typename T> void StoreEnum(std::byte *at, int64_t value)
{
    const T stored = static_cast<T>(value);
    std::memcpy(at, &stored, sizeof(T));
}

/// Puts the enumerator's value @p value into the enum @p field holds at @p at.
/// Narrowed by width alone: the bits are the same whichever way it is signed.
void PutEnum(const FieldMeta &field, std::byte *at, int64_t value)
{
    switch (field.enumSize)
    {
    case sizeof(int8_t):
        StoreEnum<uint8_t>(at, value);
        return;
    case sizeof(int16_t):
        StoreEnum<uint16_t>(at, value);
        return;
    case sizeof(int32_t):
        StoreEnum<uint32_t>(at, value);
        return;
    default:
        StoreEnum<int64_t>(at, value);
        return;
    }
}

/// The enumerator @p field holds at @p at, by name, or its number when the
/// enum names none for it.
std::string EnumName(const FieldMeta &field, const std::byte *at)
{
    const int64_t value = EnumAt(field, at);
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

/// Whether a slider can hold @p field.
bool IsNumber(const FieldMeta &field)
{
    const ShownAs kind = KindOf(field);
    return kind == ShownAs::Floating || kind == ShownAs::Signed || kind == ShownAs::Unsigned;
}

/// The number a field of @p type holds at @p at.
double NumberAt(FieldType type, const std::byte *at)
{
    switch (type)
    {
    case FieldType::Float:
        return ReadAt<float>(at);
    case FieldType::Double:
        return ReadAt<double>(at);
    case FieldType::Int8:
        return ReadAt<int8_t>(at);
    case FieldType::Int16:
        return ReadAt<int16_t>(at);
    case FieldType::Int32:
        return ReadAt<int32_t>(at);
    case FieldType::Int64:
        return static_cast<double>(ReadAt<int64_t>(at));
    case FieldType::UInt8:
        return ReadAt<uint8_t>(at);
    case FieldType::UInt16:
        return ReadAt<uint16_t>(at);
    case FieldType::UInt32:
        return ReadAt<uint32_t>(at);
    default:
        return static_cast<double>(ReadAt<uint64_t>(at));
    }
}

/// @p number as a @p T: rounded when that is a whole number, and held to what
/// one can store, which a slider's range knows nothing of.
template <typename T> T Fitted(double number)
{
    if constexpr (std::is_floating_point_v<T>)
    {
        return static_cast<T>(number);
    }
    else
    {
        // Compared rather than clamped: the top of a 64-bit type rounds up as a
        // double, past what casting back can hold.
        constexpr T kLowest = std::numeric_limits<T>::lowest();
        constexpr T kHighest = std::numeric_limits<T>::max();
        const double whole = std::round(number);
        if (whole <= static_cast<double>(kLowest))
        {
            return kLowest;
        }
        return whole >= static_cast<double>(kHighest) ? kHighest : static_cast<T>(whole);
    }
}

template <typename T> void StoreNumber(std::byte *at, double number)
{
    const T stored = Fitted<T>(number);
    std::memcpy(at, &stored, sizeof(T));
}

/// Puts @p number into the field of @p type at @p at.
void PutNumber(FieldType type, std::byte *at, double number)
{
    switch (type)
    {
    case FieldType::Float:
        StoreNumber<float>(at, number);
        return;
    case FieldType::Double:
        StoreNumber<double>(at, number);
        return;
    case FieldType::Int8:
        StoreNumber<int8_t>(at, number);
        return;
    case FieldType::Int16:
        StoreNumber<int16_t>(at, number);
        return;
    case FieldType::Int32:
        StoreNumber<int32_t>(at, number);
        return;
    case FieldType::Int64:
        StoreNumber<int64_t>(at, number);
        return;
    case FieldType::UInt8:
        StoreNumber<uint8_t>(at, number);
        return;
    case FieldType::UInt16:
        StoreNumber<uint16_t>(at, number);
        return;
    case FieldType::UInt32:
        StoreNumber<uint32_t>(at, number);
        return;
    default:
        StoreNumber<uint64_t>(at, number);
        return;
    }
}

/// Which step a stepped slider rests on for the field @p field at @p at, or
/// nothing for an enum value no enumerator has.
std::optional<WidgetValue> PositionAt(const FieldMeta &field, const std::byte *at)
{
    if (field.type != FieldType::Enum)
    {
        return WidgetValue{Fitted<int32_t>(NumberAt(field.type, at))};
    }
    const std::vector<Core::Reflect::EnumConstant>::const_iterator named =
        std::ranges::find(field.enumConstants, EnumAt(field, at), &Core::Reflect::EnumConstant::value);
    if (named == field.enumConstants.end())
    {
        return std::nullopt;
    }
    return WidgetValue{static_cast<int32_t>(std::distance(field.enumConstants.begin(), named))};
}

/// Puts step @p position of a stepped slider into the field @p field at @p at.
void PutPosition(const FieldMeta &field, std::byte *at, int32_t position)
{
    if (field.type != FieldType::Enum)
    {
        PutNumber(field.type, at, position);
        return;
    }
    if (position >= 0 && static_cast<std::size_t>(position) < field.enumConstants.size())
    {
        PutEnum(field, at, field.enumConstants[static_cast<std::size_t>(position)].value);
    }
}

/// Whether a control of @p use can hold @p field.
bool ControlHolds(BindingUse use, const FieldMeta &field)
{
    switch (use)
    {
    case BindingUse::Toggle:
        return field.type == FieldType::Bool;
    case BindingUse::Slider:
        return IsNumber(field);
    case BindingUse::Stepped:
        return IsNumber(field) || (field.type == FieldType::Enum && !field.enumConstants.empty());
    case BindingUse::Text:
    case BindingUse::Bool:
    case BindingUse::Count_:
        break;
    }
    return false;
}

/// Whether @p use is a control, which is all that writes.
bool IsControl(BindingUse use)
{
    return use == BindingUse::Toggle || use == BindingUse::Slider || use == BindingUse::Stepped;
}

} // namespace

std::optional<BindingUse> ControlUse(BuiltinWidget widget)
{
    switch (widget)
    {
    case BuiltinWidget::Toggle:
        return BindingUse::Toggle;
    case BuiltinWidget::ContinuousSlider:
        return BindingUse::Slider;
    case BuiltinWidget::SteppedSlider:
        return BindingUse::Stepped;
    case BuiltinWidget::None:
    case BuiltinWidget::Button:
    case BuiltinWidget::Scroll:
    case BuiltinWidget::TextField:
    case BuiltinWidget::Count_:
        break;
    }
    return std::nullopt;
}

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
        return "names no reflected component or struct";
    case BindingError::AmbiguousName:
        return "names both a component and a struct, and cannot say which";
    case BindingError::UnknownField:
        return "names a field its component or struct does not have";
    case BindingError::Unshowable:
        return "names a field text cannot show: numbers, bools, enums, strings and vectors can be";
    case BindingError::BadFormat:
        return "gives a format its field's type or its place does not take";
    case BindingError::NotBool:
        return "binds a field that is not a bool";
    case BindingError::TwoWayElsewhere:
        return "writes {=...} where nothing writes it back: only a control's on or value does";
    case BindingError::NotAControl:
        return "binds a node that holds no value: only a toggle, a slider and a stepped slider do";
    case BindingError::WrongControl:
        return "binds a field its control cannot hold: a toggle holds a bool, a slider a number, and a "
               "stepped slider a whole number or an enum";
    case BindingError::SourceContext:
        return "reads a registered source through a context, and a source is on no entity";
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

std::expected<FieldBinding, BindingError> ResolveBinding(const BindingSource &source, BindingOwner owner, BindingUse use)
{
    if (owner.component != nullptr && owner.structure != nullptr)
    {
        return std::unexpected(BindingError::AmbiguousName);
    }
    if (owner.component == nullptr && owner.structure == nullptr)
    {
        return std::unexpected(BindingError::UnknownComponent);
    }
    if (owner.structure != nullptr && !source.context.empty())
    {
        return std::unexpected(BindingError::SourceContext);
    }
    const std::vector<FieldMeta> &fields = owner.component != nullptr ? owner.component->fields : owner.structure->fields;
    const std::vector<FieldMeta>::const_iterator field = std::ranges::find(fields, source.field, &FieldMeta::name);
    if (field == fields.end())
    {
        return std::unexpected(BindingError::UnknownField);
    }
    if (source.twoWay && !IsControl(use))
    {
        return std::unexpected(BindingError::TwoWayElsewhere);
    }

    FieldBinding binding{.context = source.context,
                         .format = "{:" + source.format + "}",
                         .component = owner.component,
                         .structure = owner.structure,
                         .field = &*field,
                         .use = use,
                         .twoWay = source.twoWay};
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
    case BindingUse::Toggle:
    case BindingUse::Slider:
    case BindingUse::Stepped:
        if (!ControlHolds(use, *field))
        {
            return std::unexpected(BindingError::WrongControl);
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

std::string ShowField(const FieldBinding &binding, const void *owner)
{
    const FieldMeta &field = *binding.field;
    const std::byte *const at = static_cast<const std::byte *>(owner) + field.offset;
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

bool ReadBoolField(const FieldBinding &binding, const void *owner)
{
    return ReadAt<bool>(static_cast<const std::byte *>(owner) + binding.field->offset);
}

std::optional<WidgetValue> ReadControlField(const FieldBinding &binding, const void *owner)
{
    const FieldMeta &field = *binding.field;
    const std::byte *const at = static_cast<const std::byte *>(owner) + field.offset;
    switch (binding.use)
    {
    case BindingUse::Toggle:
        return WidgetValue{ReadAt<bool>(at)};
    case BindingUse::Slider:
        return WidgetValue{static_cast<float>(NumberAt(field.type, at))};
    case BindingUse::Stepped:
        return PositionAt(field, at);
    case BindingUse::Text:
    case BindingUse::Bool:
    case BindingUse::Count_:
        break;
    }
    return std::nullopt;
}

void WriteControlField(const FieldBinding &binding, void *owner, const WidgetValue &value)
{
    const FieldMeta &field = *binding.field;
    std::byte *const at = static_cast<std::byte *>(owner) + field.offset;
    if (const bool *const on = std::get_if<bool>(&value))
    {
        std::memcpy(at, on, sizeof(bool));
    }
    else if (const float *const rests = std::get_if<float>(&value))
    {
        PutNumber(field.type, at, *rests);
    }
    else if (const int32_t *const position = std::get_if<int32_t>(&value))
    {
        PutPosition(field, at, *position);
    }
}

} // namespace Assisi::Mondrian
