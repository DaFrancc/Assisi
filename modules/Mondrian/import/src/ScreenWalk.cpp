/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "ScreenWalk.hpp"

#include "MarkupValues.hpp"
#include "ScreenAttributes.hpp"
#include "ThemeTokens.hpp"

#include <Assisi/Mondrian/Import/ScreenCompiler.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

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
                    .line = scope.line,
                    .column = scope.column});
            }
            return std::unexpected(At(attribute, "'" + name + "' is already the name of a node on this screen (line " +
                                                     std::to_string(first->second.line) + "). A name means one node."));
        }
        walk.names.emplace(name, NamedNode{.index = index, .line = attribute.line, .column = attribute.column});
    }
    walk.document.nodes[index].name = Core::InternedString{name};
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

/// Where an error about a node's text points.
struct TextPlace
{
    /// "text" or "placeholder", for the message.
    std::string_view slot;
    uint32_t line = 1;
    uint32_t column = 1;
};

MarkupError At(const TextPlace &where, std::string message)
{
    return MarkupError{.message = std::move(message), .line = where.line, .column = where.column};
}

/// What a node's text turned out to be.
enum class TextIs : uint8_t
{
    Written, ///< shown as it is
    Key,     ///< a key into a string table
    Bound,   ///< holding bindings, kept as written for the loader
    Count_
};

/// @p source as a file writes it, for a message.
std::string Written(const BindingSource &source)
{
    std::string written{kBindingOpen};
    if (source.twoWay)
    {
        written += kTwoWayMark;
    }
    if (!source.context.empty())
    {
        written += source.context + kBindingSeparator;
    }
    written += source.component + kFieldSeparator + source.field;
    if (!source.format.empty())
    {
        written += kBindingSeparator + source.format;
    }
    return written + kBindingClose;
}

/// What @p name is among the types this program reflects.
BindingOwner OwnerOf(const ScreenRules &rules, std::string_view name)
{
    return {.component = rules.components ? rules.components(name) : nullptr,
            .structure = rules.structs ? rules.structs(name) : nullptr};
}

/// Whether the words @p binding shows can reach a player: the table it names
/// has an entry for every value, and a project requiring keys is given one.
std::expected<void, std::string> CheckWords(const Walk &walk, const BindingSource &source, const FieldBinding &binding)
{
    if (binding.use == BindingUse::Text && ShowsProgramWords(binding) &&
        walk.rules.literals == LiteralText::RequiresKey && !walk.document.debugOnly)
    {
        return std::unexpected("'" + Written(source) +
                               "' shows a word from the code, and this project requires text a player reads to come "
                               "from a string table. Give it a key, such as {" +
                               source.component + kFieldSeparator + source.field + ":#table:key}.");
    }
    for (const std::string &label : LabelsOf(binding))
    {
        // Each is the binding's key, which ResolveBinding split, and a name.
        const std::optional<TableKey> split = SplitKey(label);
        if (!split)
        {
            continue;
        }
        const std::unordered_map<std::string, StringTable>::const_iterator table =
            walk.rules.tables.byName.find(std::string{split->table});
        if (table == walk.rules.tables.byName.end())
        {
            return std::unexpected("'" + Written(source) + "' names the string table '" + std::string{split->table} +
                                   "', and there is none. List its file in " + std::string{kUiConfigPath} + ".");
        }
        if (!table->second.entries.contains(std::string{split->key}))
        {
            return std::unexpected("'" + Written(source) + "' needs the entry '" + std::string{split->key} +
                                   "' in the string table '" + std::string{split->table} + "', and it has none.");
        }
    }
    return {};
}

