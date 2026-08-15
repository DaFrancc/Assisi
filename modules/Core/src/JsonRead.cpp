/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Core/Reflect/JsonRead.hpp>

#include <Assisi/Core/Logger.hpp>

#include <format>

namespace Assisi::Core::Reflect
{
namespace
{

/// What a value actually is, for the "found X" half of the message. `type_name()`
/// is nlohmann's own spelling and is what a human editing the file will recognise.
std::string_view KindOf(const nlohmann::json &value) { return value.type_name(); }

/// The shared shape of every rejection: which component, which field, what was
/// wanted, what was there. Truncated because a mistyped field can be a whole
/// nested object and a log line is not a file viewer.
void Reject(const char *component, const char *field, std::string_view expected,
            const nlohmann::json &found)
{
    constexpr std::size_t kMaxDump = 64;

    std::string dump = found.dump();
    if (dump.size() > kMaxDump)
    {
        dump.resize(kMaxDump);
        dump += "…";
    }

    Log::Error("Reflect: {}.{} expects {}, but the file has {} ({}). The field is not readable, so the "
               "file is refused rather than loaded with a value nobody wrote.",
               component, field, expected, KindOf(found), dump);
}

/// The present-and-testable half every reader shares. Returns nullptr when the
/// key is absent — success, and the caller leaves its output alone.
const nlohmann::json *Present(const nlohmann::json &j, const char *field)
{
    const auto it = j.find(field);
    return it == j.end() ? nullptr : &*it;
}

} // namespace

void ReportBadField(const char *component, const char *field, std::string_view expected,
                    const nlohmann::json &found)
{
    Reject(component, field, expected, found);
}

bool FindField(const nlohmann::json &j, const char *field, const nlohmann::json *&out)
{
    const nlohmann::json *value = Present(j, field);
    if (value == nullptr)
        return false;
    out = value;
    return true;
}

bool ReadFloat(const nlohmann::json &j, const char *component, const char *field, float &out)
{
    const nlohmann::json *value = Present(j, field);
    if (value == nullptr)
        return true;
    if (!value->is_number())
    {
        Reject(component, field, "a number", *value);
        return false;
    }
    out = value->get<float>();
    return true;
}

bool ReadDouble(const nlohmann::json &j, const char *component, const char *field, double &out)
{
    const nlohmann::json *value = Present(j, field);
    if (value == nullptr)
        return true;
    if (!value->is_number())
    {
        Reject(component, field, "a number", *value);
        return false;
    }
    out = value->get<double>();
    return true;
}

bool ReadInt32(const nlohmann::json &j, const char *component, const char *field, int32_t &out)
{
    const nlohmann::json *value = Present(j, field);
    if (value == nullptr)
        return true;
    if (!value->is_number_integer())
    {
        Reject(component, field, "a whole number", *value);
        return false;
    }
    out = value->get<int32_t>();
    return true;
}

bool ReadUInt32(const nlohmann::json &j, const char *component, const char *field, uint32_t &out)
{
    const nlohmann::json *value = Present(j, field);
    if (value == nullptr)
        return true;
    if (!value->is_number_unsigned())
    {
        Reject(component, field, "a whole number that is not negative", *value);
        return false;
    }
    out = value->get<uint32_t>();
    return true;
}

bool ReadInt64(const nlohmann::json &j, const char *component, const char *field, int64_t &out)
{
    const nlohmann::json *value = Present(j, field);
    if (value == nullptr)
        return true;
    if (!value->is_number_integer())
    {
        Reject(component, field, "a whole number", *value);
        return false;
    }
    out = value->get<int64_t>();
    return true;
}

bool ReadUInt64(const nlohmann::json &j, const char *component, const char *field, uint64_t &out)
{
    const nlohmann::json *value = Present(j, field);
    if (value == nullptr)
        return true;
    if (!value->is_number_unsigned())
    {
        Reject(component, field, "a whole number that is not negative", *value);
        return false;
    }
    out = value->get<uint64_t>();
    return true;
}

bool ReadBool(const nlohmann::json &j, const char *component, const char *field, bool &out)
{
    const nlohmann::json *value = Present(j, field);
    if (value == nullptr)
        return true;
    if (!value->is_boolean())
    {
        Reject(component, field, "true or false", *value);
        return false;
    }
    out = value->get<bool>();
    return true;
}

bool ReadString(const nlohmann::json &j, const char *component, const char *field, std::string &out)
{
    const nlohmann::json *value = Present(j, field);
    if (value == nullptr)
        return true;
    if (!value->is_string())
    {
        Reject(component, field, "a string", *value);
        return false;
    }
    out = value->get<std::string>();
    return true;
}

bool ReadFloatArray(const nlohmann::json &j, const char *component, const char *field, std::size_t count,
                    float *out)
{
    const nlohmann::json *value = Present(j, field);
    if (value == nullptr)
        return true;

    if (!value->is_array())
    {
        Reject(component, field, std::format("an array of {} numbers", count), *value);
        return false;
    }
    // Before a single element is read, so a hand-edited "scale": [1, 1] is
    // refused rather than indexed past the end.
    if (value->size() != count)
    {
        Reject(component, field, std::format("an array of {} numbers, not {}", count, value->size()),
               *value);
        return false;
    }

    for (std::size_t i = 0; i < count; ++i)
    {
        const nlohmann::json &element = (*value)[i];
        if (!element.is_number())
        {
            Reject(component, field, std::format("a number in every slot (slot {} is not one)", i), *value);
            return false;
        }
        out[i] = element.get<float>();
    }
    return true;
}

bool FindArray(const nlohmann::json &j, const char *component, const char *field, const nlohmann::json *&out)
{
    const nlohmann::json *value = Present(j, field);
    if (value == nullptr)
        return false;
    if (!value->is_array())
    {
        Reject(component, field, "an array", *value);
        return false;
    }
    out = value;
    return true;
}

} // namespace Assisi::Core::Reflect
