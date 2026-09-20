/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Utf8.hpp
/// @brief Reads codepoints out of UTF-8 text, and steps over it by the
/// characters a reader sees rather than by the bytes or codepoints it is made
/// of.
///
/// A character here is one codepoint together with everything that modifies it:
/// an accent written separately from the letter it sits on, a variation
/// selector, an emoji's skin tone, the parts a zero-width joiner binds into one
/// picture, and the two halves of a flag. Everything a caret touches counts in
/// these, so no edit can leave half a character behind. This is not the full
/// Unicode segmentation algorithm, which needs tables this does not carry; it
/// covers what a person typing into a field produces.

#include <cstdint>
#include <string>
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

/// @brief Appends @p codepoint to @p text as UTF-8. A surrogate or a value past
/// U+10FFFF appends kReplacementCharacter, so what comes out is always valid.
void EncodeUtf8(uint32_t codepoint, std::string &text);

/// @brief Where the character after the one at byte @p offset begins.
///
/// Returns the end of @p text for an offset at or past it, so a loop stepping
/// forward always terminates. An offset inside a character is treated as its
/// start: stepping from anywhere lands on a boundary.
[[nodiscard]] uint32_t NextCharacter(std::string_view text, uint32_t offset);

/// @brief Where the character before byte @p offset begins, or zero at the start.
[[nodiscard]] uint32_t PreviousCharacter(std::string_view text, uint32_t offset);

/// @brief How many characters @p text holds.
[[nodiscard]] uint32_t CharacterCount(std::string_view text);

/// @brief The byte @p index's character starts at, or the end of @p text when
/// it holds fewer than that.
[[nodiscard]] uint32_t CharacterOffset(std::string_view text, uint32_t index);

/// @brief How many whole characters come before byte @p offset. The inverse of
/// CharacterOffset for an offset on a boundary.
[[nodiscard]] uint32_t CharacterIndex(std::string_view text, uint32_t offset);

} // namespace Assisi::Mondrian