/// Whether @p source names a field of a component or struct this program has
/// that @p use can take, from a context the screen declares.
std::expected<void, std::string> CheckBinding(const Walk &walk, const BindingSource &source, BindingUse use)
{
    if (!source.context.empty() && !walk.checkingAlone &&
        !std::ranges::contains(walk.document.contexts, Core::InternedString{source.context}, &ScreenContext::name))
    {
        return std::unexpected("'" + Written(source) + "' reads the context '" + source.context +
                               "', which this screen does not declare. Declare it with <context name=\"" +
                               source.context + "\" />.");
    }
    const std::expected<FieldBinding, BindingError> resolved =
        ResolveBinding(source, OwnerOf(walk.rules, source.component), use);
    if (resolved)
    {
        return CheckWords(walk, source, *resolved);
    }
    switch (resolved.error())
    {
    case BindingError::UnknownComponent:
        return std::unexpected("'" + Written(source) + "' names '" + source.component +
                               "', which is not a component or struct this build reflects.");
    case BindingError::UnknownField:
        return std::unexpected("'" + Written(source) + "' names '" + source.field + "', which " + source.component +
                               " does not have.");
    default:
        return std::unexpected("'" + Written(source) + "' " + std::string{ToString(resolved.error())} + ".");
    }
}

/// Whether @p words, outside every binding, say nothing a player reads.
bool IsBlank(std::string_view words)
{
    return std::ranges::all_of(words, [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; });
}

/// The braces in @p text: its bindings checked when it holds any, and each
/// `{{` and `}}` made one brace when it holds none. A binding is refused where
/// @p bindable is false, which is everywhere but a text's or a button's words.
std::expected<TextIs, MarkupError> ResolveBraces(const Walk &walk, std::string &text, const TextPlace &where,
                                                 bool bindable)
{
    const std::expected<std::vector<TextRun>, BindingError> runs = SplitBoundText(text);
    if (!runs)
    {
        return std::unexpected(At(where, "the " + std::string{where.slot} + " '" + text + "' " +
                                             std::string{ToString(runs.error())} + "."));
    }
    if (std::ranges::none_of(*runs, &TextRun::bound))
    {
        text = runs->front().words;
        return TextIs::Written;
    }
    if (!bindable)
    {
        return std::unexpected(At(where, "the " + std::string{where.slot} + " '" + text +
                                             "' holds a binding, and only a text's words and a button's label are "
                                             "filled from the world. A { shown as it is is written {{."));
    }
    const bool keysRequired = walk.rules.literals == LiteralText::RequiresKey && !walk.document.debugOnly;
    for (const TextRun &run : *runs)
    {
        if (keysRequired && !IsBlank(run.words))
        {
            return std::unexpected(At(where, "the text '" + text +
                                                 "' has words around its bindings, and this project requires text "
                                                 "a player reads to come from a string table. Give the words a text "
                                                 "of their own with a key."));
        }
        if (!run.bound)
        {
            continue;
        }
        if (const std::expected<void, std::string> checked = CheckBinding(walk, run.binding, BindingUse::Text);
            !checked)
        {
            return std::unexpected(At(where, checked.error()));
        }
    }
    return TextIs::Bound;
}

