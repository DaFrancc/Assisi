/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Diagnostic.hpp
/// @brief What the compiler says about a file: errors that stop it, and
///        warnings that don't, each pointing at a place in the source.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Assisi::Sigil::Compile
{

/// @brief A place in a source file. One-based, counted the way an editor shows
///        them, so a message can be pasted at a file and land on the character.
struct SourceLocation
{
    uint32_t line = 1;
    uint32_t column = 1;
};

enum class Severity : uint8_t
{
    Error,   ///< The file does not compile.
    Warning, ///< The file compiles; something in it is probably a mistake.
    Count_,
};

/// @brief One thing the compiler says. Each member has an initializer, so a
///        diagnostic can be built naming only some: GCC's missing-initializer
///        warning passes over a member that has one.
struct Diagnostic
{
    /// What is wrong, in one short line.
    std::string message;
    /// How to put it right, when there's more to say than fits the message.
    std::string help{};
    /// The file `where` is in: the one compiled, or a library it imports.
    std::string file{};
    /// The text of the line `where` is on, filled in by the compile.
    std::string excerpt{};
    SourceLocation where{};
    Severity severity = Severity::Error;
};

using Diagnostics = std::vector<Diagnostic>;

enum class Style : uint8_t
{
    Plain, ///< For logs and files.
    Color, ///< ANSI colors, for a terminal.
    Count_,
};

/// @brief @p diagnostic for a person to read: `file:line:column: error:
///        message`, then the source line with a caret under the place, then the
///        help, each on a line of its own and the last ending in a newline.
[[nodiscard]] std::string Format(const Diagnostic &diagnostic, Style style = Style::Plain);

/// @brief Whether any of @p diagnostics is an error.
[[nodiscard]] bool HasErrors(std::span<const Diagnostic> diagnostics);

} // namespace Assisi::Sigil::Compile
