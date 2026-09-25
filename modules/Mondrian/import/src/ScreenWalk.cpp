/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "ScreenWalk.hpp"

#include "MarkupValues.hpp"
#include "ScreenAttributes.hpp"

#include <Assisi/Mondrian/Import/ScreenCompiler.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>

namespace Assisi::Mondrian::Import
{
namespace
{

/// The size keywords a bare length replaced, refused by name so the message
/// can say what to write instead.
constexpr std::string_view kFixedWord = "fixed";
constexpr std::string_view kPercentWord = "percent";

/// @p name as the screen knows it: qualified with the instance it was written
/// inside, if any.
std::string Qualify(const Walk &walk, std::string_view name)
{
    return QualifyIn(CurrentPrefix(walk), name);
}

bool IsParameterStart(char character)
{
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || character == '_';
}

bool IsParameterChar(char character)
{
    return IsParameterStart(character) || (character >= '0' && character <= '9');
}

/// Names node @p index, refusing a name another node on the screen has. A name
/// is what a target and a lookup mean a node by, so two nodes sharing one would
/// leave both meaning whichever came first.
std::expected<void, MarkupError> ApplyName(Walk &walk, const MarkupAttribute &attribute, uint32_t index)
{
    // The separator is how an instance qualifies what is inside it, so a name
    // written with one could be the same name an instance makes.
    if (attribute.value.find(kNameSeparator) != std::string::npos)
    {
        return std::unexpected(At(attribute, "'" + attribute.value + "' holds '" + kNameSeparator +
                                                 "', which is how a template instance qualifies the names inside "
                                                 "it. Name the instance instead."));
    }
    const std::string name = Qualify(walk, attribute.value);
    if (!name.empty())
    {
        const std::unordered_map<std::string, NamedNode>::const_iterator first = walk.names.find(name);
        if (first != walk.names.end())
        {
            // The same attribute reached twice is one template's name, made by
            // two instances of it that each left the name as it was written.
            const bool sameSource = first->second.line == attribute.line && first->second.column == attribute.column;
            if (sameSource && !walk.scopes.empty() && !walk.scopes.back().named)
            {
                const Scope &scope = walk.scopes.back();
                return std::unexpected(MarkupError{
                    .message = "template '" + std::string{scope.templateName} + "' names '" + attribute.value +
                               "' inside it, and an unnamed instance keeps the names it makes as they are written, "
                               "so a second one makes them twice. Name the instance.",
                    .file = {},
                    .line = scope.line,
                    .column = scope.column});
            }
            return std::unexpected(At(attribute, "'" + name + "' is already the name of a node on this screen (line " +
                                                     std::to_string(first->second.line) + "). A name means one node."));
        }
        walk.names.emplace(name, NamedNode{.index = index, .line = attribute.line, .column = attribute.column});
    }
    walk.document.nodes[index].name = name;
    return {};
}

std::expected<void, MarkupError> CompileChildren(Walk &walk, const MarkupElement &element, uint32_t index)
{
    for (const MarkupElement &child : element.children)
    {
        if (const std::expected<void, MarkupError> compiled = CompileElement(walk, child, index); !compiled)
        {
            return std::unexpected(compiled.error());
        }
    }
    return {};
}

/// The template names that can be written where the walk is: the file's own,
/// or, inside a template's body, those of the file the template came from.
const Namespace &Here(const Walk &walk)
{
    return walk.scopes.empty() ? *walk.space : *walk.scopes.back().used->home;
}

/// Every name in @p space, sorted, for a message listing them.
std::string KnownTemplates(const Namespace &space)
{
    std::vector<std::string_view> sorted;
    for (const std::pair<const std::string, const Template *> &entry : space)
    {
        sorted.push_back(entry.first);
    }
    std::ranges::sort(sorted);
    std::string names;
    for (const std::string_view name : sorted)
    {
        names += names.empty() ? "" : ", ";
        names += name;
    }
    return names;
}

/// @p error, saying which instance it was found inside, called once the
/// instance's scope is closed. An error with no file yet was found in the
/// template's own. A stand-in was written nowhere, so an error inside one is
/// left pointing at the template alone.
MarkupError Inside(MarkupError error, const Scope &scope, const Walk &walk)
{
    if (error.file.empty())
    {
        error.file = scope.used->file;
    }
    if (!scope.standIn)
    {
        // The instance was written in the file around it, which may not be the
        // one the error is in.
        const std::string_view writtenIn = walk.scopes.empty() ? walk.file : walk.scopes.back().used->file;
        std::string where;
        if (writtenIn != error.file)
        {
            where = " of " + (writtenIn.empty() ? std::string{"the screen"} : std::string{writtenIn});
        }
        error.message += " (inside template '" + std::string{scope.templateName} + "', used at line " +
                         std::to_string(scope.line) + " column " + std::to_string(scope.column) + where + ")";
    }
    return error;
}

/// The template root's attributes an instance leaves alone, as an element to
/// apply: every one the instance does not also write, since the instance's win.
MarkupElement TemplateBase(const MarkupElement &root, const MarkupElement &instance)
{
    MarkupElement base;
    base.name = root.name;
    base.line = root.line;
    base.column = root.column;
    for (const MarkupAttribute &attribute : root.attributes)
    {
        if (instance.Find(attribute.name) == nullptr)
        {
            base.attributes.push_back(attribute);
        }
    }
    return base;
}

/// The prefix the names inside @p instance are qualified with: its own name
/// where it has one, read where the instance sits, or the enclosing prefix.
std::expected<std::string, MarkupError> InstancePrefix(const Walk &walk, const MarkupElement &instance)
{
    const MarkupAttribute *const instanceName = instance.Find("name");
    if (instanceName == nullptr || instanceName->value.empty())
    {
        return std::string{CurrentPrefix(walk)};
    }
    const std::expected<Substituted, MarkupError> name =
        Substitute(walk, instanceName->value, instanceName->line, instanceName->column);
    if (!name)
    {
        return std::unexpected(name.error());
    }
    if (name->known && name->text.empty())
    {
        return std::string{CurrentPrefix(walk)};
    }
    // Unknown only in a template's own check, where the raw text, which holds
    // a mark no real name can, still keeps the names inside apart.
    return Qualify(walk, name->known ? name->text : instanceName->value);
}

/// Binds every parameter @p used declares into @p scope, from what @p instance
/// passes, read where the instance sits, or from the declared default.
std::expected<void, MarkupError> BindParameters(const Walk &walk, const MarkupElement &instance, const Template &used,
                                                Scope &scope)
{
    for (const ParameterDeclaration &declared : used.parameters)
    {
        Parameter bound;
        if (const MarkupAttribute *const passed = instance.Find(declared.name))
        {
            const std::expected<Substituted, MarkupError> value =
                Substitute(walk, passed->value, passed->line, passed->column);
            if (!value)
            {
                return std::unexpected(value.error());
            }
            bound.value = value->text;
            bound.resolveIn = value->sole != nullptr ? value->sole->resolveIn : std::string{CurrentPrefix(walk)};
            bound.known = value->known;
        }
        else if (declared.hasDefault)
        {
            bound.value = declared.fallback;
            bound.resolveIn = scope.prefix;
        }
        else if (scope.standIn)
        {
            bound.known = false;
        }
        else
        {
            return std::unexpected(At(instance, "template '" + instance.name + "' takes '" + declared.name +
                                                    "', which has no default, and this instance does not pass it."));
        }
        scope.parameters.emplace(declared.name, std::move(bound));
    }
    return {};
}

/// The template's part of an instance, compiled inside its scope: the root's
/// text unless the instance has its own, the root's attributes the instance
/// leaves alone in @p base, and the root's children.
std::expected<void, MarkupError> CompileTemplatePart(Walk &walk, const MarkupElement &instance,
                                                     const MarkupElement &base, uint32_t index)
{
    const MarkupElement &root = *walk.scopes.back().used->root;
    if (instance.text.empty())
    {
        const std::expected<Substituted, MarkupError> rootText = Substitute(walk, root.text, root.line, root.column);
        if (!rootText)
        {
            return std::unexpected(rootText.error());
        }
        walk.document.nodes[index].text = rootText->known ? rootText->text : std::string{};
    }
    if (const std::expected<void, MarkupError> attributes = ApplyAttributes(walk, base, index); !attributes)
    {
        return attributes;
    }
    return CompileChildren(walk, root, index);
}

} // namespace

std::string_view CurrentPrefix(const Walk &walk)
{
    return walk.scopes.empty() ? std::string_view{} : std::string_view{walk.scopes.back().prefix};
}

std::string QualifyIn(std::string_view prefix, std::string_view name)
{
    if (prefix.empty())
    {
        return std::string{name};
    }
    return std::string{prefix} + kNameSeparator + std::string{name};
}

bool IsParameterName(std::string_view name)
{
    return !name.empty() && IsParameterStart(name.front()) && std::ranges::all_of(name, IsParameterChar);
}

std::expected<Substituted, MarkupError> Substitute(const Walk &walk, std::string_view text, uint32_t line,
                                                   uint32_t column)
{
    Substituted out;
    if (walk.scopes.empty())
    {
        out.text = text;
        return out;
    }
    const Scope &scope = walk.scopes.back();
    bool whole = false;
    std::size_t index = 0;
    while (index < text.size())
    {
        if (text[index] != kParameterMark)
        {
            out.text += text[index];
            ++index;
            continue;
        }
        if (index + 1 < text.size() && text[index + 1] == kParameterMark)
        {
            out.text += kParameterMark;
            index += 2;
            continue;
        }
        std::size_t end = index + 1;
        if (end < text.size() && IsParameterStart(text[end]))
        {
            while (end < text.size() && IsParameterChar(text[end]))
            {
                ++end;
            }
        }
        if (end == index + 1)
        {
            return std::unexpected(MarkupError{.message = "'" + std::string{text} + "' holds a '" + kParameterMark +
                                                          "' that begins no parameter name. Inside a template, " +
                                                          "a literal one is written '" + kParameterMark +
                                                          kParameterMark + "'.",
                                               .file = {},
                                               .line = line,
                                               .column = column});
        }
        const std::string name{text.substr(index + 1, end - index - 1)};
        const std::unordered_map<std::string, Parameter>::const_iterator found = scope.parameters.find(name);
        if (found == scope.parameters.end())
        {
            return std::unexpected(MarkupError{.message = "template '" + std::string{scope.templateName} +
                                                          "' declares no parameter '" + name +
                                                          "'. A template lists what it takes in params=\"...\".",
                                               .file = {},
                                               .line = line,
                                               .column = column});
        }
        out.text += found->second.value;
        out.known = out.known && found->second.known;
        out.sole = &found->second;
        whole = index == 0 && end == text.size();
        index = end;
    }
    if (!whole)
    {
        out.sole = nullptr;
    }
    return out;
}

std::expected<void, MarkupError> ApplyAttributes(Walk &walk, const MarkupElement &element, uint32_t index)
{
    const bool isRoot = index == 0;
    ScreenNode &node = walk.document.nodes[index];

    for (const MarkupAttribute &written : element.attributes)
    {
        if (written.name == "on_click")
        {
            if (isRoot)
            {
                return std::unexpected(At(written, "the screen itself cannot be clicked; put on_click on a "
                                                   "control inside it."));
            }
            if (node.widget != BuiltinWidget::Button)
            {
                return std::unexpected(At(written, "only a button is clicked. Every other control answers a "
                                                   "press itself, and what it holds is read rather than "
                                                   "announced."));
            }
            if (const std::expected<void, MarkupError> action = ApplyAction(walk, index, written); !action)
            {
                return std::unexpected(action.error());
            }
            continue;
        }

        const std::expected<Substituted, MarkupError> substituted =
            Substitute(walk, written.value, written.line, written.column);
        if (!substituted)
        {
            return std::unexpected(substituted.error());
        }
        // Checked per instance, once the value is known.
        if (!substituted->known)
        {
            continue;
        }
        MarkupAttribute attribute = written;
        attribute.value = substituted->text;

        // Read here rather than through the style table, which only says a
        // value was bad: a colour's likeliest mistakes each have one fix to name.
        if (Color *const colour = ColorField(node.style, attribute.name))
        {
            const ParsedColor read = ParseColor(attribute.value);
            if (!read)
            {
                return std::unexpected(At(attribute, read.error()));
            }
            *colour = *read;
            continue;
        }

        if (attribute.name == "pattern" && node.widget == BuiltinWidget::TextField)
        {
            if (const std::expected<void, MarkupError> pattern = ApplyPattern(node, attribute); !pattern)
            {
                return std::unexpected(pattern.error());
            }
            continue;
        }

        // `scroll_bars` and `axes` write the same field, and a scroll spells it
        // `axes` after the argument its own call takes. Two spellings for one
        // thing is how a file comes to say two different things at once.
        if (attribute.name == "scroll_bars" && node.widget == BuiltinWidget::Scroll)
        {
            return std::unexpected(At(attribute, "a scroll says which axes it scrolls with 'axes', not "
                                                 "'scroll_bars'."));
        }

        // The size keywords a length replaced, each with one fix to name.
        if (attribute.name == "width" || attribute.name == "height")
        {
            const std::vector<std::string_view> words = SplitWords(attribute.value);
            if (!words.empty() && (words[0] == kFixedWord || words[0] == kPercentWord))
            {
                const std::string example = words[0] == kFixedWord ? "420" : "50%";
                return std::unexpected(At(attribute, "'" + std::string{words[0]} +
                                                         "' is not a size: write the length " + "itself, such as " +
                                                         attribute.name + "=\"" + example +
                                                         "\". A percentage is written with %, from 0 to 100."));
            }
        }

        if (attribute.name == "name")
        {
            // A name written in the file would say again what its path says,
            // and the two drift the moment the file is renamed.
            if (isRoot)
            {
                return std::unexpected(At(attribute, "a screen is found by the path it is loaded from, such as "
                                                     "\"ui/Pause.amdn\", and names itself nowhere. Remove 'name'."));
            }
            if (const std::expected<void, MarkupError> named = ApplyName(walk, attribute, index); !named)
            {
                return std::unexpected(named.error());
            }
            continue;
        }

        if (attribute.name == "focus")
        {
            const std::optional<bool> focused = ParseBool(attribute.value);
            if (!focused)
            {
                return std::unexpected(At(attribute, "focus is written true or false."));
            }
            if (*focused)
            {
                if (walk.focusClaimed)
                {
                    return std::unexpected(At(attribute, "a second node asks for focus; a screen starts with "
                                                         "the keys on one node."));
                }
                walk.focusClaimed = true;
                walk.document.focus = index;
            }
            continue;
        }

        Applied applied =
            isRoot ? ApplyScreenAttribute(walk.document, attribute.name, attribute.value) : Applied::Unknown;
        if (applied == Applied::Unknown)
        {
            applied = ApplyWidgetAttribute(node, attribute.name, attribute.value);
        }
        if (applied == Applied::Unknown)
        {
            applied = ApplyNodeAttribute(node, attribute.name, attribute.value);
        }
        if (applied == Applied::Unknown)
        {
            applied = ApplyStyleAttribute(node.style, attribute.name, attribute.value);
        }

        if (applied == Applied::BadValue)
        {
            return std::unexpected(
                At(attribute, "'" + attribute.value + "' is not a value '" + attribute.name + "' can hold."));
        }
        if (applied == Applied::Unknown)
        {
            return std::unexpected(At(attribute, "'" + attribute.name +
                                                     "' is not an attribute this markup "
                                                     "has."));
        }
    }
    return {};
}

/// A node of the template's root kind, the template's attributes and children
/// compiled inside the instance's scope, then the instance's own over them in
/// the scope around it.
///
/// Scoped so that what the template writes means the template's parts — its
/// names and targets carry the instance's name — while what the instance
/// writes means what it does where the instance sits.
std::expected<void, MarkupError> CompileInstance(Walk &walk, const MarkupElement &instance, const Template &used,
                                                 uint32_t parent, bool standIn)
{
    if (walk.scopes.size() >= kMaxTemplateNesting)
    {
        return std::unexpected(At(instance, "templates nest more than " + std::to_string(kMaxTemplateNesting) +
                                                " deep here. Flatten one of them."));
    }

    const MarkupElement &root = *used.root;
    // A template's root is a built-in element; CollectTemplates holds it to that.
    const ElementKind &kind = *FindElement(root.name);
    if (!kind.carriesText && !instance.text.empty())
    {
        return std::unexpected(At(instance, "'" + instance.name + "' is a " + std::string{kind.name} +
                                                ", which carries no text. Put the words in a <text>."));
    }
    if (!kind.carriesChildren && !instance.children.empty())
    {
        return std::unexpected(
            At(instance, "'" + instance.name + "' is a " + std::string{kind.name} + ", which holds no elements."));
    }

    ScreenNode node;
    node.parent = parent;
    node.widget = kind.widget;
    Seed(node);
    node.style.direction = kind.direction;
    const uint32_t index = static_cast<uint32_t>(walk.document.nodes.size());
    walk.document.nodes.push_back(std::move(node));

    // What the instance writes is read where the instance sits, before its
    // own scope opens. The instance's words replace the template's.
    if (!instance.text.empty())
    {
        const std::expected<Substituted, MarkupError> ownText =
            Substitute(walk, instance.text, instance.line, instance.column);
        if (!ownText)
        {
            return std::unexpected(ownText.error());
        }
        walk.document.nodes[index].text = ownText->known ? ownText->text : std::string{};
    }
    const std::expected<std::string, MarkupError> prefix = InstancePrefix(walk, instance);
    if (!prefix)
    {
        return std::unexpected(prefix.error());
    }
    const MarkupAttribute *const instanceName = instance.Find("name");
    Scope scope{.parameters = {},
                .prefix = *prefix,
                .templateName = instance.name,
                .used = &used,
                .line = instance.line,
                .column = instance.column,
                .named = instanceName != nullptr && !instanceName->value.empty(),
                .standIn = standIn};
    if (const std::expected<void, MarkupError> bound = BindParameters(walk, instance, used, scope); !bound)
    {
        return bound;
    }

    // The instance's attributes are each a parameter or an override of the
    // root's, never both: a parameter may not be named like an attribute.
    MarkupElement overrides;
    overrides.name = instance.name;
    overrides.line = instance.line;
    overrides.column = instance.column;
    for (const MarkupAttribute &attribute : instance.attributes)
    {
        if (std::ranges::find(used.parameters, attribute.name, &ParameterDeclaration::name) == used.parameters.end())
        {
            overrides.attributes.push_back(attribute);
        }
    }

    const MarkupElement base = TemplateBase(root, overrides);
    walk.scopes.push_back(std::move(scope));
    const std::expected<void, MarkupError> inside = CompileTemplatePart(walk, instance, base, index);
    const Scope left = std::move(walk.scopes.back());
    walk.scopes.pop_back();
    if (!inside)
    {
        return std::unexpected(Inside(inside.error(), left, walk));
    }

    if (const std::expected<void, MarkupError> own = ApplyAttributes(walk, overrides, index); !own)
    {
        return std::unexpected(own.error());
    }
    const bool stepWritten = base.Find("step") != nullptr || overrides.Find("step") != nullptr;
    if (const std::expected<void, MarkupError> required = Required(instance, walk.document.nodes[index], stepWritten);
        !required)
    {
        return std::unexpected(required.error());
    }
    // After the template's, so an instance adds to what the template holds.
    return CompileChildren(walk, instance, index);
}

std::expected<void, MarkupError> CompileElement(Walk &walk, const MarkupElement &element, uint32_t parent)
{
    if (element.name == kTemplateElement)
    {
        return std::unexpected(At(element, "a template is declared directly inside the <" +
                                               std::string{kScreenElement} + "> or <" + std::string{kTemplatesElement} +
                                               ">, and nowhere else."));
    }
    if (element.name == kImportElement)
    {
        return std::unexpected(At(element, "an import sits directly inside the <" + std::string{kScreenElement} +
                                               "> or <" + std::string{kTemplatesElement} + ">, and nowhere else."));
    }
    if (element.name == kTemplatesElement)
    {
        return std::unexpected(
            At(element, "<" + std::string{kTemplatesElement} + "> is the root of a template library, a " +
                            std::string{kLibraryExtension} + " file of its own, which a screen imports."));
    }
    const ElementKind *const kind = FindElement(element.name);
    if (kind == nullptr)
    {
        const Namespace &here = Here(walk);
        const Namespace::const_iterator used = here.find(element.name);
        if (used != here.end())
        {
            return CompileInstance(walk, element, *used->second, parent, false);
        }
        std::string message = "'" + element.name + "' is not an element this markup has. It has: " + KnownElements();
        message += here.empty() ? "." : "; and the templates here: " + KnownTemplates(here) + ".";
        return std::unexpected(At(element, std::move(message)));
    }
    if (!kind->carriesText && !element.text.empty())
    {
        return std::unexpected(At(element, "'" + element.name +
                                               "' holds text, which it does not carry. Put the "
                                               "words in a <text>."));
    }
    if (!kind->carriesChildren && !element.children.empty())
    {
        return std::unexpected(At(element, "'" + element.name + "' holds elements, and holds none."));
    }

    const std::expected<Substituted, MarkupError> text = Substitute(walk, element.text, element.line, element.column);
    if (!text)
    {
        return std::unexpected(text.error());
    }

    ScreenNode node;
    node.parent = parent;
    node.widget = kind->widget;
    node.text = text->known ? text->text : std::string{};
    // Seeded first, because a control's own look is what a file's attributes
    // are written over.
    Seed(node);
    node.style.direction = kind->direction;

    const uint32_t index = static_cast<uint32_t>(walk.document.nodes.size());
    walk.document.nodes.push_back(std::move(node));

    if (const std::expected<void, MarkupError> attributes = ApplyAttributes(walk, element, index); !attributes)
    {
        return std::unexpected(attributes.error());
    }
    if (const std::expected<void, MarkupError> required =
            Required(element, walk.document.nodes[index], element.Find("step") != nullptr);
        !required)
    {
        return std::unexpected(required.error());
    }
    return CompileChildren(walk, element, index);
}

} // namespace Assisi::Mondrian::Import