/// Reads @p text as a key, as bound text, or as literal text, leaving it as
/// the key without its mark, as written, or as the text it stands for.
/// @p bindable is whether a binding may be in it.
std::expected<TextIs, MarkupError> ResolveText(const Walk &walk, std::string &text, const TextPlace &where,
                                               bool bindable)
{
    // Nothing written is nothing to translate, and a template checked on its
    // own leaves a parameter's text empty until an instance gives it one.
    if (text.empty())
    {
        return TextIs::Written;
    }
    if (text.size() > 1 && text[0] == kKeyMark && text[1] == kKeyMark)
    {
        text.erase(0, 1);
    }
    else if (text[0] == kKeyMark)
    {
        std::string qualified = text.substr(1);
        const std::optional<TableKey> split = SplitKey(qualified);
        if (!split)
        {
            return std::unexpected(At(where, "'" + text + "' names no string table. Write the table before the key, " +
                                                 "such as #pause:title. A # that is not a key is written ##."));
        }
        const std::unordered_map<std::string, StringTable>::const_iterator table =
            walk.rules.tables.byName.find(std::string{split->table});
        if (table == walk.rules.tables.byName.end())
        {
            return std::unexpected(At(where, "there is no string table '" + std::string{split->table} +
                                                 "'. List its file in " + std::string{kUiConfigPath} + "."));
        }
        if (!table->second.entries.contains(std::string{split->key}))
        {
            return std::unexpected(At(where, "the string table '" + std::string{split->table} + "' has no key '" +
                                                 std::string{split->key} + "'."));
        }
        text = std::move(qualified);
        return TextIs::Key;
    }
    if (text.find(kBindingOpen) != std::string::npos || text.find(kBindingClose) != std::string::npos)
    {
        const std::expected<TextIs, MarkupError> braced = ResolveBraces(walk, text, where, bindable);
        if (!braced || *braced == TextIs::Bound)
        {
            return braced;
        }
    }
    if (walk.rules.literals == LiteralText::RequiresKey && !walk.document.debugOnly)
    {
        return std::unexpected(At(where, "the " + std::string{where.slot} + " '" + text +
                                             "' is written as it is, and this project requires text a player reads "
                                             "to come from a string table. Write a key such as #pause:title, or mark "
                                             "the screen debug_only=\"true\" if players never see it."));
    }
    return TextIs::Written;
}

/// The attribute a bool binding can drive.
constexpr std::string_view kVisibleAttribute = "visible";

/// The attribute holding what a toggle shows, and what a slider does.
constexpr std::string_view kToggleValueAttribute = "on";
constexpr std::string_view kSliderValueAttribute = "value";

/// Whether @p name is the attribute saying what @p widget holds.
bool IsValueAttribute(BuiltinWidget widget, std::string_view name)
{
    return ControlUse(widget).has_value() &&
           name == (widget == BuiltinWidget::Toggle ? kToggleValueAttribute : kSliderValueAttribute);
}

/// @p attribute's value, which is one binding @p use can take, kept as written.
std::expected<Core::PooledString, MarkupError> KeepBinding(Walk &walk, const MarkupAttribute &attribute, BindingUse use)
{
    const std::expected<BindingSource, BindingError> source = ReadSoleBinding(attribute.value);
    if (!source)
    {
        return std::unexpected(
            At(attribute, "'" + attribute.value + "' " + std::string{ToString(source.error())} + "."));
    }
    if (const std::expected<void, std::string> checked = CheckBinding(walk, *source, use); !checked)
    {
        return std::unexpected(At(attribute, checked.error()));
    }
    return walk.document.AddText(attribute.value);
}

/// `visible="{Component.field}"`: shown while that bool holds.
std::expected<void, MarkupError> ApplyVisibleBinding(Walk &walk, ScreenNode &node, const MarkupAttribute &attribute)
{
    const std::expected<Core::PooledString, MarkupError> kept = KeepBinding(walk, attribute, BindingUse::Bool);
    if (!kept)
    {
        return std::unexpected(kept.error());
    }
    node.visibleBinding = *kept;
    return {};
}

/// `value="{=Component.field}"`: what the control holds follows that field.
std::expected<void, MarkupError> ApplyValueBinding(Walk &walk, ScreenNode &node, const MarkupAttribute &attribute)
{
    // Whether this attribute holds the node's value was asked before coming here.
    const std::expected<Core::PooledString, MarkupError> kept =
        KeepBinding(walk, attribute, ControlUse(node.widget).value_or(BindingUse::Count_));
    if (!kept)
    {
        return std::unexpected(kept.error());
    }
    node.valueBinding = *kept;
    return {};
}

