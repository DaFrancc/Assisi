/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file BitmaskJson.hpp
/// @brief How a Core::Bitmask field is written to a file and read back: by the
///        names of the enumerators it holds, not by its integer.
///
/// `["World", "Character"]` says which channels a mask holds and survives an
/// enumerator moving to another bit; `3` says neither. Every enumerator is
/// written as `"All"`, so a default mask stays one word however many there are.
///
/// A set bit with no name — a value of the enum nobody named — is written as its
/// bit number, so nothing set is ever lost on a save. A name comes from the
/// enum's own enumerators or, for the values it leaves unnamed, from EnumLabels.

#include <Assisi/Core/Reflect/JsonRead.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace Assisi::Core::Reflect
{

/// @brief Everything about a bitmask's enum that naming its bits needs.
struct BitmaskNames
{
    std::span<const EnumName> names; ///< The enum's own enumerators.
    std::string_view enumType;       ///< Its qualified name, for EnumLabels.
    std::uint32_t all = 0;           ///< Every enumerator's bit: what "All" means.
};

/// @brief @p bits as a file stores it: "All", or the names of the bits set.
[[nodiscard]] nlohmann::json BitmaskToJson(std::uint32_t bits, const BitmaskNames &names);

/// @brief Reads a bitmask field written as "All" or as a list of names and bit
/// numbers.
///
/// A name the enum does not have, a bit number outside the mask, or anything
/// else is refused with the names it could have been. An absent field leaves
/// @p out alone, as every reader here does.
[[nodiscard]] bool ReadBitmask(const nlohmann::json &j, const char *component, const char *field,
                               const BitmaskNames &names, std::uint32_t &out);

} // namespace Assisi::Core::Reflect
