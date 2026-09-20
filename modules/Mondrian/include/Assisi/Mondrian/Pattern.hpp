/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Pattern.hpp
/// @brief A regular expression a text field measures itself against.
///
/// A pattern is compiled once and asked, on each edit, one question with three
/// answers: the text matches, the text could still become a match, or it never
/// will. The middle answer is what lets a field be typed into at all — "jim@"
/// is not an address and never stops anyone reaching one — and it is why this
/// wraps a library rather than the standard regular expressions, which cannot
/// answer it.
///
/// Matching is bounded: a pattern that would otherwise backtrack for the rest
/// of the frame gives up and reports no match instead.

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace Assisi::Mondrian
{

/// @brief A compiled pattern. Immutable, and shared by every field using it.
class Pattern;

/// @brief Why a pattern would not compile, and where the trouble starts.
struct PatternError
{
    std::string message;
    uint32_t offset = 0; ///< the byte of the pattern the message is about
};

/// @brief What a pattern makes of a piece of text.
enum class PatternMatch : uint8_t
{
    No,      ///< it does not match, and no addition to it would
    Partial, ///< it does not match yet, but something added to it could
    Yes,
    Count
};

/// @brief Compiles @p pattern, or says why it could not, pointing at the byte
/// of it the trouble starts at.
///
/// Patterns are Perl-compatible and run over codepoints rather than bytes, so
/// `.` is one codepoint however many bytes it takes — but a letter and the
/// accent written on it are two codepoints and one character, and `\X` is the
/// one that counts characters the way a caret does.
[[nodiscard]] std::expected<std::shared_ptr<const Pattern>, PatternError> CompilePattern(std::string_view pattern);

/// @brief What @p pattern makes of @p text, which must match in full: a pattern
/// describes the whole field, not a part of it.
///
/// Empty text is no match for any pattern that wants a character, rather than
/// text on its way to being one, so a field that wants to allow itself to be
/// emptied allows that itself rather than asking here.
[[nodiscard]] PatternMatch MatchPattern(const Pattern &pattern, std::string_view text);

/// The patterns a field is likely to want, spelled once. Each matches ASCII
/// only: a pattern is a rule about what belongs in a particular field, and the
/// fields these suit — a port number, a product key, an address — are the ones
/// whose contents are ASCII by definition. A field that holds a person's name
/// or anything they wrote wants no pattern at all.
namespace Patterns
{

inline constexpr std::string_view kAlphabetic = "[A-Za-z]+";
inline constexpr std::string_view kAlphanumeric = "[A-Za-z0-9]+";
inline constexpr std::string_view kInteger = "-?[0-9]+";
inline constexpr std::string_view kReal = "-?[0-9]*\\.?[0-9]+";
inline constexpr std::string_view kEmail = "[^@[:space:]]+@[^@[:space:]]+\\.[^@[:space:]]+";

} // namespace Patterns

} // namespace Assisi::Mondrian
