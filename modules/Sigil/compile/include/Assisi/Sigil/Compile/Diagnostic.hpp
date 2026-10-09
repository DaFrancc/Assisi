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

struct Diagnostic
{
    std::string message;
    /// The file `where` is in: the one compiled, or a library it imports. The
    /// `{}` lets a diagnostic be built without naming it: GCC's
    /// missing-initializer warning passes over a member with an initializer.
    std::string file{};
    SourceLocation where{};
    Severity severity = Severity::Error;
};

using Diagnostics = std::vector<Diagnostic>;

/// @brief @p diagnostic as `file:line:column: error: message`.
[[nodiscard]] std::string Format(const Diagnostic &diagnostic);

/// @brief Whether any of @p diagnostics is an error.
[[nodiscard]] bool HasErrors(std::span<const Diagnostic> diagnostics);

} // namespace Assisi::Sigil::Compile
