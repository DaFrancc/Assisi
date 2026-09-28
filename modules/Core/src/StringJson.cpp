/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Core/Reflect/StringJson.hpp>

#include <cstdint>
#include <limits>
#include <string>

#include <Assisi/Core/Reflect/JsonRead.hpp>

namespace Assisi::Core::Reflect
{
namespace
{

constexpr const char *kOffsetKey = "offset";
constexpr const char *kLengthKey = "length";

/// Whether @p object holds @p key as a whole number that fits 32 bits unsigned.
bool IsUInt32(const nlohmann::json &object, const char *key)
{
    const nlohmann::json::const_iterator member = object.find(key);
    return member != object.end() && member->is_number_unsigned() &&
           member->get<std::uint64_t>() <= std::numeric_limits<std::uint32_t>::max();
}

} // namespace

nlohmann::json PooledStringToJson(const PooledString &handle)
{
    nlohmann::json value = nlohmann::json::object();
    value[kOffsetKey]     = handle.offset;
    value[kLengthKey]     = handle.length;
    return value;
}

bool PooledStringFromJson(const nlohmann::json &value, const char *component, const char *field, PooledString &out)
{
    // Both members are required: a handle missing its length would name some
    // other string rather than none.
    if (!value.is_object() || !IsUInt32(value, kOffsetKey) || !IsUInt32(value, kLengthKey))
    {
        ReportBadField(component, field, "an object with a whole-number offset and length", value);
        return false;
    }
    out = PooledString{.offset = value.at(kOffsetKey).get<std::uint32_t>(),
                       .length = value.at(kLengthKey).get<std::uint32_t>()};
    return true;
}

bool ReadInternedString(const nlohmann::json &j, const char *component, const char *field, InternedString &out)
{
    std::string text;
    if (!ReadString(j, component, field, text))
    {
        return false;
    }
    if (j.contains(field))
    {
        out = InternedString{text};
    }
    return true;
}

bool ReadDisplayedString(const nlohmann::json &j, const char *component, const char *field, DisplayedString &out)
{
    std::string source;
    if (!ReadString(j, component, field, source))
    {
        return false;
    }
    if (j.contains(field))
    {
        out = DisplayedString::FromSource(source);
    }
    return true;
}

bool ReadPooledString(const nlohmann::json &j, const char *component, const char *field, PooledString &out)
{
    const nlohmann::json *value = nullptr;
    if (!FindField(j, field, value))
    {
        return true;
    }
    return PooledStringFromJson(*value, component, field, out);
}

bool ReadStringPool(const nlohmann::json &j, const char *component, const char *field, StringPool &out)
{
    std::string bytes;
    if (!ReadString(j, component, field, bytes))
    {
        return false;
    }
    if (!j.contains(field))
    {
        return true;
    }
    if (!out.Assign(bytes))
    {
        ReportBadField(component, field, "a string pool no larger than the pool limit", j.at(field));
        return false;
    }
    return true;
}

} // namespace Assisi::Core::Reflect
