/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Evaluate.hpp
/// @brief Runs compiled expressions against a block of values.
///
/// Nothing here allocates or calls out: an expression reads its block and its
/// own code and nothing else. Code is trusted, so it must have passed Verify
/// against a block of at least the size given.

#include <Assisi/Sigil/Bytecode.hpp>
#include <Assisi/Sigil/Layout.hpp>

#include <cstdint>
#include <span>

namespace Assisi::Sigil
{

/// @brief The value of the expression at @p entry in @p code.
[[nodiscard]] Word Evaluate(std::span<const Word> code, uint32_t entry, std::span<const Word> block);

/// @brief Fills the let slots of @p block, each let from its entry in
///        @p letEntries, in order, so a let reads the ones before it as this
///        frame's values. Runs once a frame, after the params and implied
///        values are written and before any condition is evaluated.
void EvaluateLets(std::span<const Word> code, std::span<const uint32_t> letEntries, const Layout &layout,
                  std::span<Word> block);

} // namespace Assisi::Sigil
