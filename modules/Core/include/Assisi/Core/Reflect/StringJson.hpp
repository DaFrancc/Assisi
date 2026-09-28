/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Reflect/StringJson.hpp
/// @brief JSON for the reflected string types, in the shape JsonRead's readers
///        have: an absent key is success, a present-but-wrong value is reported
///        and refused, nothing throws.
///
/// An InternedString is its text and a DisplayedString its written form, so both
/// read as plain strings in a file. A PooledString is `{ "offset", "length" }`
/// and its StringPool one string of every byte: a pool is written by a cook, not
/// by hand, and keeping the numbers means a file reloads to the same handles.

#include <nlohmann/json.hpp>

#include <Assisi/Core/DisplayedString.hpp>
#include <Assisi/Core/InternedString.hpp>
#include <Assisi/Core/StringPool.hpp>

namespace Assisi::Core::Reflect
{

[[nodiscard]] nlohmann::json PooledStringToJson(const PooledString &handle);

/// @brief Reads @p value itself as a PooledString — the element form, for a
/// container. Reports against @p component and @p field when it is not one.
[[nodiscard]] bool PooledStringFromJson(const nlohmann::json &value, const char *component, const char *field,
                                        PooledString &out);

[[nodiscard]] bool ReadInternedString(const nlohmann::json &j, const char *component, const char *field,
                                      InternedString &out);
[[nodiscard]] bool ReadDisplayedString(const nlohmann::json &j, const char *component, const char *field,
                                       DisplayedString &out);
[[nodiscard]] bool ReadPooledString(const nlohmann::json &j, const char *component, const char *field,
                                    PooledString &out);

/// @brief Reads a pool's bytes. One over kMaxPoolBytes is refused, not cut.
[[nodiscard]] bool ReadStringPool(const nlohmann::json &j, const char *component, const char *field,
                                  StringPool &out);

} // namespace Assisi::Core::Reflect