/// Whether what a stepped slider says about its steps suits the field it is
/// bound to: an enum decides them, and a whole number leaves them to the file.
/// Asked once everything the node writes has settled, so it does not matter
/// which of a template and its instance wrote them.
std::expected<void, MarkupError> CheckBoundSteps(const Walk &walk, const MarkupElement &element, const ScreenNode &node)
{
    const std::string_view written = walk.document.Text(node.valueBinding);
    if (node.widget != BuiltinWidget::SteppedSlider || written.empty())
    {
        return {};
    }
    // A binding that does not read was refused when it was kept.
    const std::expected<BindingSource, BindingError> source = ReadSoleBinding(written);
    if (!source)
    {
        return {};
    }
    const std::expected<FieldBinding, BindingError> resolved =
        ResolveBinding(*source, OwnerOf(walk.rules, source->component), BindingUse::Stepped);
    if (!resolved)
    {
        return {};
    }
    const ScreenNode unwritten;
    if (resolved->field->type == Core::Reflect::FieldType::Enum)
    {
        if (node.steps != unwritten.steps || node.range != unwritten.range)
        {
            return std::unexpected(At(element, "a stepped slider bound to an enum has a step for each enumerator, so "
                                      "it writes no steps, min or max."));
        }
        return {};
    }
    // A template checked alone may leave them to a parameter it has no value
    // for yet.
    if (node.steps <= unwritten.steps && !walk.checkingAlone)
    {
        return std::unexpected(At(element, "a stepped slider bound to a whole number says how many positions it has, "
                                  "with steps."));
    }
    return {};
}

/// Resolves node @p index's text and placeholder, each once, now that every
/// attribute and parameter has settled what they say. @p textFrom and
/// @p placeholderFrom are the elements that wrote each: the node's own, or the
/// root of the template it is an instance of.
std::expected<void, MarkupError> ResolveNodeText(Walk &walk, uint32_t index, const MarkupElement &textFrom,
                                                 const MarkupElement &placeholderFrom)
{
    ScreenNode &node = walk.document.nodes[index];
    std::string words{walk.document.Text(node.text)};
    // A field's text is the player's, so only plain text and a label are
    // filled from the world.
    const bool bindable = node.widget == BuiltinWidget::None || node.widget == BuiltinWidget::Button;
    const std::expected<TextIs, MarkupError> text = ResolveText(
        walk, words, TextPlace{.slot = "text", .line = textFrom.line, .column = textFrom.column}, bindable);
    if (!text)
    {
        return std::unexpected(text.error());
    }
    node.text = walk.document.AddText(words);
    node.textIsKey = *text == TextIs::Key;
    node.textIsBound = *text == TextIs::Bound;

    // At the attribute where one was written, and at the element otherwise.
    TextPlace placeAt{.slot = "placeholder", .line = placeholderFrom.line, .column = placeholderFrom.column};
    if (const MarkupAttribute *const written = placeholderFrom.Find("placeholder"); written != nullptr)
    {
        placeAt.line = written->line;
        placeAt.column = written->column;
    }
    std::string shown{walk.document.Text(node.placeholder)};
    const std::expected<TextIs, MarkupError> placeholder = ResolveText(walk, shown, placeAt, false);
    if (!placeholder)
    {
        return std::unexpected(placeholder.error());
    }
    node.placeholder = walk.document.AddText(shown);
    node.placeholderIsKey = *placeholder == TextIs::Key;
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
        walk.document.nodes[index].text = walk.document.AddText(rootText->known ? rootText->text : std::string{});
    }
    if (const std::expected<void, MarkupError> attributes = ApplyAttributes(walk, base, index); !attributes)
    {
        return attributes;
    }
    return CompileChildren(walk, root, index);
}

/// `on_click`, which only a button carries.
std::expected<void, MarkupError> ApplyOnClick(Walk &walk, uint32_t index, const MarkupAttribute &written)
{
    if (index == 0)
    {
        return std::unexpected(At(written, "the screen itself cannot be clicked; put on_click on a "
                                           "control inside it."));
    }
    if (walk.document.nodes[index].widget != BuiltinWidget::Button)
    {
        return std::unexpected(At(written, "only a button is clicked. Every other control answers a "
                                           "press itself, and what it holds is read rather than "
                                           "announced."));
    }
    return ApplyAction(walk, index, written);
}

