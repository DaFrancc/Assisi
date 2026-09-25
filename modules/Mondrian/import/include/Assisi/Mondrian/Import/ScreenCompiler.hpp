/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ScreenCompiler.hpp
/// @brief A parsed markup file becoming a screen document, and the bytes a
/// package carries.
///
/// Every name a file writes is checked here, against tables this build holds
/// and against the event catalog this build linked: an element, an attribute,
/// an enumerator or an event that names nothing fails, with the line and column
/// of the thing that named it. That is the whole value of cooking — the same
/// check at load would find it on a player's machine.
///
/// CompileScreenText is the one entry both the cooker and the editor call, so
/// there is one definition of what a screen file means and not two that drift.
///
/// Cook-side only. The shipped game parses no source text.

#include <Assisi/Mondrian/Import/Markup.hpp>

#include <Assisi/Mondrian/ScreenDocument.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::Core
{
class EventCatalog;
}

namespace Assisi::Mondrian::Import
{

/// @brief How many template instances may sit inside one another. A bound, so
/// a chain of templates each using the next cannot grow the tree without end;
/// a template using itself is refused outright, whatever the count.
inline constexpr uint32_t kMaxTemplateNesting = 8;

/// @brief Reads the text of the file at an asset path, or says why it cannot.
///
/// How a compile reaches the template libraries a file imports. Taken rather
/// than reached for, so a test can serve files from memory.
using SourceReader = std::function<std::expected<std::string, std::string>(std::string_view vpath)>;

/// @brief Compiles a parsed screen into its document.
///
/// @p catalog is what an `on_click` naming an event is checked against — taken
/// rather than reached for, so a test can hand over exactly the events its case
/// is about. @p read reaches the libraries the screen imports.
[[nodiscard]] std::expected<ScreenDocument, MarkupError> CompileScreen(const MarkupElement &root,
                                                                       const Core::EventCatalog &catalog,
                                                                       const SourceReader &read);

/// @brief Parses and compiles @p text, and writes the cooked blob.
[[nodiscard]] std::expected<std::vector<std::byte>, MarkupError> CompileScreenText(std::string_view text,
                                                                                   const Core::EventCatalog &catalog,
                                                                                   const SourceReader &read);

/// @brief Checks the template library at @p vpath: its form, its imports, and
/// every template it declares, used or not. A library cooks to nothing, so this
/// is the whole of its cook.
[[nodiscard]] std::expected<void, MarkupError> CheckLibrary(std::string_view vpath, const Core::EventCatalog &catalog,
                                                            const SourceReader &read);

/// @brief Every library the file @p text imports, directly or through another
/// library, sorted: what its cooked bytes depend on besides itself.
///
/// A path that cannot be read or parsed is listed and not followed; compiling
/// the file reports it.
[[nodiscard]] std::vector<std::string> ImportedLibraries(std::string_view text, const SourceReader &read);

} // namespace Assisi::Mondrian::Import
