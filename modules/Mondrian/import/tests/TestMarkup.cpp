/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Import/Markup.hpp>

#include <doctest/doctest.h>

#include <expected>
#include <string>
#include <string_view>

using namespace Assisi::Mondrian::Import;

namespace
{

/// The error from parsing @p text, which the case expects to fail.
MarkupError Refused(std::string_view text)
{
    const std::expected<MarkupElement, MarkupError> parsed = ParseMarkup(text);
    REQUIRE_FALSE(parsed.has_value());
    return parsed.error();
}

/// The reason @p parsed failed, or empty. Its own function because `*` binds
/// tighter than `?:` inside doctest's message macro, so a ternary written at
/// the call would be parsed as part of the stream expression.
template <typename T> std::string Why(const std::expected<T, MarkupError> &parsed)
{
    return parsed.has_value() ? std::string{} : parsed.error().message;
}

MarkupElement Parsed(std::string_view text)
{
    std::expected<MarkupElement, MarkupError> parsed = ParseMarkup(text);
    REQUIRE_MESSAGE(parsed.has_value(), Why(parsed));
    return *parsed;
}

} // namespace

TEST_CASE("Markup: elements, attributes, text and nesting reach the tree")
{
    const MarkupElement root = Parsed("<screen name=\"Pause\" pause='true'>\n"
                                      "  <column gap=\"12\">\n"
                                      "    <text>Paused</text>\n"
                                      "    <button on_click=\"hide\">Resume</button>\n"
                                      "  </column>\n"
                                      "</screen>\n");

    CHECK(root.name == "screen");
    REQUIRE(root.Find("name") != nullptr);
    CHECK(root.Find("name")->value == "Pause");
    // Either quote, because a value holding one has to be writable with the other.
    REQUIRE(root.Find("pause") != nullptr);
    CHECK(root.Find("pause")->value == "true");
    CHECK(root.Find("absent") == nullptr);

    REQUIRE(root.children.size() == 1);
    const MarkupElement &column = root.children[0];
    CHECK(column.name == "column");
    REQUIRE(column.children.size() == 2);
    CHECK(column.children[0].name == "text");
    CHECK(column.children[0].text == "Paused");
    CHECK(column.children[1].text == "Resume");
    CHECK(column.children[1].Find("on_click")->value == "hide");
}

TEST_CASE("Markup: an element's own line and column are where its name starts")
{
    // What every error a person reads is built from, so it is worth pinning.
    const MarkupElement root = Parsed("<screen>\n"
                                      "  <column>\n"
                                      "    <text>Paused</text>\n"
                                      "  </column>\n"
                                      "</screen>\n");

    CHECK(root.line == 1);
    CHECK(root.column == 2);
    CHECK(root.children[0].line == 2);
    CHECK(root.children[0].column == 4);

    const MarkupAttribute *gap = Parsed("<screen>\n  <column gap=\"12\" />\n</screen>\n").children[0].Find("gap");
    REQUIRE(gap != nullptr);
    CHECK(gap->line == 2);
    CHECK(gap->column == 11);
}

TEST_CASE("Markup: a self-closing element has no children and no text")
{
    const MarkupElement root = Parsed("<screen><column gap=\"12\" /></screen>");

    REQUIRE(root.children.size() == 1);
    CHECK(root.children[0].children.empty());
    CHECK(root.children[0].text.empty());
    CHECK(root.children[0].Find("gap")->value == "12");
}

TEST_CASE("Markup: comments are skipped wherever they may sit")
{
    const MarkupElement root = Parsed("<!-- before -->\n"
                                      "<screen>\n"
                                      "  <!-- inside -->\n"
                                      "  <text>Paused<!-- after the words --></text>\n"
                                      "</screen>\n"
                                      "<!-- after -->\n");

    REQUIRE(root.children.size() == 1);
    CHECK(root.children[0].text == "Paused");
}

TEST_CASE("Markup: the five entities are the five it knows")
{
    const MarkupElement root = Parsed("<text title=\"&quot;a&apos;b&quot;\">&lt;tag&gt; &amp; more</text>");

    CHECK(root.text == "<tag> & more");
    CHECK(root.Find("title")->value == "\"a'b\"");

    const MarkupError unknown = Refused("<text>&nbsp;</text>");
    CHECK(unknown.message.find("&nbsp;") != std::string::npos);

    const MarkupError unclosed = Refused("<text>&amp</text>");
    CHECK(unclosed.message.find("never closed") != std::string::npos);
}