/// Refuses the spellings an attribute had before, or has on another element,
/// each with the one fix to name.
std::expected<void, MarkupError> RefuseMisspelling(const ScreenNode &node, const MarkupAttribute &attribute)
{
    // `scroll_bars` and `axes` write the same field, and a scroll spells it
    // `axes` after the argument its own call takes. Two spellings for one
    // thing is how a file comes to say two different things at once.
    if (attribute.name == "scroll_bars" && node.widget == BuiltinWidget::Scroll)
    {
        return std::unexpected(At(attribute, "a scroll says which axes it scrolls with 'axes', not "
                                             "'scroll_bars'."));
    }

    // The size keywords a length replaced.
    if (attribute.name == "width" || attribute.name == "height")
    {
        const std::vector<std::string_view> words = SplitWords(attribute.value);
        if (!words.empty() && (words[0] == kFixedWord || words[0] == kPercentWord))
        {
            const std::string example = words[0] == kFixedWord ? "420" : "50%";
            return std::unexpected(At(attribute, "'" + std::string{words[0]} + "' is not a size: write the length " +
                                                     "itself, such as " + attribute.name + "=\"" + example +
                                                     "\". A percentage is written with %, from 0 to 100."));
        }
    }
    return {};
}

/// `focus`, which one node on a screen may ask for.
std::expected<void, MarkupError> ApplyFocus(Walk &walk, uint32_t index, const MarkupAttribute &attribute)
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
    return {};
}

/// `style`, which names a style every theme the project lists must have.
std::expected<void, MarkupError> ApplyStyleName(const Walk &walk, ScreenNode &node, const MarkupAttribute &attribute)
{
    const std::string &name = attribute.value;
    if (name.empty())
    {
        node.styleName = {};
        return {};
    }
    if (walk.rules.themes.empty())
    {
        return std::unexpected(At(attribute, "the style '" + name + "' is named, and " + std::string{kUiConfigPath} +
                                                 " lists no theme to hold it. List one under \"themes\"."));
    }
    for (const ProjectTheme &theme : walk.rules.themes)
    {
        const ThemeDocument &document = theme.compiled.document;
        if (document.FindStyle(name) == nullptr)
        {
            std::string message = "the theme '" + theme.vpath + "' has no style '" + name + "'.";
            for (std::size_t style = 0; style < document.styles.size(); ++style)
            {
                message += style == 0 ? " Its styles are: " : ", ";
                message += document.styles[style].name.View();
            }
            message += document.styles.empty() ? "" : ".";
            return std::unexpected(At(attribute, std::move(message)));
        }
    }
    node.styleName = Core::InternedString{name};
    return {};
}

/// Refuses @p attribute giving a node a picture when it already names @p other
/// the other way: a node shows one.
std::expected<void, MarkupError> RefuseSecondPicture(const MarkupAttribute &attribute,
                                                     const Core::InternedString &other)
{
    if (!attribute.value.empty() && !other.Empty())
    {
        return std::unexpected(At(attribute, "a node shows one picture, named by a sprite or by a texture; this one "
                                             "already names '" +
                                                 std::string{other.View()} + "'."));
    }
    return {};
}

/// `sprite`, which a sheet the project lists must have.
std::expected<void, MarkupError> ApplySprite(const Walk &walk, ScreenNode &node, const MarkupAttribute &attribute)
{
    if (const std::expected<void, MarkupError> refused = RefuseSecondPicture(attribute, node.texture); !refused)
    {
        return refused;
    }
    if (!attribute.value.empty())
    {
        if (const std::expected<void, std::string> checked = CheckSprite(walk.rules, attribute.value); !checked)
        {
            return std::unexpected(At(attribute, "'sprite' " + checked.error()));
        }
    }
    node.sprite = Core::InternedString{attribute.value};
    return {};
}

