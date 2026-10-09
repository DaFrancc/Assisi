/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Parser.hpp
/// @brief Tokens into a Syntax::File.

#include <Assisi/Sigil/Compile/Diagnostic.hpp>
#include <Assisi/Sigil/Compile/Lexer.hpp>
#include <Assisi/Sigil/Compile/Syntax.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace Assisi::Sigil::Compile
{

/// @brief How deep blocks may nest. A bound, so a file cannot drive the
///        parser's recursion off the stack.
inline constexpr uint32_t kMaxBlockDepth = 32;

/// @brief How deep an expression may nest, for the same reason.
inline constexpr uint32_t kMaxExpressionDepth = 64;

/// @brief The file @p tokens spell, or every error in them, reported against
///        @p file. @p tokens ends with an End token, as Lex leaves it.
[[nodiscard]] std::expected<Syntax::File, Diagnostics> Parse(std::span<const Token> tokens, std::string_view file);

} // namespace Assisi::Sigil::Compile
