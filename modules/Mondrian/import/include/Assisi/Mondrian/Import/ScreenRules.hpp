/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ScreenRules.hpp
/// @brief What a screen is checked against beyond its own file: the project's
/// string tables, whether literal text is allowed, and the project's themes.
///
/// Read once per compile from the UI settings and the files they list, and
/// handed to the screen compiler, so the cook and the editor check a screen
/// against the same things.
///
/// Cook-side only. The shipped game parses no source text.

#include <Assisi/Mondrian/Import/Markup.hpp>
#include <Assisi/Mondrian/Import/SourceReader.hpp>
#include <Assisi/Mondrian/Import/StringTableCompiler.hpp>
#include <Assisi/Mondrian/Import/ThemeCompiler.hpp>

#include <Assisi/Mondrian/StringTable.hpp>
#include <Assisi/Mondrian/UiConfig.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace Assisi::Mondrian::Import
{

/// @brief Whether text a player reads may be written in a screen as it is.
enum class LiteralText : uint8_t
{
    Allowed,
    RequiresKey, ///< only a key into a table, except on a debug-only screen
    Count_
};

/// @brief A theme the project lists, as a screen is checked against it.
struct ProjectTheme
{
    /// Where it was read from, for a message about it.
    std::string vpath;
    CompiledTheme compiled;
};

/// @brief What a screen is checked against.
struct ScreenRules
{
    StringTables tables;
    /// A style or a token a screen names must be in every one of these, in
    /// every mode, since the game may be wearing any of them.
    std::vector<ProjectTheme> themes;
    LiteralText literals = LiteralText::Allowed;
};

/// @brief The UI settings, through @p read. A project with none has the
/// defaults; one that does not parse is an error naming the file.
[[nodiscard]] std::expected<UiConfig, MarkupError> LoadUiConfig(const SourceReader &read);

/// @brief What an empty cell in a table means under @p config.
[[nodiscard]] EmptyText EmptyTextFor(const UiConfig &config);

/// @brief The settings and every table and theme they list, through @p read.
///
/// A listed table or theme that cannot be read or compiled is an error naming
/// it: a screen's keys and looks cannot be checked without it. Two listed
/// tables with one name are an error naming both.
[[nodiscard]] std::expected<ScreenRules, MarkupError> LoadScreenRules(const SourceReader &read);

} // namespace Assisi::Mondrian::Import
