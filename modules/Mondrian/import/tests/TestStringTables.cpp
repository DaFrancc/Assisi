/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "ScreenTesting.hpp"

#include <Assisi/Mondrian/Import/StringTableCompiler.hpp>
#include <Assisi/Mondrian/Import/TextRules.hpp>

#include <Assisi/Mondrian/StringTable.hpp>

#include <Assisi/Core/BitStream.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace Assisi::Mondrian;
using namespace Assisi::Mondrian::Import;
using ScreenTesting::Files;
using ScreenTesting::Serving;
using ScreenTesting::Why;

namespace
{

StringTable Compiled(std::string_view csv, EmptyText empty = EmptyText::Refuse)
{
    std::expected<StringTable, MarkupError> table = CompileStringTable(csv, empty);
    REQUIRE_MESSAGE(table.has_value(), Why(table));
    return *table;
}

MarkupError Refused(std::string_view csv, EmptyText empty = EmptyText::Refuse)
{
    const std::expected<StringTable, MarkupError> table = CompileStringTable(csv, empty);
    REQUIRE_FALSE(table.has_value());
    return table.error();
}

/// A settings document listing @p tables, with the two policies as given.
std::string Settings(std::string_view tables, bool requireKeys, bool allowEmpty)
{
    return std::string{"{\"version\": 1, \"type\": \"UiConfig\", \"stringTables\": ["} + std::string{tables} +
           "], \"requireStringKeys\": " + (requireKeys ? "true" : "false") +
           ", \"allowEmptyStrings\": " + (allowEmpty ? "true" : "false") + "}";
}

} // namespace

TEST_CASE("StringTable: a key's text is the column after it")
{
    const StringTable table = Compiled("key,en\ntitle,Paused\nresume,Resume\n");
    CHECK(table.entries.size() == 2);
    CHECK(table.entries.at("title") == "Paused");
    CHECK(table.entries.at("resume") == "Resume");
}

TEST_CASE("StringTable: columns after the text are for languages a later build reads")
{
    const StringTable table = Compiled("key,en,fr\ntitle,Paused,En pause\n");
    CHECK(table.entries.at("title") == "Paused");
}

TEST_CASE("StringTable: a header must start with key and have a text column")
{
    CHECK(Refused("name,en\ntitle,Paused\n").line == 1);
    CHECK(Refused("key\ntitle\n").line == 1);
    CHECK_FALSE(CompileStringTable("", EmptyText::Refuse).has_value());
}

TEST_CASE("StringTable: a row is refused at its line for what is wrong with it")
{
    CHECK(Refused("key,en\ntitle,Paused\nalone\n").line == 3);
    CHECK(Refused("key,en\n,Paused\n").line == 2);
    CHECK(Refused("key,en\ntitle,Paused\ntitle,Again\n").line == 3);
}

TEST_CASE("StringTable: an empty text is refused unless the project allows it")
{
    CHECK(Refused("key,en\ntitle,\n").line == 2);
    CHECK(Compiled("key,en\ntitle,\n", EmptyText::Allow).entries.at("title").empty());
}

TEST_CASE("StringTable: the cooked table reads back as it was written")
{
    const StringTable table = Compiled("key,en\ntitle,Paused\nquote,\"a, \"\"b\"\"\"\n");
    Assisi::Core::BitWriter writer;
    WriteCookedStringTable(writer, table);
    const std::span<const std::byte> bytes = writer.Data();

    const std::expected<StringTable, CookedStringTableError> read = ReadCookedStringTable(bytes);
    REQUIRE(read.has_value());
    CHECK(*read == table);

    // Cut short, it is refused rather than read as far as it goes.
    const std::expected<StringTable, CookedStringTableError> cut = ReadCookedStringTable(bytes.first(bytes.size() - 1));
    CHECK_FALSE(cut.has_value());
}

TEST_CASE("StringTable: a table is named after its file")
{
    CHECK(TableName("ui/pause.csv") == "pause");
    CHECK(TableName("pause.csv") == "pause");
    CHECK(TableName("text/strings.fr.csv") == "strings.fr");
}

TEST_CASE("StringTable: a key names its table before the separator")
{
    const std::optional<TableKey> split = SplitKey("pause:menu.title");
    REQUIRE(split.has_value());
    CHECK(split->table == "pause");
    CHECK(split->key == "menu.title");

    CHECK_FALSE(SplitKey("title").has_value());
    CHECK_FALSE(SplitKey(":title").has_value());
    CHECK_FALSE(SplitKey("pause:").has_value());
}

TEST_CASE("TextRules: a project with no UI settings has no tables and allows literal text")
{
    const std::expected<TextRules, MarkupError> rules = LoadTextRules(Serving({}));
    REQUIRE_MESSAGE(rules.has_value(), Why(rules));
    CHECK(rules->tables.byName.empty());
    CHECK(rules->literals == LiteralText::Allowed);
}

TEST_CASE("TextRules: every listed table is read under its name, with the project's policies")
{
    const Files files{{std::string{kUiConfigPath}, Settings("\"ui/pause.csv\", \"text/quests.csv\"", true, true)},
                      {"ui/pause.csv", "key,en\ntitle,Paused\n"},
                      {"text/quests.csv", "key,en\nintro,\n"}};
    const std::expected<TextRules, MarkupError> rules = LoadTextRules(Serving(files));
    REQUIRE_MESSAGE(rules.has_value(), Why(rules));
    CHECK(rules->literals == LiteralText::RequiresKey);
    REQUIRE(rules->tables.Find("pause", "title") != nullptr);
    CHECK(*rules->tables.Find("pause", "title") == "Paused");
    // Empty, which allowEmptyStrings let through.
    REQUIRE(rules->tables.Find("quests", "intro") != nullptr);
    CHECK(rules->tables.Find("quests", "intro")->empty());
}

TEST_CASE("TextRules: the empty-text policy reaches the tables")
{
    const Files files{{std::string{kUiConfigPath}, Settings("\"ui/pause.csv\"", false, false)},
                      {"ui/pause.csv", "key,en\ntitle,\n"}};
    const std::expected<TextRules, MarkupError> rules = LoadTextRules(Serving(files));
    REQUIRE_FALSE(rules.has_value());
    CHECK(rules.error().file == "ui/pause.csv");
    CHECK(rules.error().line == 2);
}

TEST_CASE("TextRules: a listed table that is not there is named")
{
    const Files files{{std::string{kUiConfigPath}, Settings("\"ui/pause.csv\"", false, false)}};
    const std::expected<TextRules, MarkupError> rules = LoadTextRules(Serving(files));
    REQUIRE_FALSE(rules.has_value());
    CHECK(rules.error().message.find("ui/pause.csv") != std::string::npos);
}

TEST_CASE("TextRules: two tables with one name are refused, naming both")
{
    const Files files{{std::string{kUiConfigPath}, Settings("\"ui/pause.csv\", \"old/pause.csv\"", false, false)},
                      {"ui/pause.csv", "key,en\ntitle,Paused\n"},
                      {"old/pause.csv", "key,en\ntitle,Paused\n"}};
    const std::expected<TextRules, MarkupError> rules = LoadTextRules(Serving(files));
    REQUIRE_FALSE(rules.has_value());
    CHECK(rules.error().message.find("ui/pause.csv") != std::string::npos);
    CHECK(rules.error().message.find("old/pause.csv") != std::string::npos);
}
