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

#include <Assisi/Math/Color.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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

/// @brief `#rrggbb`, `#rrggbbaa`, or three or four numbers from 0 to 1.
///
/// Both forms because they answer different questions: a hex triple is what a
/// palette or a design tool hands over, and floats are what a value carried
/// from code keeps exactly.
[[nodiscard]] std::optional<Math::Color4<Math::ColorSpace::Srgb>> ParseColor(std::string_view text);

/// @brief `fit`, `grow`, `fixed <n>` or `percent <n>`, each optionally followed
/// by `min <n>` and `max <n>`.
[[nodiscard]] std::optional<Sizing> ParseSizing(std::string_view text);

/// @brief One number for all four edges, or four for left, top, right, bottom.
[[nodiscard]] std::optional<Padding> ParsePadding(std::string_view text);

/// @brief Two alignments, across then down.
[[nodiscard]] std::optional<std::array<Alignment, kAxisCount>> ParseAlignPair(std::string_view text);

/// @brief Which axes a value names: `x`, `y`, `xy`, or `none`.
[[nodiscard]] std::optional<std::array<bool, kAxisCount>> ParseAxes(std::string_view text);

/// @brief A layer the engine names, or a plain number for one between them.
[[nodiscard]] std::optional<int32_t> ParseSortKey(std::string_view text);

[[nodiscard]] std::optional<Alignment> ParseAlignment(std::string_view text);
[[nodiscard]] std::optional<Direction> ParseDirection(std::string_view text);
[[nodiscard]] std::optional<CornerStyle> ParseCornerStyle(std::string_view text);
[[nodiscard]] std::optional<TextAlign> ParseTextAlign(std::string_view text);
[[nodiscard]] std::optional<ScrollBarVisibility> ParseScrollBarVisibility(std::string_view text);
[[nodiscard]] std::optional<ScrollBarDrag> ParseScrollBarDrag(std::string_view text);
[[nodiscard]] std::optional<FloatAnchor> ParseFloatAnchor(std::string_view text);

} // namespace Assisi::Mondrian::Import
