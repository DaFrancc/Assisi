/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "ScreenLibraries.hpp"

#include "MarkupValues.hpp"
#include "ScreenAttributes.hpp"

#include <algorithm>
#include <utility>

namespace Assisi::Mondrian::Import
{
namespace
{

/// @p error, placed in @p file when nothing has placed it yet.
MarkupError InFile(MarkupError error, std::string_view file)
{
    if (error.file.empty())
    {
        error.file = file;
    }
    return error;
}

/// Refuses a library root that is not one: `<templates>` holding templates and
/// imports, and nothing else.
std::expected<void, MarkupError> CheckLibraryRoot(const MarkupElement &root)
{
    if (root.name == kScreenElement)
    {
        return std::unexpected(At(root, "a template library starts with <" + std::string{kTemplatesElement} + ">. A <" +
                                            std::string{kScreenElement} + "> is a screen, which is a " +
                                            std::string{kScreenExtension} + " file of its own."));
    }
    if (root.name != kTemplatesElement)
    {
        return std::unexpected(At(root, "a template library starts with <" + std::string{kTemplatesElement} +
                                            ">, not <" + root.name + ">."));
    }
    bool declaresOne = false;
    for (const MarkupElement &child : root.children)
    {
        declaresOne = declaresOne || child.name == kTemplateElement;
        if (child.name == kScreenElement)
        {
            return std::unexpected(At(child, "a <" + std::string{kScreenElement} + "> is a " +
                                                 std::string{kScreenExtension} + " file of its own."));
        }
        if (child.name != kTemplateElement && child.name != kImportElement)
        {
            return std::unexpected(At(child, "a template library holds <" + std::string{kTemplateElement} + "> and <" +
                                                 std::string{kImportElement} + ">, and nothing else."));
        }
    }
    if (!root.text.empty() || !declaresOne)
    {
        return std::unexpected(At(root, "a template library declares one or more templates, and holds no text."));
    }
    return {};
}

/// Parses and checks the library at @p vpath, whose text is @p text, into a new
/// entry of @p libraries. Every error it returns names the file it is in.
std::expected<const TemplateFile *, MarkupError> BuildLibrary(Libraries &libraries, std::string_view vpath,
                                                              std::string_view text)
{
    std::expected<MarkupElement, MarkupError> parsed = ParseMarkup(text);
    if (!parsed)
    {
        return std::unexpected(InFile(std::move(parsed.error()), vpath));
    }
    Library &library = libraries.loaded.emplace_back();
    library.root = std::move(*parsed);
    library.templates.file = vpath;
    const MarkupElement &root = library.root;

    if (std::expected<void, MarkupError> checked = CheckLibraryRoot(root); !checked)
    {
        return std::unexpected(InFile(std::move(checked.error()), vpath));
    }
    if (std::expected<void, MarkupError> collected = CollectTemplates(library.templates, root); !collected)
    {
        return std::unexpected(InFile(std::move(collected.error()), vpath));
    }
    if (std::expected<void, MarkupError> imported = ApplyImports(library.templates, libraries, root); !imported)
    {
        return std::unexpected(InFile(std::move(imported.error()), vpath));
    }
    if (std::expected<void, MarkupError> checked = CheckTemplates(library.templates, root, libraries.catalog); !checked)
    {
        return std::unexpected(InFile(std::move(checked.error()), vpath));
    }
    return &library.templates;
}

/// The library an import's @p path names, loaded once per compile however many
/// files import it. Errors about the path itself point at @p path.
std::expected<const TemplateFile *, MarkupError> LoadLibrary(Libraries &libraries, const MarkupAttribute &path)
{
    const std::string &vpath = path.value;
    if (vpath.ends_with(kScreenExtension))
    {
        return std::unexpected(At(path, "'" + vpath +
                                            "' is a screen, and a screen's templates are its own. Move the ones to "
                                            "share into a template library, a " +
                                            std::string{kLibraryExtension} + " file, and import that."));
    }
    if (!vpath.ends_with(kLibraryExtension))
    {
        return std::unexpected(At(path, "'" + vpath + "' is not a template library; an import names a " +
                                            std::string{kLibraryExtension} + " file."));
    }
    if (const std::vector<std::string>::const_iterator seen = std::ranges::find(libraries.chain, vpath);
        seen != libraries.chain.end())
    {
        std::string chain;
        for (std::vector<std::string>::const_iterator next = seen; next != libraries.chain.end(); ++next)
        {
            chain += "'" + *next + "' imports ";
        }
        return std::unexpected(
            At(path, "a library may not import itself, however far down: " + chain + "'" + vpath + "'."));
    }
    for (const Library &library : libraries.loaded)
    {
        if (library.templates.file == vpath)
        {
            return &library.templates;
        }
    }

    const std::expected<std::string, std::string> text = libraries.read(vpath);
    if (!text)
    {
        return std::unexpected(At(path, "'" + vpath + "' could not be read: " + text.error()));
    }
    libraries.chain.push_back(vpath);
    std::expected<const TemplateFile *, MarkupError> built = BuildLibrary(libraries, vpath, *text);
    libraries.chain.pop_back();
    return built;
}

/// Every template @p library declares, sorted.
std::vector<std::string_view> DeclaredNames(const TemplateFile &library)
{
    std::vector<std::string_view> declared;
    for (const std::pair<const std::string, Template> &entry : library.declared)
    {
        declared.push_back(entry.first);
    }
    std::ranges::sort(declared);
    return declared;
}

/// The templates an import's @p names lists, each checked against what
/// @p library declares, or every one it declares when @p names is absent. Only
/// what the library declares: what it imports is its own business.
std::expected<std::vector<std::string_view>, MarkupError> ChosenNames(const TemplateFile &library,
                                                                      const MarkupAttribute *names)
{
    const std::vector<std::string_view> declared = DeclaredNames(library);
    if (names == nullptr)
    {
        return declared;
    }
    const std::vector<std::string_view> chosen = SplitWords(names->value);
    for (const std::string_view name : chosen)
    {
        if (!library.declared.contains(std::string{name}))
        {
            std::string known;
            for (const std::string_view one : declared)
            {
                known += known.empty() ? "" : ", ";
                known += one;
            }
            return std::unexpected(At(*names, "'" + std::string{name} + "' is not a template '" + library.file +
                                                  "' declares. It declares: " + known + "."));
        }
    }
    return chosen;
}

/// The prefix an import's @p as gives the names it brings, with the separator,
/// claimed in @p into so no other import can give it too.
std::expected<std::string, MarkupError> ClaimPrefix(TemplateFile &into, const MarkupAttribute *as)
{
    if (as == nullptr)
    {
        return std::string{};
    }
    if (!IsParameterName(as->value))
    {
        return std::unexpected(
            At(*as, "'" + as->value + "' is not a prefix: letters, digits and '_', not starting with a digit."));
    }
    if (!into.prefixes.insert(as->value).second)
    {
        return std::unexpected(At(*as, "another import already puts its templates under '" + as->value +
                                           "'. Give each import its own prefix."));
    }
    return as->value + kNameSeparator;
}

/// Brings the templates @p element imports into @p into's names.
std::expected<void, MarkupError> ApplyImport(TemplateFile &into, Libraries &libraries, const MarkupElement &element)
{
    for (const MarkupAttribute &attribute : element.attributes)
    {
        if (attribute.name != "path" && attribute.name != "names" && attribute.name != "as")
        {
            return std::unexpected(At(attribute, "'" + attribute.name +
                                                     "' is not an attribute an import has: it has path, names "
                                                     "and as."));
        }
    }
    const MarkupAttribute *const path = element.Find("path");
    if (path == nullptr || path->value.empty() || !element.text.empty() || !element.children.empty())
    {
        return std::unexpected(At(element, "an import names a library and holds nothing: <" +
                                               std::string{kImportElement} + " path=\"ui/Library" +
                                               std::string{kLibraryExtension} + "\" />."));
    }
    const std::expected<const TemplateFile *, MarkupError> loaded = LoadLibrary(libraries, *path);
    if (!loaded)
    {
        return std::unexpected(loaded.error());
    }
    const TemplateFile &library = **loaded;

    const std::expected<std::vector<std::string_view>, MarkupError> chosen =
        ChosenNames(library, element.Find("names"));
    if (!chosen)
    {
        return std::unexpected(chosen.error());
    }
    const std::expected<std::string, MarkupError> prefix = ClaimPrefix(into, element.Find("as"));
    if (!prefix)
    {
        return std::unexpected(prefix.error());
    }

    for (const std::string_view name : *chosen)
    {
        const std::string written = *prefix + std::string{name};
        if (into.visible.contains(written))
        {
            return std::unexpected(At(element, "'" + written +
                                                   "' is already a template in this file. Import it under a "
                                                   "prefix, with as=\"...\"."));
        }
        into.visible.emplace(written, &library.declared.at(std::string{name}));
    }
    return {};
}

/// The paths every `<import>` directly inside @p root names.
std::vector<std::string> ImportPaths(const MarkupElement &root)
{
    std::vector<std::string> paths;
    for (const MarkupElement &child : root.children)
    {
        const MarkupAttribute *const path = child.Find("path");
        if (child.name == kImportElement && path != nullptr)
        {
            paths.push_back(path->value);
        }
    }
    return paths;
}

} // namespace

std::expected<void, MarkupError> ApplyImports(TemplateFile &into, Libraries &libraries, const MarkupElement &root)
{
    for (const MarkupElement &child : root.children)
    {
        if (child.name != kImportElement)
        {
            continue;
        }
        if (std::expected<void, MarkupError> imported = ApplyImport(into, libraries, child); !imported)
        {
            return imported;
        }
    }
    return {};
}

std::expected<void, MarkupError> CheckLibrary(std::string_view vpath, const Core::EventCatalog &catalog,
                                              const SourceReader &read)
{
    const std::expected<std::string, std::string> text = read(vpath);
    if (!text)
    {
        return std::unexpected(MarkupError{
            .message = "could not be read: " + text.error(), .file = std::string{vpath}, .line = 1, .column = 1});
    }
    Libraries libraries{.read = read, .catalog = catalog, .loaded = {}, .chain = {std::string{vpath}}};
    const std::expected<const TemplateFile *, MarkupError> built = BuildLibrary(libraries, vpath, *text);
    if (!built)
    {
        return std::unexpected(built.error());
    }
    return {};
}

std::vector<std::string> ImportedLibraries(std::string_view text, const SourceReader &read)
{
    std::vector<std::string> found;
    const std::expected<MarkupElement, MarkupError> parsed = ParseMarkup(text);
    if (!parsed)
    {
        return found;
    }
    std::vector<std::string> pending = ImportPaths(*parsed);
    while (!pending.empty())
    {
        std::string vpath = std::move(pending.back());
        pending.pop_back();
        if (std::ranges::find(found, vpath) != found.end())
        {
            continue;
        }
        found.push_back(vpath);
        const std::expected<std::string, std::string> library = read(vpath);
        if (!library)
        {
            continue;
        }
        const std::expected<MarkupElement, MarkupError> root = ParseMarkup(*library);
        if (!root)
        {
            continue;
        }
        for (std::string &next : ImportPaths(*root))
        {
            pending.push_back(std::move(next));
        }
    }
    std::ranges::sort(found);
    return found;
}

} // namespace Assisi::Mondrian::Import