/// `texture`, which must be an image the project has.
std::expected<void, MarkupError> ApplyTexture(const Walk &walk, ScreenNode &node, const MarkupAttribute &attribute)
{
    if (const std::expected<void, MarkupError> refused = RefuseSecondPicture(attribute, node.sprite); !refused)
    {
        return refused;
    }
    if (!attribute.value.empty())
    {
        if (!walk.rules.read)
        {
            return std::unexpected(At(attribute, "the image '" + attribute.value +
                                                     "' cannot be checked here: nothing reads the project's files."));
        }
        const std::expected<std::string, std::string> bytes = walk.rules.read(attribute.value);
        if (!bytes)
        {
            return std::unexpected(
                At(attribute, "the texture '" + attribute.value + "' could not be read (" + bytes.error() + ")."));
        }
        if (const std::expected<Extent, std::string> size = ImageSize(*bytes); !size)
        {
            return std::unexpected(At(attribute, "'" + attribute.value + "' " + size.error()));
        }
    }
    node.texture = Core::InternedString{attribute.value};
    return {};
}

/// An attribute the table leaves to this file, with its parameters replaced.
std::expected<void, MarkupError> ApplyHandled(Walk &walk, uint32_t index, HandledAttribute handled,
                                              const MarkupAttribute &attribute)
{
    ScreenNode &node = walk.document.nodes[index];
    switch (handled)
    {
    case HandledAttribute::Name:
        // A name written in the file would say again what its path says, and
        // the two drift the moment the file is renamed.
        if (index == 0)
        {
            return std::unexpected(At(attribute, "a screen is found by the path it is loaded from, such as "
                                                 "\"ui/Pause.amdn\", and names itself nowhere. Remove 'name'."));
        }
        return ApplyName(walk, attribute, index);
    case HandledAttribute::Focus:
        return ApplyFocus(walk, index, attribute);
    case HandledAttribute::Pattern:
        return ApplyPattern(walk.document, node, attribute);
    case HandledAttribute::StyleName:
        return ApplyStyleName(walk, node, attribute);
    case HandledAttribute::Sprite:
        return ApplySprite(walk, node, attribute);
    case HandledAttribute::Texture:
        return ApplyTexture(walk, node, attribute);
    case HandledAttribute::Background:
    case HandledAttribute::BorderColor:
    case HandledAttribute::TextColor:
    case HandledAttribute::Tint:
        return ApplyColor(node.style, handled, attribute);
    case HandledAttribute::Change:
        return ApplyOnChange(walk, node, attribute);
    case HandledAttribute::Action:
        // Read before its parameters are replaced, so never reaches here.
    case HandledAttribute::None:
    case HandledAttribute::Count_:
        break;
    }
    return {};
}

/// The look node @p node has in @p state, made empty if it has none yet.
ThemedPatch &LookIn(ScreenNode &node, StyleState state)
{
    std::vector<ThemedPatch>::iterator look =
        std::ranges::find_if(node.states, [state](const ThemedPatch &held) { return held.patch.state == state; });
    if (look == node.states.end())
    {
        ThemedPatch empty;
        empty.patch.state = state;
        node.states.push_back(std::move(empty));
        look = std::prev(node.states.end());
    }
    return *look;
}

/// The use of a token @p attribute writes for @p property, once every theme
/// the project lists gives it a value that property can hold in every mode.
std::expected<TokenUse, MarkupError> UseToken(const Walk &walk, StyleProperty property,
                                              const MarkupAttribute &attribute)
{
    const std::expected<std::string_view, MarkupError> token = TokenNamed(attribute);
    if (!token)
    {
        return std::unexpected(token.error());
    }
    if (walk.rules.themes.empty())
    {
        return std::unexpected(At(attribute, "'" + attribute.value + "' is a token, and " + std::string{kUiConfigPath} +
                                                 " lists no theme to give it a value. List one under \"themes\"."));
    }
    for (const ProjectTheme &theme : walk.rules.themes)
    {
        if (const std::expected<void, MarkupError> checked =
                CheckToken(theme.compiled, "the theme '" + theme.vpath + "'", *token, property, attribute);
            !checked)
        {
            return std::unexpected(checked.error());
        }
    }
    return TokenUse{.token = Core::InternedString{*token}, .property = property};
}

