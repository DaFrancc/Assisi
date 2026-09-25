/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ScreenTesting.hpp
/// @brief What the screen compiler's test files share: an event catalog, files
/// served from memory, and compiling a file that should or should not compile.

#include <Assisi/Mondrian/Import/ScreenCompiler.hpp>

#include <Assisi/Core/EventCatalog.hpp>
#include <Assisi/Core/EventQueue.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace ScreenTesting
{

struct QuitRequested
{
};

/// The events a file may name here. Built per case rather than linked, so what
/// an `on_click` resolves against is what the case says it is.
inline Assisi::Core::EventCatalog OneEvent()
{
    Assisi::Core::EventCatalog catalog;
    catalog.Register({.name = "Game::QuitRequested",
                      .push = [](Assisi::Core::EventQueue &events) { events.Push(QuitRequested{}); }});
    return catalog;
}

/// Files by asset path, for a compile to import from.
using Files = std::unordered_map<std::string, std::string>;

/// A reader serving @p files and nothing else.
inline Assisi::Mondrian::Import::SourceReader Serving(Files files)
{
    return [files = std::move(files)](std::string_view vpath) -> std::expected<std::string, std::string>
    {
        const Files::const_iterator found = files.find(std::string{vpath});
        if (found == files.end())
        {
            return std::unexpected(std::string{"no such file"});
        }
        return found->second;
    };
}

/// A reader for a screen that imports nothing.
inline Assisi::Mondrian::Import::SourceReader NoFiles()
{
    return Serving({});
}

/// The reason @p result failed, or empty. Its own function because `*` binds
/// tighter than `?:` inside doctest's message macro, so a ternary written at
/// the call would be parsed as part of the stream expression.
template <typename T, typename E> std::string Why(const std::expected<T, E> &result)
{
    return result.has_value() ? std::string{} : std::string{result.error().message};
}

/// @p text compiled, importing from @p files.
inline Assisi::Mondrian::ScreenDocument Compiled(std::string_view text, const Files &files = {})
{
    using namespace Assisi::Mondrian::Import;
    const std::expected<MarkupElement, MarkupError> parsed = ParseMarkup(text);
    REQUIRE_MESSAGE(parsed.has_value(), Why(parsed));

    std::expected<Assisi::Mondrian::ScreenDocument, MarkupError> document =
        CompileScreen(*parsed, OneEvent(), Serving(files));
    REQUIRE_MESSAGE(document.has_value(), Why(document));
    return *document;
}

/// The error from compiling @p text, importing from @p files, which the case
/// expects to fail.
inline Assisi::Mondrian::Import::MarkupError Refused(std::string_view text, const Files &files = {})
{
    using namespace Assisi::Mondrian::Import;
    const std::expected<MarkupElement, MarkupError> parsed = ParseMarkup(text);
    REQUIRE_MESSAGE(parsed.has_value(), Why(parsed));

    const std::expected<Assisi::Mondrian::ScreenDocument, MarkupError> document =
        CompileScreen(*parsed, OneEvent(), Serving(files));
    REQUIRE_FALSE(document.has_value());
    return document.error();
}

/// The node called @p name, which the case expects to exist.
inline const Assisi::Mondrian::ScreenNode &NodeNamed(const Assisi::Mondrian::ScreenDocument &document,
                                                     std::string_view name)
{
    for (const Assisi::Mondrian::ScreenNode &node : document.nodes)
    {
        if (node.name == name)
        {
            return node;
        }
    }
    REQUIRE_MESSAGE(false, "no node called " << name);
    return document.nodes[0];
}

/// The index of the node called @p name, or kNoNode.
inline uint32_t IndexOf(const Assisi::Mondrian::ScreenDocument &document, std::string_view name)
{
    for (uint32_t index = 0; index < document.nodes.size(); ++index)
    {
        if (document.nodes[index].name == name)
        {
            return index;
        }
    }
    return Assisi::Mondrian::kNoNode;
}

} // namespace ScreenTesting
