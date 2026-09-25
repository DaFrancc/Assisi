/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ScreenLibraries.hpp
/// @brief Template libraries, `.amdt` files, loaded once per compile and
/// brought into a file's names by its `<import>`s.
///
/// Internal to the import library.

#include "ScreenWalk.hpp"

#include <Assisi/Mondrian/Import/ScreenCompiler.hpp>

#include <deque>
#include <expected>
#include <string>
#include <vector>

namespace Assisi::Mondrian::Import
{

/// A template library, parsed and checked.
struct Library
{
    /// What every Template in `templates` points into.
    MarkupElement root;
    TemplateFile templates;
};

/// Every library one compile has loaded, and the chain of imports being
/// followed, which is how an import of a file already on it is found.
struct Libraries
{
    const SourceReader &read;
    const Core::EventCatalog &catalog;
    /// A deque, because a Template points into its library and a library is
    /// loaded while the one importing it is still being filled.
    std::deque<Library> loaded;
    std::vector<std::string> chain;
};

/// Every `<import>` directly inside @p root, in the order the file writes them,
/// brought into @p into's names. Run after the file's own templates are
/// collected, so a clash is reported at the import.
[[nodiscard]] std::expected<void, MarkupError> ApplyImports(TemplateFile &into, Libraries &libraries,
                                                            const MarkupElement &root);

} // namespace Assisi::Mondrian::Import
