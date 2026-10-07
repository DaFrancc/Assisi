/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file StringTableCompiler.hpp
/// @brief A `.csv` string table becoming a table, and the bytes a package carries.
///
/// The first row is a header whose first column is `key`. Each row after it is
/// one string: its key, then its text. Columns after the second are ignored, so
/// a file can already carry the languages a later build will read.
///
/// Cook-side only. The shipped game parses no source text.

#include <Assisi/Mondrian/Import/Markup.hpp>

#include <Assisi/Mondrian/StringTable.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

namespace Assisi::Mondrian::Import
{

/// @brief What a row with a key and no text means.
enum class EmptyText : uint8_t
{
    Refuse, ///< a string nobody has written yet, which fails the cook
    Allow,  ///< a string that is meant to be empty
    Count_
};

/// @brief The table @p csv describes.
///
/// Refused, with the row's line: no header, a header not starting with `key`,
/// a row with no text column, an empty key, a key written twice, and an empty
/// text when @p empty says so.
[[nodiscard]] std::expected<StringTable, MarkupError> CompileStringTable(std::string_view csv, EmptyText empty);

/// @brief Compiles @p csv and writes the cooked blob.
[[nodiscard]] std::expected<std::vector<std::byte>, MarkupError> CompileStringTableText(std::string_view csv,
                                                                                        EmptyText empty);

} // namespace Assisi::Mondrian::Import
