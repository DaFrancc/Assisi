/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Csv.hpp
/// @brief Comma-separated rows, as a spreadsheet saves them.
///
/// Fields separated by commas, rows by LF or CRLF. A field in double quotes may
/// hold commas, line breaks and `""` for one quote. A UTF-8 byte order mark at
/// the start is skipped, because a spreadsheet saving "CSV UTF-8" writes one.
/// Nothing else is read differently: no other separators, no other encodings.
///
/// Cook-side only. The shipped game parses no source text.

#include <Assisi/Mondrian/Import/Markup.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::Mondrian::Import
{

/// @brief One row, and the line it starts on, for an error about it to point at.
struct CsvRow
{
    std::vector<std::string> fields;
    uint32_t line = 1;
};

/// @brief The rows of @p text, in order.
///
/// A row with nothing on it is dropped, which is what a spreadsheet leaves at
/// the end of a file. An unclosed quote, or a quote closed partway through a
/// field, is an error with the line and column of the quote.
[[nodiscard]] std::expected<std::vector<CsvRow>, MarkupError> ReadCsv(std::string_view text);

} // namespace Assisi::Mondrian::Import
