/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Lower.hpp
/// @brief A checked Program's expressions into the bytecode the runtime runs.
///
/// Whatever is known at cook time is worked out here, by the runtime's own
/// evaluator, so a folded value is exactly the one the game would have got.

#include <Assisi/Sigil/Bytecode.hpp>
#include <Assisi/Sigil/Compile/Program.hpp>
#include <Assisi/Sigil/Compile/Vocabulary.hpp>
#include <Assisi/Sigil/Layout.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace Assisi::Sigil::Compile
{

/// @brief The block @p program's expressions run against: its params, then
///        the functions of @p vocabulary, the one it was compiled with, then
///        its lets.
[[nodiscard]] Layout MakeLayout(const Program &program, const Vocabulary &vocabulary);

/// @brief Appends the code for @p expr to @p code, ending in a Return, and
///        gives where it starts. Refused only for an expression that isn't a
///        number or a bool, such as a clip, or code too large for an operand.
[[nodiscard]] std::expected<uint32_t, std::string> LowerExpression(const Layout &layout, const Expr &expr,
                                                                   std::vector<Word> &code);

/// @brief Appends the code for each of @p program's lets, in order, and gives
///        where each starts, as EvaluateLets takes them.
[[nodiscard]] std::expected<std::vector<uint32_t>, std::string> LowerLets(const Program &program,
                                                                          const Layout &layout,
                                                                          std::vector<Word> &code);

} // namespace Assisi::Sigil::Compile
