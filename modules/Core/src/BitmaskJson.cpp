/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file BitmaskJson.cpp
/// @brief Implements bitmask fields' file form — see the header.

#include <Assisi/Core/Reflect/BitmaskJson.hpp>

#include <Assisi/Core/Bitmask.hpp>
#include <Assisi/Core/Reflect/EnumLabels.hpp>

#include <optional>
#include <string>

namespace Assisi::Core::Reflect
{

namespace
{

/// What a file writes for a mask holding every enumerator.
constexpr std::string_view kAll = "All";

/// The name of bit @p bit, from the enum or from EnumLabels, if it has one.
std::optional<std::string_view> NameOf(std::uint32_t bit, const BitmaskNames &names)
{
    for (const EnumName &entry : names.names)
    {
        if (entry.value == static_cast<std::int64_t>(bit))
        {
            return entry.name;
        }
    }
    for (const EnumConstant &label : EnumLabelsOf(names.enumType))
    {
        if (label.value == static_cast<std::int64_t>(bit))
        {
            return std::string_view(label.name);
        }
    }
    return std::nullopt;
}

/// The bit @p name stands for, from the enum or from EnumLabels.
std::optional<std::uint32_t> BitNamed(std::string_view name, const BitmaskNames &names)
{
    for (const EnumName &entry : names.names)
    {
        if (entry.name == name)
        {
            return static_cast<std::uint32_t>(entry.value);
        }
    }
    for (const EnumConstant &label : EnumLabelsOf(names.enumType))
    {
        if (label.name == name)
        {
            return static_cast<std::uint32_t>(label.value);
        }
    }
    return std::nullopt;
}

/// Every name a mask of this enum may hold, for a refusal to list.
std::string AllowedNames(const BitmaskNames &names)
{
    std::string allowed = "\"All\", or a list of: ";
    bool first = true;
    for (const EnumName &entry : names.names)
    {
        allowed += first ? "" : ", ";
        allowed += entry.name;
        first = false;
    }
    for (const EnumConstant &label : EnumLabelsOf(names.enumType))
    {
        allowed += first ? "" : ", ";
        allowed += label.name;
        first = false;
    }
    return allowed;
}

/// Reads one element of a mask's list into @p out. False when it names no bit.
bool ReadElement(const nlohmann::json &element, const BitmaskNames &names, std::uint32_t &out)
{
    if (element.is_string())
    {
        const std::optional<std::uint32_t> bit = BitNamed(element.get<std::string>(), names);
        if (!bit.has_value())
        {
            return false;
        }
        out |= 1u << *bit;
        return true;
    }
    if (element.is_number_unsigned() && element.get<std::uint64_t>() < kBitmaskBits)
    {
        out |= 1u << element.get<std::uint32_t>();
        return true;
    }
    return false;
}

} // namespace

nlohmann::json BitmaskToJson(std::uint32_t bits, const BitmaskNames &names)
{
    if (bits == names.all)
    {
        return std::string(kAll);
    }
    nlohmann::json list = nlohmann::json::array();
    for (std::uint32_t bit = 0; bit < kBitmaskBits; ++bit)
    {
        if ((bits & (1u << bit)) == 0u)
        {
            continue;
        }
        const std::optional<std::string_view> name = NameOf(bit, names);
        if (name.has_value())
        {
            list.push_back(std::string(*name));
        }
        else
        {
            list.push_back(bit);
        }
    }
    return list;
}

bool ReadBitmask(const nlohmann::json &j, const char *component, const char *field, const BitmaskNames &names,
                 std::uint32_t &out)
{
    const nlohmann::json *value = nullptr;
    if (!FindField(j, field, value))
    {
        return true;
    }

    if (value->is_string() && value->get<std::string>() == kAll)
    {
        out = names.all;
        return true;
    }
    if (!value->is_array())
    {
        ReportBadField(component, field, AllowedNames(names), *value);
        return false;
    }

    std::uint32_t bits = 0;
    for (const nlohmann::json &element : *value)
    {
        if (!ReadElement(element, names, bits))
        {
            ReportBadField(component, field, AllowedNames(names), element);
            return false;
        }
    }
    out = bits;
    return true;
}

} // namespace Assisi::Core::Reflect
