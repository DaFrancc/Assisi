/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Import/Csv.hpp>

#include <doctest/doctest.h>

#include <expected>
#include <string>
#include <string_view>
#include <vector>

using namespace Assisi::Mondrian::Import;

namespace
{

using Fields = std::vector<std::string>;

std::string Why(const std::expected<std::vector<CsvRow>, MarkupError> &read)
{
    return read.has_value() ? std::string{} : read.error().message;
}

std::vector<CsvRow> Read(std::string_view text)
{
    std::expected<std::vector<CsvRow>, MarkupError> read = ReadCsv(text);
    REQUIRE_MESSAGE(read.has_value(), Why(read));
    return *read;
}

MarkupError Refused(std::string_view text)
{
    const std::expected<std::vector<CsvRow>, MarkupError> read = ReadCsv(text);
    REQUIRE_FALSE(read.has_value());
    return read.error();
}

} // namespace

TEST_CASE("Csv: fields split on commas and rows on line breaks")
{
    const std::vector<CsvRow> rows = Read("key,en\ntitle,Paused\n");
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].fields == Fields{"key", "en"});
    CHECK(rows[1].fields == Fields{"title", "Paused"});
    CHECK(rows[0].line == 1);
    CHECK(rows[1].line == 2);
}

TEST_CASE("Csv: a quoted field holds commas, line breaks and doubled quotes")
{
    const std::vector<CsvRow> rows = Read("a,\"one, two\"\n"
                                          "b,\"first\nsecond\"\n"
                                          "c,\"say \"\"hi\"\"\"\n"
                                          "d,after\n");
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].fields == Fields{"a", "one, two"});
    CHECK(rows[1].fields == Fields{"b", "first\nsecond"});
    CHECK(rows[2].fields == Fields{"c", "say \"hi\""});
    // The line break inside the quotes moved the count on.
    CHECK(rows[3].line == 5);
}

TEST_CASE("Csv: what a spreadsheet saves reads the same as what a person types")
{
    // CRLF line ends, a byte order mark, and a blank line at the end.
    const std::vector<CsvRow> rows = Read("\xEF\xBB\xBFkey,en\r\ntitle,Paused\r\n\r\n");
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].fields == Fields{"key", "en"});
    CHECK(rows[1].fields == Fields{"title", "Paused"});
}

TEST_CASE("Csv: empty fields are kept, in the middle and at the end")
{
    const std::vector<CsvRow> rows = Read("a,,c\nd,\n");
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].fields == Fields{"a", "", "c"});
    CHECK(rows[1].fields == Fields{"d", ""});
}

TEST_CASE("Csv: a file with no final line break still ends its last row")
{
    const std::vector<CsvRow> rows = Read("a,b\nc,d");
    REQUIRE(rows.size() == 2);
    CHECK(rows[1].fields == Fields{"c", "d"});
}

TEST_CASE("Csv: a quote left open is refused where it was opened")
{
    const MarkupError error = Refused("a,b\nc,\"never closed\n");
    CHECK(error.line == 2);
    CHECK(error.column == 3);
}

TEST_CASE("Csv: a quote closed partway through a field is refused where it closed")
{
    const MarkupError error = Refused("a,\"closed\"early\n");
    CHECK(error.line == 1);
    CHECK(error.column == 10);
}
