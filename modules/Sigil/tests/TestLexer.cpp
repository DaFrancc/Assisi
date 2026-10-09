/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestLexer.cpp
/// @brief Source text becomes tokens that know where they were written: numbers
/// tell ints from floats, comments vanish without throwing positions off,
/// strings undo their escapes, and what can't be a token is refused where it is.

#include "SigilTesting.hpp"

#include <doctest/doctest.h>

#include <Assisi/Sigil/Compile/Lexer.hpp>

using namespace Assisi::Sigil::Compile;
using Assisi::Sigil::Testing::Dump;

namespace
{

std::vector<Token> Tokens(std::string_view source)
{
    std::expected<std::vector<Token>, Diagnostics> tokens = Lex(source, "t.sgl");
    REQUIRE_MESSAGE(tokens.has_value(), Dump(tokens.has_value() ? Diagnostics{} : tokens.error()));
    return std::move(*tokens);
}

Diagnostics LexErrors(std::string_view source)
{
    std::expected<std::vector<Token>, Diagnostics> tokens = Lex(source, "t.sgl");
    REQUIRE_FALSE(tokens.has_value());
    return std::move(tokens.error());
}

} // namespace

TEST_CASE("Lexer: 2 is an int and 2.0 a float")
{
    const std::vector<Token> tokens = Tokens("2 2.0");
    REQUIRE(tokens.size() == 3);
    CHECK(tokens[0].kind == TokenKind::Int);
    CHECK(tokens[1].kind == TokenKind::Float);
    CHECK(tokens[2].kind == TokenKind::End);
}

TEST_CASE("Lexer: comments are skipped, and the lines and columns after them still count from the source")
{
    const std::vector<Token> tokens = Tokens("// one\n/* two\n three */ x");
    REQUIRE(tokens.size() == 2);
    CHECK(tokens[0].text == "x");
    CHECK(tokens[0].where.line == 3);
    CHECK(tokens[0].where.column == 11);
}

TEST_CASE("Lexer: a string's escapes are undone")
{
    const std::vector<Token> tokens = Tokens(R"("a\"b\\")");
    REQUIRE(tokens.size() == 2);
    CHECK(tokens[0].kind == TokenKind::String);
    CHECK(tokens[0].text == "a\"b\\");
}

TEST_CASE("Lexer: two-character operators are one token each")
{
    const std::vector<Token> tokens = Tokens("a->b<=c&&d");
    REQUIRE(tokens.size() == 8);
    CHECK(tokens[1].kind == TokenKind::Arrow);
    CHECK(tokens[3].kind == TokenKind::LessEqual);
    CHECK(tokens[5].kind == TokenKind::And);
}

TEST_CASE("Lexer: what can't be read is refused where it starts")
{
    const Diagnostics unclosed = LexErrors("x \"abc");
    REQUIRE(unclosed.size() == 1);
    CHECK(unclosed[0].where.column == 3);
    CHECK(unclosed[0].message.find("never closed") != std::string::npos);

    const Diagnostics digit = LexErrors("\n9abc");
    REQUIRE(digit.size() == 1);
    CHECK(digit[0].where.line == 2);
    CHECK(digit[0].message.find("can't start with a digit") != std::string::npos);

    CHECK(LexErrors("/* never closed")[0].message.find("*/") != std::string::npos);
    CHECK(LexErrors(R"("\n")")[0].message.find("escapes") != std::string::npos);
    CHECK(LexErrors("a # b")[0].where.column == 3);
    CHECK(LexErrors("99999999999")[0].message.find("too large") != std::string::npos);
}