/// @p attribute read into the look node @p index has in @p state, over what
/// that look already writes.
std::expected<void, MarkupError> ApplyLook(Walk &walk, uint32_t index, StyleState state, const AttributeSpec &spec,
                                           const MarkupAttribute &attribute)
{
    ThemedPatch &look = LookIn(walk.document.nodes[index], state);
    if (NamesToken(attribute.value))
    {
        const std::expected<TokenUse, MarkupError> use = UseToken(walk, *spec.property, attribute);
        if (!use)
        {
            return std::unexpected(use.error());
        }
        look.Bind(*use);
        return {};
    }
    Style written = look.patch.values;
    if (const std::expected<void, MarkupError> read = ApplyStyleAttribute(spec, attribute, written); !read)
    {
        return read;
    }
    look.Take(*spec.property, written);
    return {};
}

/// Records that node @p node writes @p property itself, with a value written
/// out, so neither its named style nor a token it took before fills it.
void WriteOwn(ScreenNode &node, StyleProperty property)
{
    node.written |= PropertyBit(property);
    std::erase_if(node.tokens, [property](const TokenUse &use) { return use.property == property; });
}


/// Whether the step @p element writes, if it writes one, has a value in the
/// scope open now. In a template's own check, one naming a parameter with no
/// default has none until an instance gives it one.
bool StepKnown(const Walk &walk, const MarkupElement &element)
{
    const MarkupAttribute *const step = element.Find("step");
    if (step == nullptr)
    {
        return true;
    }
    const std::expected<Substituted, MarkupError> value = Substitute(walk, step->value, step->line, step->column);
    return value.has_value() && value->known;
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
        const std::expected<std::optional<StyleState>, MarkupError> state = StateOf(written);
        if (!state)
        {
            return std::unexpected(state.error());
        }
        const std::string_view name = *state ? AfterPrefix(written.name) : std::string_view{written.name};
        const AttributeSpec *const spec = FindAttribute(name, node.widget, isRoot);
        if (spec == nullptr)
        {
            return std::unexpected(At(written, "'" + written.name + "' is not an attribute this markup has."));
        }
        if (*state && spec->handled == HandledAttribute::StyleName)
        {
            return std::unexpected(At(written, "a node names one style, which holds its own look for each state. "
                                               "Write the state on the style's attributes instead."));
        }
        if (*state && !spec->property)
        {
            return std::unexpected(At(written, "'" + std::string{name} +
                                                   "' says what the node is or does rather than how it looks, so it "
                                                   "cannot be given a state."));
        }

        // Read as written: which parts of a call came from parameters decides
        // where each target is resolved.
        if (spec->handled == HandledAttribute::Action)
        {
            if (const std::expected<void, MarkupError> action = ApplyOnClick(walk, index, written); !action)
            {
                return action;
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
        attribute.name = name;
        attribute.value = substituted->text;

        if (const std::expected<void, MarkupError> refused = RefuseMisspelling(node, attribute); !refused)
        {
            return refused;
        }
        if (attribute.name == kVisibleAttribute && attribute.value.find(kBindingOpen) != std::string::npos)
        {
            if (const std::expected<void, MarkupError> bound = ApplyVisibleBinding(walk, node, attribute); !bound)
            {
                return bound;
            }
            continue;
        }
        if (IsValueAttribute(node.widget, attribute.name) && attribute.value.find(kBindingOpen) != std::string::npos)
        {
            if (const std::expected<void, MarkupError> bound = ApplyValueBinding(walk, node, attribute); !bound)
            {
                return bound;
            }
            continue;
        }
        // Checked here, where the project's sheets are in hand, whether it is
        // the node's own look or a state's.
        if (spec->property == StyleProperty::BackgroundImage && !attribute.value.empty() &&
            !NamesToken(attribute.value))
        {
            if (const std::expected<void, std::string> checked = CheckSprite(walk.rules, attribute.value); !checked)
            {
                return std::unexpected(At(attribute, "'" + attribute.name + "' " + checked.error()));
            }
        }
        if (*state)
        {
            if (const std::expected<void, MarkupError> look = ApplyLook(walk, index, **state, *spec, attribute); !look)
            {
                return look;
            }
            continue;
        }
        if (spec->property && NamesToken(attribute.value))
        {
            const std::expected<TokenUse, MarkupError> use = UseToken(walk, *spec->property, attribute);
            if (!use)
            {
                return std::unexpected(use.error());
            }
            WriteOwn(node, use->property);
            node.tokens.push_back(*use);
            continue;
        }
        if (spec->handled != HandledAttribute::None)
        {
            if (const std::expected<void, MarkupError> handled = ApplyHandled(walk, index, spec->handled, attribute);
                !handled)
            {
                return handled;
            }
        }
        else if (!spec->apply(AttributeTarget{.document = walk.document, .node = node, .style = node.style},
                              attribute.value))
        {
            return std::unexpected(At(attribute, "'" + attribute.value + "' is not a value '" + attribute.name +
                                                     "' can hold." + (NamesToken(attribute.value)
                                                                          ? " A token gives only how a node looks."
                                                                          : "")));
        }
        if (spec->property)
        {
            WriteOwn(node, *spec->property);
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
    if (kind.namesDirection)
    {
        node.written |= PropertyBit(StyleProperty::Direction);
    }
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
        walk.document.nodes[index].text = walk.document.AddText(ownText->known ? ownText->text : std::string{});
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
    // Read while the template's scope is open, which is where its root's step
    // takes its value.
    const bool baseStepKnown = StepKnown(walk, base);
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
    const bool stepKnown = baseStepKnown && StepKnown(walk, overrides);
    if (const std::expected<void, MarkupError> required =
            Required(instance, walk.document.nodes[index], stepWritten, stepKnown);
        !required)
    {
        return std::unexpected(required.error());
    }
    if (const std::expected<void, MarkupError> steps = CheckBoundSteps(walk, instance, walk.document.nodes[index]);
        !steps)
    {
        return std::unexpected(steps.error());
    }
    // Once the template's text and the instance's have settled which wins, and
    // pointing at whichever wrote it.
    const MarkupElement &textFrom = instance.text.empty() ? root : instance;
    const MarkupElement &placeholderFrom = overrides.Find("placeholder") != nullptr ? instance : root;
    if (const std::expected<void, MarkupError> text = ResolveNodeText(walk, index, textFrom, placeholderFrom); !text)
    {
        return text;
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
    if (element.name == kContextElement)
    {
        return std::unexpected(At(element, "a context is declared directly inside the <" +
                                               std::string{kScreenElement} + ">, and nowhere else."));
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
    node.text = walk.document.AddText(text->known ? text->text : std::string{});
    // Seeded first, because a control's own look is what a file's attributes
    // are written over.
    Seed(node);
    node.style.direction = kind->direction;
    if (kind->namesDirection)
    {
        node.written |= PropertyBit(StyleProperty::Direction);
    }

    const uint32_t index = static_cast<uint32_t>(walk.document.nodes.size());
    walk.document.nodes.push_back(std::move(node));

    if (const std::expected<void, MarkupError> attributes = ApplyAttributes(walk, element, index); !attributes)
    {
        return std::unexpected(attributes.error());
    }
    if (const std::expected<void, MarkupError> required =
            Required(element, walk.document.nodes[index], element.Find("step") != nullptr, StepKnown(walk, element));
        !required)
    {
        return std::unexpected(required.error());
    }
    if (const std::expected<void, MarkupError> steps = CheckBoundSteps(walk, element, walk.document.nodes[index]);
        !steps)
    {
        return std::unexpected(steps.error());
    }
    if (const std::expected<void, MarkupError> resolved = ResolveNodeText(walk, index, element, element); !resolved)
    {
        return resolved;
    }
    return CompileChildren(walk, element, index);
}

} // namespace Assisi::Mondrian::Import
