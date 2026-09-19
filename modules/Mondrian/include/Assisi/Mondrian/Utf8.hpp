/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Utf8.hpp
/// @brief Reads codepoints out of UTF-8 text.

#include <cstdint>
#include <string_view>

namespace Assisi::Mondrian
{

/// The codepoint that stands in for bytes that are not valid UTF-8.
inline constexpr uint32_t kReplacementCharacter = 0xFFFD;

/// @brief The codepoint starting at byte @p offset of @p text, moving @p offset
/// past it. @p offset must be inside @p text.
///
/// A sequence that is not valid UTF-8 — a stray continuation byte, one cut
/// short, an overlong form, a surrogate or a value past U+10FFFF — reads as
/// kReplacementCharacter and consumes one byte, so a decode loop always moves
/// forward and never reads past the end.
[[nodiscard]] uint32_t DecodeUtf8(std::string_view text, uint32_t &offset);

} // namespace Assisi::Mondrian
