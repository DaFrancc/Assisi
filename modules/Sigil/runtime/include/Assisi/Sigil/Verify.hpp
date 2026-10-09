/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Verify.hpp
/// @brief Checks code before it is trusted, so a damaged asset is refused
///        when it loads rather than read past its end while the game runs.

#include <Assisi/Sigil/Bytecode.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace Assisi::Sigil
{

/// @brief Whether the expression at @p entry in @p code is safe to evaluate
///        against a block of @p slotCount words, or why not.
///
/// It is when every opcode is known; every slot is inside the block; every
/// push has its word; every jump goes forward to an instruction no further
/// than the Return; the stack never runs dry or deeper than kMaxStackDepth;
/// both paths of a jump meet with the same depth; and it ends in a Return
/// with exactly one word left.
[[nodiscard]] std::expected<void, std::string> Verify(std::span<const Word> code, uint32_t entry,
                                                      uint32_t slotCount);

} // namespace Assisi::Sigil
