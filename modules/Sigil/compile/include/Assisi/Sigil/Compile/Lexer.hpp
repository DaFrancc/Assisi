/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Lexer.hpp
/// @brief Sigil source text as tokens, each knowing where it was written.
///
/// Keywords come out as names: which words are reserved depends on the
/// vocabulary, which the parser does not know, so the parser and checker tell
/// them apart by text.

#include <Assisi/Sigil/Compile/Diagnostic.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::Sigil::Compile
{

enum class TokenKind : uint8_t
{
    Name,
    Int,
    Float,
    String,
    LeftBrace,
    RightBrace,
    LeftParen,
    RightParen,
    Semicolon,
    Colon,
    Comma,
    Dot,
    Arrow,
    Assign,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Not,
    And,
    Or,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    End, ///< After the last token, so the parser never reads past the end.
    Count_,
};

/// @brief How @p kind is written, for "expected ';'" messages.
[[nodiscard]] std::string_view Describe(TokenKind kind);

struct Token
{
    /// A name or number as written; a string's contents with its escapes undone.
    std::string text;
    SourceLocation where;
    TokenKind kind = TokenKind::End;
};

/// @brief The tokens of @p source, ending with one End token, or every error in
///        it, reported against @p file.
[[nodiscard]] std::expected<std::vector<Token>, Diagnostics> Lex(std::string_view source, std::string_view file);

} // namespace Assisi::Sigil::Compile
