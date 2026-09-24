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

/// @brief Compiles a parsed screen into its document.
///
/// @p catalog is what an `on_click` naming an event is checked against — taken
/// rather than reached for, so a test can hand over exactly the events its case
/// is about.
[[nodiscard]] std::expected<ScreenDocument, MarkupError> CompileScreen(const MarkupElement &root,
                                                                       const Core::EventCatalog &catalog);

/// @brief Parses and compiles @p text, and writes the cooked blob.
[[nodiscard]] std::expected<std::vector<std::byte>, MarkupError> CompileScreenText(std::string_view text,
                                                                                   const Core::EventCatalog &catalog);

} // namespace Assisi::Mondrian::Import
