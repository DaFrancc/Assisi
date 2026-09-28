/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Mondrian/Import/StringTableCompiler.hpp>

#include <Assisi/Mondrian/Import/Csv.hpp>

#include <Assisi/Core/BitStream.hpp>

#include <span>
#include <string>
#include <utility>

namespace Assisi::Mondrian::Import
{
namespace
{

/// What the header's first column says, so the file reads as a table of keys.
constexpr std::string_view kKeyHeader = "key";

/// A header or a row needs a key and one text column.
constexpr std::size_t kMinColumns = 2;

/// Where a row keeps its key and its text.
constexpr std::size_t kKeyColumn = 0;
constexpr std::size_t kTextColumn = 1;

MarkupError AtRow(const CsvRow &row, std::string message)
{
    return MarkupError{.message = std::move(message), .line = row.line, .column = 1};
}

} // namespace

std::expected<StringTable, MarkupError> CompileStringTable(std::string_view csv, EmptyText empty)
{
    const std::expected<std::vector<CsvRow>, MarkupError> rows = ReadCsv(csv);
    if (!rows)
    {
        return std::unexpected(rows.error());
    }
    if (rows->empty())
    {
        return std::unexpected(MarkupError{.message = "the table is empty. Its first row is a header: key, then a "
                                                      "column for the text, such as key,en."});
    }

    const CsvRow &header = rows->front();
    if (header.fields.size() < kMinColumns || header.fields[kKeyColumn] != kKeyHeader)
    {
        return std::unexpected(AtRow(header, "the first row is a header: key, then a column for the text, such as "
                                             "key,en. A spreadsheet has to save it as CSV UTF-8, with commas "
                                             "between columns."));
    }

    StringTable table;
    for (std::size_t index = 1; index < rows->size(); ++index)
    {
        const CsvRow &row = (*rows)[index];
        if (row.fields.size() < kMinColumns)
        {
            return std::unexpected(AtRow(row, "this row has a key and no text column. Write key,text."));
        }
        const std::string &key = row.fields[kKeyColumn];
        const std::string &text = row.fields[kTextColumn];
        if (key.empty())
        {
            return std::unexpected(AtRow(row, "this row has no key, so nothing can name its text."));
        }
        if (text.empty() && empty == EmptyText::Refuse)
        {
            return std::unexpected(AtRow(row, "'" + key +
                                                  "' has no text. Write it, or set allowEmptyStrings in "
                                                  "config/ui.json if it is meant to be empty."));
        }
        if (key.size() > kMaxStringTableEntryBytes || text.size() > kMaxStringTableEntryBytes)
        {
            return std::unexpected(AtRow(row, "'" + key + "' is longer than " +
                                                  std::to_string(kMaxStringTableEntryBytes) +
                                                  " bytes. Split it into several strings."));
        }
        if (!table.entries.emplace(key, text).second)
        {
            return std::unexpected(AtRow(row, "'" + key + "' is already a key in this table."));
        }
    }
    return table;
}

std::expected<std::vector<std::byte>, MarkupError> CompileStringTableText(std::string_view csv, EmptyText empty)
{
    const std::expected<StringTable, MarkupError> table = CompileStringTable(csv, empty);
    if (!table)
    {
        return std::unexpected(table.error());
    }
    Core::BitWriter writer;
    WriteCookedStringTable(writer, *table);
    const std::span<const std::byte> bytes = writer.Data();
    return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

} // namespace Assisi::Mondrian::Import
