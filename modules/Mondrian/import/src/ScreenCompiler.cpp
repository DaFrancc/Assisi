/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Mondrian/Import/ScreenCompiler.hpp>

#include "ScreenAttributes.hpp"
#include "ScreenLibraries.hpp"
#include "ScreenWalk.hpp"

#include <Assisi/Mondrian/ScreenBlob.hpp>

#include <Assisi/Core/BitStream.hpp>

#include <span>
#include <utility>

namespace Assisi::Mondrian::Import
{

std::expected<ScreenDocument, MarkupError> CompileScreen(const MarkupElement &root, const Core::EventCatalog &catalog,
                                                         const SourceReader &read)
{
    if (root.name == kTemplatesElement)
    {
        return std::unexpected(At(root, "<" + std::string{kTemplatesElement} + "> starts a template library, a " +
                                            std::string{kLibraryExtension} + " file. A screen file starts with <" +
                                            std::string{kScreenElement} + ">."));
    }
    if (root.name != kScreenElement)
    {
        return std::unexpected(
            At(root, "a screen file starts with <" + std::string{kScreenElement} + ">, not <" + root.name + ">."));
    }
    if (!root.text.empty())
    {
        return std::unexpected(At(root, "the screen holds text, which only text and button carry."));
    }

    ScreenDocument document;
    // The root of the tree, which the loader applies to the tree's own root
    // rather than creating. Its direction is a column because a screen that
    // stacks what it holds is the common case, and a file says `direction`
    // when it wants otherwise.
    ScreenNode screenNode;
    screenNode.style.direction = Direction::Column;
    document.nodes.push_back(std::move(screenNode));

    TemplateFile screen;
    Libraries libraries{.read = read, .catalog = catalog, .loaded = {}, .chain = {}};
    Walk walk{.document = document,
              .catalog = catalog,
              .names = {},
              .targets = {},
              .scopes = {},
              .space = &screen.visible,
              .file = {}};
    if (const std::expected<void, MarkupError> attributes = ApplyAttributes(walk, root, 0); !attributes)
    {
        return std::unexpected(attributes.error());
    }
    if (const std::expected<void, MarkupError> templates = CollectTemplates(screen, root); !templates)
    {
        return std::unexpected(templates.error());
    }
    if (const std::expected<void, MarkupError> imported = ApplyImports(screen, libraries, root); !imported)
    {
        return std::unexpected(imported.error());
    }
    if (const std::expected<void, MarkupError> checked = CheckTemplates(screen, root, catalog); !checked)
    {
        return std::unexpected(checked.error());
    }
    for (const MarkupElement &child : root.children)
    {
        // Declarations and imports make nothing themselves; instances do.
        if (child.name == kTemplateElement || child.name == kImportElement)
        {
            continue;
        }
        if (const std::expected<void, MarkupError> compiled = CompileElement(walk, child, 0); !compiled)
        {
            return std::unexpected(compiled.error());
        }
    }
    if (const std::expected<void, MarkupError> targets = ResolveTargets(walk); !targets)
    {
        return std::unexpected(targets.error());
    }
    return document;
}

std::expected<std::vector<std::byte>, MarkupError> CompileScreenText(std::string_view text,
                                                                     const Core::EventCatalog &catalog,
                                                                     const SourceReader &read)
{
    const std::expected<MarkupElement, MarkupError> parsed = ParseMarkup(text);
    if (!parsed)
    {
        return std::unexpected(parsed.error());
    }

    const std::expected<ScreenDocument, MarkupError> document = CompileScreen(*parsed, catalog, read);
    if (!document)
    {
        return std::unexpected(document.error());
    }

    Core::BitWriter writer;
    WriteCookedScreen(writer, *document);
    const std::span<const std::byte> bytes = writer.Data();
    return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

} // namespace Assisi::Mondrian::Import