TEST_CASE("Markup: text keeps what is between the words and drops what is around them")
{
    // A label is written as text, and what somebody typed between two words is
    // what they meant.
    CHECK(Parsed("<text>  two   words  </text>").text == "two   words");
    CHECK(Parsed("<text>\n  Paused\n</text>").text == "Paused");
    CHECK(Parsed("<text> </text>").text.empty());
}

TEST_CASE("Markup: a structural mistake is refused at the line and column it is on")
{
    // Every case but the first puts its fault on line 3, so a parser that
    // reported line 1 for everything could not pass by accident.
    SUBCASE("an unclosed element")
    {
        // Reported where the wrong tag was found, naming where the open one
        // started — either end can be the mistake, and only both together say
        // which.
        const MarkupError error = Refused("<screen>\n"
                                          "  <column>\n"
                                          "    <text>Paused\n"
                                          "  </column>\n"
                                          "</screen>\n");
        CHECK(error.line == 4);
        CHECK(error.message.find("never closed") != std::string::npos);
        CHECK(error.message.find("line 3") != std::string::npos);
    }

    SUBCASE("an element left open at the end of the file")
    {
        const MarkupError error = Refused("<screen>\n"
                                          "  <column>\n"
                                          "    <text>Paused</text>\n");
        CHECK(error.line == 2);
        CHECK(error.message.find("never closed") != std::string::npos);
    }

    SUBCASE("a mismatched closing tag")
    {
        const MarkupError error = Refused("<screen>\n"
                                          "  <column>\n"
                                          "  </row>\n"
                                          "</screen>\n");
        CHECK(error.line == 3);
        CHECK(error.message.find("row") != std::string::npos);
        CHECK(error.message.find("column") != std::string::npos);
    }

    SUBCASE("an unquoted attribute value")
    {
        const MarkupError error = Refused("<screen>\n"
                                          "  <column\n"
                                          "      gap=12 />\n"
                                          "</screen>\n");
        CHECK(error.line == 3);
        CHECK(error.message.find("unquoted") != std::string::npos);
    }

    SUBCASE("an attribute with no value")
    {
        const MarkupError error = Refused("<screen>\n"
                                          "  <column\n"
                                          "      gap />\n"
                                          "</screen>\n");
        CHECK(error.line == 3);
        CHECK(error.message.find("gap") != std::string::npos);
    }

    SUBCASE("the same attribute twice")
    {
        const MarkupError error = Refused("<screen>\n"
                                          "  <column gap=\"1\"\n"
                                          "          gap=\"2\" />\n"
                                          "</screen>\n");
        CHECK(error.line == 3);
        CHECK(error.message.find("twice") != std::string::npos);
    }
}

TEST_CASE("Markup: what belongs to a larger XML is refused by name")
{
    // Each of these is a file written against a different language. Skipping
    // one silently would hide that from whoever wrote it.
    SUBCASE("a processing instruction")
    {
        CHECK(Refused("<?xml version=\"1.0\"?>\n<screen />\n").message.find("processing instruction") !=
              std::string::npos);
    }

    SUBCASE("a doctype")
    {
        CHECK(Refused("<!DOCTYPE screen>\n<screen />\n").message.find("DOCTYPE") != std::string::npos);
    }

    SUBCASE("a CDATA section")
    {
        CHECK(Refused("<text><![CDATA[raw]]></text>").message.find("CDATA") != std::string::npos);
    }

    SUBCASE("a namespaced name")
    {
        CHECK(Refused("<ui:screen />").message.find("namespace") != std::string::npos);
    }
}

TEST_CASE("Markup: a document holds exactly one root")
{
    CHECK(Refused("").message.find("no element") != std::string::npos);
    CHECK(Refused("<!-- only a comment -->").message.find("no element") != std::string::npos);
    CHECK(Refused("<screen />\n<screen />\n").message.find("more than one") != std::string::npos);
    CHECK(Refused("Paused").message.find("starts with '<'") != std::string::npos);
}

TEST_CASE("Markup: nesting deeper than the bound is refused rather than recursed")
{
    // A file must not be able to drive the parser off the stack.
    std::string deep;
    for (uint32_t depth = 0; depth <= kMaxMarkupDepth + 1; ++depth)
    {
        deep += "<column>";
    }
    for (uint32_t depth = 0; depth <= kMaxMarkupDepth + 1; ++depth)
    {
        deep += "</column>";
    }

    CHECK(Refused(deep).message.find("nest deeper") != std::string::npos);
}
