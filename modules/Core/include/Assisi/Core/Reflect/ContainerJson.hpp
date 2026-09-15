/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Reflect/ContainerJson.hpp
/// @brief JSON for reflected containers, written as templates over the concrete
///        element type rather than as generated per-type expressions.
///
/// The binary codec reaches a container through type-erased ops because it walks
/// a FieldMeta. Generated JSON code has the opposite problem and the opposite
/// luxury: it is emitted per field, so it can name `decltype(Comp::member)` and
/// let the compiler do the walking. That keeps reflectgen's table free of a JSON
/// expression per element type, and keeps the two directions honest by giving
/// each a single definition.
///
/// A vector is a JSON array. A map is a JSON **object**, so its keys are text —
/// which is why a key must be an integer or one of the inline strings, and why a
/// float or an enum cannot be one.
///
/// Failure follows JsonRead's contract exactly: an absent key is success and
/// leaves the destination alone, a present-but-wrong value is reported against
/// its component and field and returns false, and nothing throws.

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include <Assisi/Core/Reflect/JsonRead.hpp>
#include <Assisi/Core/ShortString.hpp>
#include <Assisi/Core/TrivialString.hpp>

namespace Assisi::Core::Reflect
{

/// @brief Whether @p T is one of the inline fixed-capacity strings.
template <typename T> struct IsTrivialString : std::false_type
{
};

template <std::size_t Capacity> struct IsTrivialString<TrivialString<Capacity>> : std::true_type
{
};

/// @brief Whether @p T is one of the container shapes this file handles, so an
///        element can tell "recurse" from "this is a value".
template <typename T> struct IsReflectedContainer : std::false_type
{
};

template <typename T, typename A> struct IsReflectedContainer<std::vector<T, A>> : std::true_type
{
};

template <typename K, typename V, typename C, typename A>
struct IsReflectedContainer<std::map<K, V, C, A>> : std::true_type
{
};

template <typename K, typename V, typename H, typename E, typename A>
struct IsReflectedContainer<std::unordered_map<K, V, H, E, A>> : std::true_type
{
};

// Declared before the element overloads, which recurse into them for a nested
// element and would otherwise not see them.
template <typename T, typename A> nlohmann::json ContainerToJson(const std::vector<T, A> &values);
template <typename K, typename V, typename C, typename A>
nlohmann::json ContainerToJson(const std::map<K, V, C, A> &entries);
template <typename K, typename V, typename H, typename E, typename A>
nlohmann::json ContainerToJson(const std::unordered_map<K, V, H, E, A> &entries);

template <typename T, typename A>
bool ContainerFromJson(const nlohmann::json &value, const char *component, const char *field,
                       std::vector<T, A> &out);
template <typename K, typename V, typename C, typename A>
bool ContainerFromJson(const nlohmann::json &value, const char *component, const char *field,
                       std::map<K, V, C, A> &out);
template <typename K, typename V, typename H, typename E, typename A>
bool ContainerFromJson(const nlohmann::json &value, const char *component, const char *field,
                       std::unordered_map<K, V, H, E, A> &out);

// ── Element values ────────────────────────────────────────────────────────────
// One overload set, used at every depth. A nested container resolves to the
// container overloads further down, which is what makes nesting cost nothing
// here beyond the recursion the compiler writes.

template <typename T> nlohmann::json ElementToJson(const T &value);
template <typename T>
bool ElementFromJson(const nlohmann::json &value, const char *component, const char *field, T &out);

namespace Detail
{

/// A whole number that fits @p T. Mirrors JsonRead's narrow reader: `get<T>()`
/// would keep the low bits of an out-of-range value and say nothing.
template <typename T>
bool ReadIntegerElement(const nlohmann::json &value, const char *component, const char *field, T &out)
{
    constexpr bool isSigned = std::is_signed_v<T>;
    if (isSigned ? !value.is_number_integer() : !value.is_number_unsigned())
    {
        ReportBadField(component, field,
                       isSigned ? "whole numbers" : "whole numbers that are not negative", value);
        return false;
    }

    const std::int64_t raw = value.get<std::int64_t>();
    if (raw < static_cast<std::int64_t>(std::numeric_limits<T>::min())
        || raw > static_cast<std::int64_t>(std::numeric_limits<T>::max()))
    {
        ReportBadField(component, field, "whole numbers small enough for this element", value);
        return false;
    }
    out = static_cast<T>(raw);
    return true;
}

} // namespace Detail

template <typename T> nlohmann::json ElementToJson(const T &value)
{
    if constexpr (std::is_enum_v<T>)
    {
        // The underlying integer, matching how a scalar enum field serializes —
        // so an enum inside a container reads the same as one beside it.
        return static_cast<std::int64_t>(value);
    }
    else if constexpr (IsTrivialString<T>::value)
    {
        return std::string(value.View());
    }
    else if constexpr (IsReflectedContainer<T>::value)
    {
        return ContainerToJson(value);
    }
    else
    {
        return value;
    }
}

template <typename T>
bool ElementFromJson(const nlohmann::json &value, const char *component, const char *field, T &out)
{
    if constexpr (std::is_enum_v<T>)
    {
        std::int64_t raw = 0;
        if (!Detail::ReadIntegerElement(value, component, field, raw))
        {
            return false;
        }
        out = static_cast<T>(raw);
        return true;
    }
    else if constexpr (IsTrivialString<T>::value)
    {
        if (!value.is_string())
        {
            ReportBadField(component, field, "strings", value);
            return false;
        }
        // Refused rather than truncated: a silently shortened key or label is a
        // value the file does not contain.
        if (!out.Assign(value.get<std::string>()))
        {
            ReportBadField(component, field, "strings short enough for this element", value);
            return false;
        }
        return true;
    }
    else if constexpr (std::is_same_v<T, bool>)
    {
        if (!value.is_boolean())
        {
            ReportBadField(component, field, "booleans", value);
            return false;
        }
        out = value.get<bool>();
        return true;
    }
    else if constexpr (std::is_floating_point_v<T>)
    {
        if (!value.is_number())
        {
            ReportBadField(component, field, "numbers", value);
            return false;
        }
        out = value.get<T>();
        return true;
    }
    else if constexpr (std::is_integral_v<T>)
    {
        return Detail::ReadIntegerElement(value, component, field, out);
    }
    else
    {
        // A nested container: the container overloads below take it.
        return ContainerFromJson(value, component, field, out);
    }
}

// ── Map keys ──────────────────────────────────────────────────────────────────
// A JSON object's keys are text, so an integer key round-trips through its
// decimal spelling and must consume all of it — "12abc" is not 12.

template <typename K> std::string KeyToText(const K &key)
{
    if constexpr (IsTrivialString<K>::value)
    {
        return std::string(key.View());
    }
    else
    {
        return std::to_string(key);
    }
}

template <typename K>
bool KeyFromText(std::string_view text, const char *component, const char *field, K &out)
{
    if constexpr (IsTrivialString<K>::value)
    {
        if (!out.Assign(text))
        {
            ReportBadField(component, field, "keys short enough for this map", nlohmann::json(text));
            return false;
        }
        return true;
    }
    else
    {
        const char *begin = text.data();
        const char *end   = text.data() + text.size();

        K value{};
        const std::from_chars_result parsed = std::from_chars(begin, end, value);
        if (parsed.ec != std::errc{} || parsed.ptr != end)
        {
            ReportBadField(component, field, "keys that are whole numbers", nlohmann::json(text));
            return false;
        }
        out = value;
        return true;
    }
}

// ── Containers ────────────────────────────────────────────────────────────────

template <typename T, typename A> nlohmann::json ContainerToJson(const std::vector<T, A> &values)
{
    nlohmann::json array = nlohmann::json::array();
    for (const T &value : values)
    {
        array.push_back(ElementToJson(value));
    }
    return array;
}

namespace Detail
{

/// Both map flavours write the same object. Keys are emitted in sorted order for
/// the same reason the binary codec sorts them: an unordered_map's own order
/// follows memory layout, so the same contents would produce a different file
/// between two runs and every save would look like an edit.
template <typename M> nlohmann::json MapToJson(const M &entries)
{
    std::map<std::string, const typename M::mapped_type *> ordered;
    for (const typename M::value_type &entry : entries)
    {
        ordered.emplace(KeyToText(entry.first), &entry.second);
    }

    nlohmann::json object = nlohmann::json::object();
    for (const auto &[text, value] : ordered)
    {
        object[text] = ElementToJson(*value);
    }
    return object;
}

template <typename M>
bool MapFromJson(const nlohmann::json &value, const char *component, const char *field, M &out)
{
    if (!value.is_object())
    {
        ReportBadField(component, field, "an object", value);
        return false;
    }

    out.clear();
    for (const auto &entry : value.items())
    {
        typename M::key_type key{};
        if (!KeyFromText(entry.key(), component, field, key))
        {
            return false;
        }

        typename M::mapped_type mapped{};
        if (!ElementFromJson(entry.value(), component, field, mapped))
        {
            return false;
        }
        out.emplace(std::move(key), std::move(mapped));
    }
    return true;
}

} // namespace Detail

template <typename K, typename V, typename C, typename A>
nlohmann::json ContainerToJson(const std::map<K, V, C, A> &entries)
{
    return Detail::MapToJson(entries);
}

template <typename K, typename V, typename H, typename E, typename A>
nlohmann::json ContainerToJson(const std::unordered_map<K, V, H, E, A> &entries)
{
    return Detail::MapToJson(entries);
}

template <typename T, typename A>
bool ContainerFromJson(const nlohmann::json &value, const char *component, const char *field,
                       std::vector<T, A> &out)
{
    if (!value.is_array())
    {
        ReportBadField(component, field, "an array", value);
        return false;
    }

    out.clear();
    out.reserve(value.size());
    for (const nlohmann::json &element : value)
    {
        T decoded{};
        if (!ElementFromJson(element, component, field, decoded))
        {
            return false;
        }
        out.push_back(std::move(decoded));
    }
    return true;
}

template <typename K, typename V, typename C, typename A>
bool ContainerFromJson(const nlohmann::json &value, const char *component, const char *field,
                       std::map<K, V, C, A> &out)
{
    return Detail::MapFromJson(value, component, field, out);
}

template <typename K, typename V, typename H, typename E, typename A>
bool ContainerFromJson(const nlohmann::json &value, const char *component, const char *field,
                       std::unordered_map<K, V, H, E, A> &out)
{
    return Detail::MapFromJson(value, component, field, out);
}

/// @brief Reads a container field out of a component's JSON object.
///
/// The entry point generated deserializers call. An absent key leaves @p out
/// untouched and succeeds, which is what lets a component gain a container field
/// without refusing every file written before it.
template <typename C>
bool ReadContainer(const nlohmann::json &j, const char *component, const char *field, C &out)
{
    const nlohmann::json *value = nullptr;
    if (!FindField(j, field, value))
    {
        return true;
    }
    return ContainerFromJson(*value, component, field, out);
}

} // namespace Assisi::Core::Reflect
