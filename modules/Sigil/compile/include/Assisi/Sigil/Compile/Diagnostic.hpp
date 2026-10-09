/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Diagnostic.hpp
/// @brief What the compiler says about a file: errors that stop it, and
///        warnings that don't, each pointing at the stretch of source it's about.

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

/// @brief A stretch of one line: where it starts and how many characters it covers.
struct Span
{
    SourceLocation where{};
    uint32_t length = 1;
};

enum class Severity : uint8_t
{
    Error,   ///< The file does not compile.
    Warning, ///< The file compiles; something in it is probably a mistake.
    Count_,
};

/// @brief Another place in the same file that explains the diagnostic, such as
///        where a name was first declared.
struct Related
{
    std::string label;
    /// The text of the line `span` is on, filled in by the compile.
    std::string excerpt{};
    Span span{};
};

enum class EditKind : uint8_t
{
    Replace,      ///< `span` on its line becomes `text`; a span of no length inserts it.
    InsertBefore, ///< `text` becomes a new line above line `span.where.line`.
    InsertAfter,  ///< `text` becomes a new line below line `span.where.line`.
    Delete,       ///< Line `span.where.line` goes.
    Count_,
};

/// @brief One change a suggestion makes to the source.
struct Edit
{
    std::string text{};
    /// The line the edit is on, as it is now.
    std::string line{};
    Span span{};
    EditKind kind = EditKind::Replace;
};

/// @brief A fix the compiler is sure of, shown as the source would read with it.
struct Suggestion
{
    std::string message;
    std::vector<Edit> edits{};
};

/// @brief One thing the compiler says. Each member has an initializer, so a
///        diagnostic can be built naming only some: GCC's missing-initializer
///        warning passes over a member that has one.
struct Diagnostic
{
    /// What is wrong, in one short line.
    std::string message;
    /// A few words under the place itself.
    std::string label{};
    /// How to put it right, when there's more to say than fits the label.
    std::string help{};
    /// The file `where` is in: the one compiled, or a library it imports.
    std::string file{};
    /// The text of the line `where` is on, filled in by the compile.
    std::string excerpt{};
    std::vector<Related> related{};
    std::vector<Suggestion> suggestions{};
    SourceLocation where{};
    /// How many characters from `where` the diagnostic is about.
    uint32_t length = 1;
    Severity severity = Severity::Error;
};

using Diagnostics = std::vector<Diagnostic>;

enum class Style : uint8_t
{
    Plain, ///< For logs and files.
    Color, ///< ANSI colors, for a terminal.
    Count_,
};

/// @brief @p diagnostic for a person to read: the message, then `--> file:line:
///        column`, then each line it's about with the place underlined and
///        labelled, then the help, then each suggestion as the source would
///        read with it. Ends with a newline.
[[nodiscard]] std::string Format(const Diagnostic &diagnostic, Style style = Style::Plain);

/// @brief Whether any of @p diagnostics is an error.
[[nodiscard]] bool HasErrors(std::span<const Diagnostic> diagnostics);

} // namespace Assisi::Sigil::Compile
