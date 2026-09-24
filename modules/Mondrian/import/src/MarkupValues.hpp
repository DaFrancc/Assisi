/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file MarkupValues.hpp
/// @brief An attribute's text becoming the value a style field holds.
///
/// Every one of these answers nullopt for a value it cannot read, and says
/// nothing about where: the caller has the attribute, and so has the line and
/// column an author needs. Splitting it this way is what keeps one table of
/// attributes in the compiler rather than one error message per field.
///
/// Internal to the import library.

#include <Assisi/Mondrian/Style.hpp>
#include <Assisi/Mondrian/TextEdit.hpp>

#include <Assisi/Math/Color.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::Mondrian::Import
{

/// @brief One spelling of an enumerator, as a file writes it.
template <typename E> struct NamedEnum
{
    std::string_view name;
    E value;
};

/// @brief The enumerator @p name spells, or nullopt.
///
/// Names are matched exactly. A file is authored, so a value shouted is a
/// different value rather than the same one — the same rule the asset tree
/// already holds for extensions.
template <typename E, std::size_t N>
[[nodiscard]] std::optional<E> LookUpEnum(std::string_view name, const std::array<NamedEnum<E>, N> &table)
{
    for (const NamedEnum<E> &entry : table)
    {
        if (entry.name == name)
        {
            return entry.value;
        }
    }
    return std::nullopt;
}

/// @brief The whitespace-separated words of @p text, with empties dropped.
[[nodiscard]] std::vector<std::string_view> SplitWords(std::string_view text);

/// @brief `true` or `false`, and nothing else — not 1, not yes. One spelling,
/// so a file cannot half-say a thing.
[[nodiscard]] std::optional<bool> ParseBool(std::string_view text);

/// @brief A decimal number. The whole value must be consumed, so "12px" is a
/// mistake rather than twelve.
[[nodiscard]] std::optional<float> ParseFloat(std::string_view text);

[[nodiscard]] std::optional<int32_t> ParseInt(std::string_view text);

[[nodiscard]] std::optional<uint32_t> ParseUInt(std::string_view text);

/// @brief The colour a style holds.
using Color = Math::Color4<Math::ColorSpace::Srgb>;

/// @brief A colour read from a file, or why the text was not one.
using ParsedColor = std::expected<Color, std::string>;

/// @brief @p text without the whitespace around it.
[[nodiscard]] std::string_view TrimSpace(std::string_view text);

/// @brief A call as a file writes it: `name(argument, argument)`, with space
/// around the name and each argument dropped.
struct MarkupCall
{
    std::vector<std::string_view> arguments;
    std::string_view name;
};

/// @brief @p text read as a call, or nullopt when it opens none. A call that
/// opens and does not close, or has anything after it, is refused with the
/// reason rather than read as the part that parsed.
[[nodiscard]] std::expected<std::optional<MarkupCall>, std::string> SplitCall(std::string_view text);

/// @brief `#rrggbb`, `#rrggbbaa`, `rgb(r, g, b[, a])` with whole numbers from 0
/// to 255, or `rgbf(r, g, b[, a])` with numbers from 0 to 1 — or why @p text is
/// none of them.
///
/// Every form says its own scale. Bare numbers are refused because they don't:
/// `1 1 1` is white on one scale and nearly black on the other. The reason is
/// returned rather than a bare failure, because the likeliest mistakes — a
/// 0-to-255 value in `rgbf`, a fraction in `rgb` — each have one fix to name.
[[nodiscard]] ParsedColor ParseColor(std::string_view text);

/// @brief A number and its unit, with nothing between them: `16` in UI pixels,
/// or `5%`, `2vw`, `2vh`, `0.5em`. Anything else — `16px`, `5 %`, `1e` — is
/// nullopt.
[[nodiscard]] std::optional<Length> ParseLength(std::string_view text);

/// @brief `fit`, `grow`, or a length, which is a fixed size, each optionally
/// followed by `min <length>` and `max <length>`.
[[nodiscard]] std::optional<Sizing> ParseSizing(std::string_view text);

/// @brief One length for all four edges, or four for left, top, right, bottom.
[[nodiscard]] std::optional<Padding> ParsePadding(std::string_view text);

/// @brief Two alignments, across then down.
[[nodiscard]] std::optional<std::array<Alignment, kAxisCount>> ParseAlignPair(std::string_view text);

/// @brief Which axes a value names: `x`, `y`, `xy`, or `none`.
[[nodiscard]] std::optional<std::array<bool, kAxisCount>> ParseAxes(std::string_view text);

/// @brief A layer the engine names, or a plain number for one between them.
[[nodiscard]] std::optional<int32_t> ParseSortKey(std::string_view text);

/// @brief What `lines` says at once: how many lines a field holds, and how
/// tall it is counted in them.
///
/// One attribute rather than two because `height` is already a node's box on
/// the Y axis, and a field wants to say both without the two names colliding.
struct LinesValue
{
    uint32_t lines = 1;
    TextLines kind = TextLines::Single;
    TextHeight height = TextHeight::Unbounded;
};

/// @brief `single`, `multi`, `multi up-to <n>` or `multi exactly <n>`.
///
/// A height belongs only to a field of many lines, and `<n>` is at least one: a
/// field held to no lines could never hold anything.
[[nodiscard]] std::optional<LinesValue> ParseLines(std::string_view text);

[[nodiscard]] std::optional<TextMask> ParseTextMask(std::string_view text);
[[nodiscard]] std::optional<TextCheck> ParseTextCheck(std::string_view text);

/// @brief The expression the preset called @p name stands for, or nullopt.
[[nodiscard]] std::optional<std::string_view> LookUpPattern(std::string_view name);

/// @brief Every preset this build knows, for the message an unknown one prints.
[[nodiscard]] std::string KnownPatterns();

[[nodiscard]] std::optional<Alignment> ParseAlignment(std::string_view text);
[[nodiscard]] std::optional<Direction> ParseDirection(std::string_view text);
[[nodiscard]] std::optional<CornerStyle> ParseCornerStyle(std::string_view text);
[[nodiscard]] std::optional<TextAlign> ParseTextAlign(std::string_view text);
[[nodiscard]] std::optional<ScrollBarVisibility> ParseScrollBarVisibility(std::string_view text);
[[nodiscard]] std::optional<ScrollBarDrag> ParseScrollBarDrag(std::string_view text);
[[nodiscard]] std::optional<FloatAnchor> ParseFloatAnchor(std::string_view text);

} // namespace Assisi::Mondrian::Import
