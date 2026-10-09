/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Compile.hpp
/// @brief A `.sgl` file's text into a checked Program.
///
/// Cook-side only: the game runs what the cook makes of a Program, and parses
/// no source text.

#include <Assisi/Sigil/Compile/Diagnostic.hpp>
#include <Assisi/Sigil/Compile/Program.hpp>
#include <Assisi/Sigil/Compile/Vocabulary.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Assisi::Sigil::Compile
{

/// @brief How deep imports may chain, past which a file is refused rather than
///        followed further.
inline constexpr uint32_t kMaxImportDepth = 16;

/// @brief Reads the text of the file at a path, or says why it can't. How a
///        compile reaches the libraries a file imports; taken rather than
///        reached for, so a test can serve files from memory.
using SourceReader = std::function<std::expected<std::string, std::string>(std::string_view path)>;

/// @brief The vocabulary @p source names on its `use` line, without compiling
///        it, so a caller can pick what to compile it with. Nothing when the
///        file doesn't start with one.
[[nodiscard]] std::optional<std::string> ReadUseLine(std::string_view source);

/// @brief @p source checked against the vocabulary it names, which must be one
///        of @p vocabularies.
///
/// @p file names the source in diagnostics. Imports are read through @p read
/// and compiled the same way. On failure, every error found and any warnings;
/// on success, the warnings are in Program::warnings.
///
/// Not called `Compile`, which would hide the namespace of that name from code
/// inside Assisi::Sigil.
[[nodiscard]] std::expected<Program, Diagnostics> CompileSource(std::string_view source, std::string_view file,
                                                                std::span<const Vocabulary> vocabularies,
                                                                const SourceReader &read);

} // namespace Assisi::Sigil::Compile
