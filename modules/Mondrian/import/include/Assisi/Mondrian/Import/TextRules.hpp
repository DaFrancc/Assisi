/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TextRules.hpp
/// @brief What a screen's text is checked against: the project's string tables,
/// and whether literal text is allowed.
///
/// Read once per compile from the UI settings and the tables they list, and
/// handed to the screen compiler, so the cook and the editor check a screen
/// against the same things.
///
/// Cook-side only. The shipped game parses no source text.

#include <Assisi/Mondrian/Import/Markup.hpp>
#include <Assisi/Mondrian/Import/SourceReader.hpp>
#include <Assisi/Mondrian/Import/StringTableCompiler.hpp>

#include <Assisi/Mondrian/StringTable.hpp>
#include <Assisi/Mondrian/UiConfig.hpp>

#include <cstdint>
#include <expected>

namespace Assisi::Mondrian::Import
{

/// @brief Whether text a player reads may be written in a screen as it is.
enum class LiteralText : uint8_t
{
    Allowed,
    RequiresKey, ///< only a key into a table, except on a debug-only screen
    Count
};

/// @brief What a screen's text is checked against.
struct TextRules
{
    StringTables tables;
    LiteralText literals = LiteralText::Allowed;
};

/// @brief The UI settings, through @p read. A project with none has the
/// defaults; one that does not parse is an error naming the file.
[[nodiscard]] std::expected<UiConfig, MarkupError> LoadUiConfig(const SourceReader &read);

/// @brief What an empty cell in a table means under @p config.
[[nodiscard]] EmptyText EmptyTextFor(const UiConfig &config);

/// @brief The settings and every table they list, through @p read.
///
/// A listed table that cannot be read or compiled is an error naming it: a
/// screen's keys cannot be checked without it. Two listed tables with one name
/// are an error naming both.
[[nodiscard]] std::expected<TextRules, MarkupError> LoadTextRules(const SourceReader &read);

} // namespace Assisi::Mondrian::Import
