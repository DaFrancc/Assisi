/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file EnumLabels.cpp
/// @brief Implements the enum label registry — see the header.

#include <Assisi/Core/Reflect/EnumLabels.hpp>

#include <Assisi/Core/Logger.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace Assisi::Core::Reflect
{

namespace
{

/// Ordered by enum name only so lookups can take a string_view; std::less<> is
/// what lets them.
using LabelTable = std::map<std::string, std::vector<EnumConstant>, std::less<>>;

/// A function-local static, so a registration running during another unit's
/// static initialization finds the table already built.
LabelTable &Labels()
{
    static LabelTable table;
    return table;
}

bool ValueBefore(const EnumConstant &constant, std::int64_t value)
{
    return constant.value < value;
}

} // namespace

void RegisterEnumLabel(std::string_view enumType, std::int64_t value, std::string_view label)
{
    LabelTable &table = Labels();
    LabelTable::iterator found = table.find(enumType);
    if (found == table.end())
    {
        found = table.emplace(std::string(enumType), std::vector<EnumConstant>{}).first;
    }

    std::vector<EnumConstant> &labels = found->second;
    const std::vector<EnumConstant>::iterator at = std::lower_bound(labels.begin(), labels.end(), value, ValueBefore);
    if (at != labels.end() && at->value == value)
    {
        Log::Warn("Enum {} value {} is already named '{}'; '{}' is ignored.", enumType, value, at->name, label);
        return;
    }
    labels.insert(at, EnumConstant{std::string(label), value});
}

std::span<const EnumConstant> EnumLabelsOf(std::string_view enumType)
{
    const LabelTable &table = Labels();
    const LabelTable::const_iterator found = table.find(enumType);
    if (found == table.end())
    {
        return {};
    }
    return found->second;
}

} // namespace Assisi::Core::Reflect
