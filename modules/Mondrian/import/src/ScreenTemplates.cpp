/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "MarkupValues.hpp"
#include "ScreenAttributes.hpp"
#include "ScreenWalk.hpp"

#include <Assisi/Mondrian/Import/ScreenCompiler.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace Assisi::Mondrian::Import
{
namespace
{

/// What separates a parameter from its default in `params`: `moves=1`.
constexpr char kDefaultSeparator = '=';

/// The parameters @p attribute, a template's `params`, declares: names
/// separated by spaces, each with an optional `=default`.
std::expected<std::vector<ParameterDeclaration>, MarkupError> ReadParameters(const MarkupAttribute &attribute)
{
    std::vector<ParameterDeclaration> parameters;
    for (const std::string_view word : SplitWords(attribute.value))
    {
        const std::size_t split = word.find(kDefaultSeparator);
        ParameterDeclaration declared{.name = std::string{word.substr(0, split)},
                                      .fallback = split == std::string_view::npos ? std::string{}
                                                                                  : std::string{word.substr(split + 1)},
                                      .hasDefault = split != std::string_view::npos};
        if (!IsParameterName(declared.name))
        {
            return std::unexpected(At(attribute, "'" + declared.name +
                                                     "' is not a parameter name: letters, digits and '_', "
                                                     "not starting with a digit."));
        }
        // An instance attribute is then a parameter or an override, never
        // either depending on how it is read.
        if (IsMarkupAttribute(declared.name) || declared.name == kParamsAttribute)
        {
            return std::unexpected(At(attribute, "'" + declared.name +
                                                     "' is already an attribute this markup has, so a parameter "
                                                     "needs another name."));
        }
        if (std::ranges::find(parameters, declared.name, &ParameterDeclaration::name) != parameters.end())
        {
            return std::unexpected(At(attribute, "'" + declared.name + "' is declared twice."));
        }
        parameters.push_back(std::move(declared));
    }
    return parameters;
}

/// A template use found beneath a declaration: where it is written, and which
/// template it names.
struct Use
{
    const MarkupElement *element = nullptr;
    const Template *used = nullptr;
};

/// Every instance anywhere beneath @p element, as @p space resolves them, in
/// the order the file writes them.
void CollectUses(const Namespace &space, const MarkupElement &element, std::vector<Use> &uses)
{
    for (const MarkupElement &child : element.children)
    {
        if (const Namespace::const_iterator found = space.find(child.name); found != space.end())
        {
            uses.push_back(Use{.element = &child, .used = found->second});
        }
        CollectUses(space, child, uses);
    }
}

/// Refuses a template that holds itself, through @p path — the templates being
/// followed, outermost first. Each template is followed once; @p done holds
/// those already cleared.
///
/// A library cannot import a file that imports it, so a template can only reach
/// itself through templates of its own file; the walk follows every use anyway.
///
/// Stops following at the nesting bound: a chain that long is refused when it
/// is compiled, and a cycle inside it with it.
std::expected<void, MarkupError> RefuseCycles(std::vector<const Template *> &path,
                                              std::unordered_set<const Template *> &done)
{
    if (path.size() > kMaxTemplateNesting)
    {
        return {};
    }
    const Template *const current = path.back();
    std::vector<Use> uses;
    CollectUses(*current->home, *current->declaration, uses);

    for (const Use &use : uses)
    {
        const std::vector<const Template *>::const_iterator seen = std::ranges::find(path, use.used);
        if (seen != path.end())
        {
            std::string chain = "'" + (*seen)->name + "'";
            for (std::vector<const Template *>::const_iterator next = seen + 1; next != path.end(); ++next)
            {
                chain += ", which holds '" + (*next)->name + "'";
            }
            chain += ", which holds '" + use.element->name + "'";
            return std::unexpected(
                At(*use.element, "a template may not hold itself, however far down: " + chain + "."));
        }
        if (done.contains(use.used))
        {
            continue;
        }
        path.push_back(use.used);
        if (const std::expected<void, MarkupError> inner = RefuseCycles(path, done); !inner)
        {
            return inner;
        }
        path.pop_back();
    }
    done.insert(current);
    return {};
}

} // namespace

std::expected<void, MarkupError> CollectTemplates(TemplateFile &into, const MarkupElement &root)
{
    for (const MarkupElement &declaration : root.children)
    {
        if (declaration.name != kTemplateElement)
        {
            continue;
        }
        const MarkupAttribute *const named = declaration.Find("name");
        const MarkupAttribute *const params = declaration.Find(kParamsAttribute);
        const std::size_t expected = params != nullptr ? 2 : 1;
        if (named == nullptr || named->value.empty() || declaration.attributes.size() != expected)
        {
            return std::unexpected(At(declaration, "a template says its name, and what it takes, and nothing "
                                                   "else: <" +
                                                       std::string{kTemplateElement} + " name=\"...\" " +
                                                       std::string{kParamsAttribute} + "=\"...\">."));
        }
        const std::string &name = named->value;
        if (name.find(kNameSeparator) != std::string::npos)
        {
            return std::unexpected(At(*named, "'" + name + "' holds '" + kNameSeparator +
                                                  "', which is how an instance qualifies the names inside it."));
        }
        if (FindElement(name) != nullptr || name == kScreenElement || name == kTemplateElement ||
            name == kTemplatesElement || name == kImportElement)
        {
            return std::unexpected(At(*named, "'" + name +
                                                  "' is already an element this markup has, so a template "
                                                  "needs another name."));
        }
        if (const std::unordered_map<std::string, Template>::const_iterator first = into.declared.find(name);
            first != into.declared.end())
        {
            return std::unexpected(At(*named, "'" + name + "' is already a template in this file (line " +
                                                  std::to_string(first->second.declaration->line) + ")."));
        }
        if (!declaration.text.empty() || declaration.children.size() != 1)
        {
            return std::unexpected(At(declaration, "template '" + name +
                                                       "' holds one element, which every instance is built "
                                                       "from, and nothing else."));
        }
        // One layer of overrides: a root that was itself an instance would put
        // this template's attributes over another's, under the instance's.
        const MarkupElement &body = declaration.children.front();
        if (FindElement(body.name) == nullptr)
        {
            return std::unexpected(At(body, "template '" + name + "' is built on <" + body.name +
                                                ">, and a template is built on an element this markup has. "
                                                "Use other templates inside it."));
        }
        std::vector<ParameterDeclaration> parameters;
        if (params != nullptr)
        {
            std::expected<std::vector<ParameterDeclaration>, MarkupError> read = ReadParameters(*params);
            if (!read)
            {
                return std::unexpected(read.error());
            }
            parameters = std::move(*read);
        }
        Template &declared = into.declared[name];
        declared = Template{.parameters = std::move(parameters),
                            .name = name,
                            .file = into.file,
                            .declaration = &declaration,
                            .root = &body,
                            .home = &into.visible};
        into.visible.emplace(name, &declared);
    }
    return {};
}

std::expected<void, MarkupError> CheckTemplates(const TemplateFile &file, const MarkupElement &root,
                                                const Core::EventCatalog &catalog)
{
    for (const MarkupElement &declaration : root.children)
    {
        if (declaration.name != kTemplateElement)
        {
            continue;
        }
        const Template &checked = file.declared.at(declaration.Find("name")->value);

        std::vector<const Template *> path{&checked};
        std::unordered_set<const Template *> done;
        if (const std::expected<void, MarkupError> cycles = RefuseCycles(path, done); !cycles)
        {
            return cycles;
        }

        ScreenDocument scratch;
        scratch.nodes.emplace_back();
        Walk check{.document = scratch,
                   .catalog = catalog,
                   .names = {},
                   .targets = {},
                   .scopes = {},
                   .space = &file.visible,
                   .file = file.file};
        MarkupElement standIn;
        standIn.name = checked.name;
        standIn.line = declaration.line;
        standIn.column = declaration.column;
        if (const std::expected<void, MarkupError> compiled = CompileInstance(check, standIn, checked, 0, true);
            !compiled)
        {
            return compiled;
        }
        if (const std::expected<void, MarkupError> targets = ResolveTargets(check); !targets)
        {
            return targets;
        }
    }
    return {};
}

} // namespace Assisi::Mondrian::Import
