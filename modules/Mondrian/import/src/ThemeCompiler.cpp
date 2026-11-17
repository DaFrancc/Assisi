/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Mondrian/Import/ThemeCompiler.hpp>

#include "ScreenAttributes.hpp"
#include "ScreenWalk.hpp"
#include "ThemeTokens.hpp"

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>

namespace Assisi::Mondrian::Import
{
namespace
{

constexpr std::string_view kThemeElement = "theme";
constexpr std::string_view kPaletteElement = "palette";
constexpr std::string_view kStyleElement = "style";
constexpr std::string_view kModeAttribute = "mode";
constexpr std::string_view kNameAttribute = "name";
constexpr std::string_view kExtendsAttribute = "extends";

/// @p value read into every property that can hold it, and written for those.
StylePatch ReadIntoEvery(std::string_view value)
{
    StylePatch patch;
    for (uint32_t index = 0; index < static_cast<uint32_t>(StyleProperty::Count_); ++index)
    {
        const StyleProperty property = static_cast<StyleProperty>(index);
        const AttributeSpec &spec = AttributeFor(property);
        MarkupAttribute attribute;
        attribute.name = spec.name;
        attribute.value = value;
        Style read;
        if (ApplyStyleAttribute(spec, attribute, read).has_value())
        {
            patch.Take(property, read);
        }
    }
    return patch;
}

std::expected<void, MarkupError> RefuseContent(const MarkupElement &element, std::string_view holds)
{
    if (!element.text.empty() || !element.children.empty())
    {
        return std::unexpected(At(element, "a <" + element.name + "> holds nothing; " + std::string{holds} +
                                               " are written as its attributes."));
    }
    return {};
}

/// Each palette's mode, refusing a palette with none and a second for one mode.
std::expected<void, MarkupError> ReadModes(std::span<const MarkupElement *const> palettes, ThemeDocument &document)
{
    for (const MarkupElement *palette : palettes)
    {
        if (const std::expected<void, MarkupError> empty = RefuseContent(*palette, "its tokens"); !empty)
        {
            return empty;
        }
        const MarkupAttribute *mode = palette->Find(kModeAttribute);
        if (mode == nullptr || mode->value.empty())
        {
            return std::unexpected(At(*palette, "a palette says which mode it gives values for, with mode=\"...\"."));
        }
        const std::optional<uint32_t> taken = document.FindMode(mode->value);
        if (taken)
        {
            return std::unexpected(At(*mode, "a second palette for mode '" + mode->value +
                                                  "'; the first is at line " +
                                                  std::to_string(palettes[*taken]->line) + "."));
        }
        document.modes.emplace_back(mode->value);
    }
    return {};
}

/// Every token's name, in the order the palettes first give one, refusing a
/// name no token can have and a value naming another token.
std::expected<std::vector<std::string>, MarkupError> ReadTokenNames(std::span<const MarkupElement *const> palettes)
{
    std::vector<std::string> names;
    for (const MarkupElement *palette : palettes)
    {
        for (const MarkupAttribute &attribute : palette->attributes)
        {
            if (attribute.name == kModeAttribute)
            {
                continue;
            }
            if (!IsParameterName(attribute.name))
            {
                return std::unexpected(At(attribute, "'" + attribute.name +
                                                         "' is no token's name: a name is letters, digits and _, "
                                                         "not starting with a digit."));
            }
            if (NamesToken(attribute.value))
            {
                return std::unexpected(
                    At(attribute, "a token's value is written out in full; one token does not name another."));
            }
            if (std::ranges::find(names, attribute.name) == names.end())
            {
                names.push_back(attribute.name);
            }
        }
    }
    return names;
}

/// The palettes' tokens, each read for every mode, refusing a token one mode
/// gives and another does not.
std::expected<void, MarkupError> ReadPalettes(const MarkupElement &root,
                                              std::span<const MarkupElement *const> palettes, CompiledTheme &theme)
{
    if (palettes.empty())
    {
        return std::unexpected(At(root, "a theme gives its tokens values in at least one <palette mode=\"...\">, "
                                        "and this one has none."));
    }
    if (const std::expected<void, MarkupError> modes = ReadModes(palettes, theme.document); !modes)
    {
        return modes;
    }
    const std::expected<std::vector<std::string>, MarkupError> names = ReadTokenNames(palettes);
    if (!names)
    {
        return std::unexpected(names.error());
    }
    for (const std::string &name : *names)
    {
        const std::span<const MarkupElement *const>::iterator giver = std::ranges::find_if(
            palettes, [&name](const MarkupElement *palette) { return palette->Find(name) != nullptr; });
        ThemeToken token{.name = Core::InternedString{name}, .values = {}};
        std::vector<std::string> written;
        for (std::size_t mode = 0; mode < palettes.size(); ++mode)
        {
            const MarkupAttribute *value = palettes[mode]->Find(name);
            if (value == nullptr)
            {
                return std::unexpected(At(*palettes[mode], "mode '" + std::string{theme.document.modes[mode].View()} +
                                                               "' gives no value for '" + name + "', which mode '" +
                                                               (*giver)->Find(kModeAttribute)->value + "' (line " +
                                                               std::to_string((*giver)->line) +
                                                               ") gives. Every mode gives every token a value."));
            }
            token.values.push_back(ReadIntoEvery(value->value));
            written.push_back(value->value);
        }
        theme.document.tokens.push_back(std::move(token));
        theme.written.push_back(std::move(written));
    }
    return {};
}

/// The look @p style has in @p state, made empty if it has none yet.
ThemedPatch &LookIn(ThemeStyle &style, StyleState state)
{
    std::vector<ThemedPatch>::iterator look =
        std::ranges::find_if(style.states, [state](const ThemedPatch &held) { return held.patch.state == state; });
    if (look == style.states.end())
    {
        ThemedPatch empty;
        empty.patch.state = state;
        style.states.push_back(std::move(empty));
        look = std::prev(style.states.end());
    }
    return *look;
}

/// One attribute of a style, about how a node looks, read into @p style.
std::expected<void, MarkupError> ReadStyleAttribute(const MarkupAttribute &written, const CompiledTheme &theme,
                                                    ThemeStyle &style)
{
    const std::expected<std::optional<StyleState>, MarkupError> state = StateOf(written);
    if (!state)
    {
        return std::unexpected(state.error());
    }
    const std::string_view name = AfterPrefix(written.name);
    const AttributeSpec *const spec = FindAttribute(name, BuiltinWidget::None, false);
    if (spec == nullptr)
    {
        return std::unexpected(At(written, IsMarkupAttribute(name)
                                               ? "'" + std::string{name} +
                                                     "' is one kind of node's own, and a style holds what every node "
                                                     "has."
                                               : "'" + written.name + "' is not an attribute this markup has."));
    }
    if (!spec->property)
    {
        return std::unexpected(At(written, "'" + std::string{name} +
                                               "' says what a node is or does, and a style holds only how a node "
                                               "looks."));
    }

    MarkupAttribute attribute = written;
    attribute.name = name;
    ThemedPatch &look = *state ? LookIn(style, **state) : style.resting;
    if (NamesToken(attribute.value))
    {
        const std::expected<std::string_view, MarkupError> token = TokenNamed(attribute);
        if (!token)
        {
            return std::unexpected(token.error());
        }
        if (const std::expected<void, MarkupError> checked =
                CheckToken(theme, "this theme", *token, *spec->property, attribute);
            !checked)
        {
            return checked;
        }
        look.Bind(TokenUse{.token = Core::InternedString{*token}, .property = *spec->property});
        return {};
    }
    Style read = look.patch.values;
    if (const std::expected<void, MarkupError> applied = ApplyStyleAttribute(*spec, attribute, read); !applied)
    {
        return applied;
    }
    look.Take(*spec->property, read);
    return {};
}

/// A style as its element writes it, before what it extends is put under it.
struct WrittenStyle
{
    ThemeStyle own;
    const MarkupElement *element = nullptr;
    /// Null when it extends nothing.
    const MarkupAttribute *extends = nullptr;
};

std::expected<WrittenStyle, MarkupError> ReadStyle(const MarkupElement &element, const CompiledTheme &theme)
{
    if (const std::expected<void, MarkupError> empty = RefuseContent(element, "its looks"); !empty)
    {
        return std::unexpected(empty.error());
    }
    const MarkupAttribute *name = element.Find(kNameAttribute);
    if (name == nullptr || name->value.empty())
    {
        return std::unexpected(At(element, "a style is named, with name=\"...\", which is how a node asks for it."));
    }
    WrittenStyle style{.own = ThemeStyle{.name = Core::InternedString{name->value}, .resting = {}, .states = {}},
                       .element = &element,
                       .extends = element.Find(kExtendsAttribute)};
    if (style.extends != nullptr && style.extends->value.empty())
    {
        style.extends = nullptr;
    }
    for (const MarkupAttribute &attribute : element.attributes)
    {
        if (attribute.name == kNameAttribute || attribute.name == kExtendsAttribute)
        {
            continue;
        }
        if (const std::expected<void, MarkupError> read = ReadStyleAttribute(attribute, theme, style.own); !read)
        {
            return std::unexpected(read.error());
        }
    }
    return style;
}

/// @p own with @p base put under it: its looks, at rest and in each state,
/// with the base's where it writes nothing of its own.
ThemeStyle Extended(const ThemeStyle &base, const ThemeStyle &own)
{
    ThemeStyle style{.name = own.name, .resting = base.resting, .states = base.states};
    style.resting.Add(own.resting);
    for (const ThemedPatch &look : own.states)
    {
        LookIn(style, look.patch.state).Add(look);
    }
    return style;
}

/// Every style with what it extends put under it, in the order the file writes
/// them, refusing a base the theme does not declare and a chain that comes back
/// round to where it started.
std::expected<std::vector<ThemeStyle>, MarkupError> Flatten(const std::vector<WrittenStyle> &written)
{
    std::unordered_map<std::string_view, std::size_t> byName;
    for (std::size_t index = 0; index < written.size(); ++index)
    {
        byName.emplace(written[index].own.name.View(), index);
    }
    std::vector<std::optional<std::size_t>> bases(written.size());
    for (std::size_t index = 0; index < written.size(); ++index)
    {
        const MarkupAttribute *extends = written[index].extends;
        if (extends == nullptr)
        {
            continue;
        }
        const std::unordered_map<std::string_view, std::size_t>::const_iterator base = byName.find(extends->value);
        if (base == byName.end())
        {
            return std::unexpected(At(*extends, "'" + std::string{written[index].own.name.View()} + "' extends '" +
                                                    extends->value + "', which this theme does not declare."));
        }
        bases[index] = base->second;
    }

    std::vector<std::optional<ThemeStyle>> flattened(written.size());
    for (std::size_t first = 0; first < written.size(); ++first)
    {
        // Up the chain to the first style already flattened, or to one that
        // extends nothing, then back down it.
        std::vector<std::size_t> chain;
        std::optional<std::size_t> at = first;
        while (at && !flattened[*at])
        {
            if (std::ranges::find(chain, *at) != chain.end())
            {
                return std::unexpected(At(*written[*at].element, "'" + std::string{written[*at].own.name.View()} +
                                                                     "' extends a style that comes back round to "
                                                                     "it, and a style cannot be built on itself."));
            }
            chain.push_back(*at);
            at = bases[*at];
        }
        for (std::vector<std::size_t>::const_reverse_iterator step = chain.rbegin(); step != chain.rend(); ++step)
        {
            const std::optional<std::size_t> base = bases[*step];
            flattened[*step] = base ? Extended(*flattened[*base], written[*step].own) : written[*step].own;
        }
    }

    std::vector<ThemeStyle> styles;
    for (std::optional<ThemeStyle> &style : flattened)
    {
        styles.push_back(std::move(*style));
    }
    return styles;
}

/// Every style the theme declares, refusing a second with one name.
std::expected<void, MarkupError> ReadStyles(std::span<const MarkupElement *const> elements, CompiledTheme &theme)
{
    std::vector<WrittenStyle> written;
    for (const MarkupElement *element : elements)
    {
        std::expected<WrittenStyle, MarkupError> style = ReadStyle(*element, theme);
        if (!style)
        {
            return std::unexpected(style.error());
        }
        const std::vector<WrittenStyle>::const_iterator taken = std::ranges::find_if(
            written, [&style](const WrittenStyle &held) { return held.own.name == style->own.name; });
        if (taken != written.end())
        {
            return std::unexpected(At(*element, "a second style called '" + std::string{style->own.name.View()} +
                                                    "'; the first is at line " +
                                                    std::to_string(taken->element->line) + "."));
        }
        written.push_back(std::move(*style));
    }
    std::expected<std::vector<ThemeStyle>, MarkupError> styles = Flatten(written);
    if (!styles)
    {
        return std::unexpected(styles.error());
    }
    theme.document.styles = std::move(*styles);
    return {};
}

} // namespace

std::expected<CompiledTheme, MarkupError> CompileTheme(const MarkupElement &root)
{
    if (root.name != kThemeElement)
    {
        return std::unexpected(
            At(root, "a theme file starts with <" + std::string{kThemeElement} + ">, not <" + root.name + ">."));
    }
    if (!root.text.empty())
    {
        return std::unexpected(At(root, "a theme holds palettes and styles, and no text."));
    }
    if (!root.attributes.empty())
    {
        return std::unexpected(At(root.attributes[0], "a <" + std::string{kThemeElement} +
                                                          "> takes no attributes; its palettes and styles are "
                                                          "written inside it."));
    }

    std::vector<const MarkupElement *> palettes;
    std::vector<const MarkupElement *> styles;
    for (const MarkupElement &child : root.children)
    {
        if (child.name == kPaletteElement)
        {
            palettes.push_back(&child);
        }
        else if (child.name == kStyleElement)
        {
            styles.push_back(&child);
        }
        else
        {
            return std::unexpected(At(child, "'" + child.name + "' is not part of a theme, which holds <" +
                                                 std::string{kPaletteElement} + "> and <" +
                                                 std::string{kStyleElement} + ">."));
        }
    }
    // A theme the cook allows must be one the codec reads.
    if (std::max(palettes.size(), styles.size()) > Core::Reflect::kMaxVectorElements)
    {
        return std::unexpected(At(root, "a theme holds at most " + std::to_string(Core::Reflect::kMaxVectorElements) +
                                            " palettes and as many styles."));
    }

    // Every palette first, wherever it is written, so a style's tokens are
    // checked against all of them.
    CompiledTheme theme;
    if (const std::expected<void, MarkupError> read = ReadPalettes(root, palettes, theme); !read)
    {
        return std::unexpected(read.error());
    }
    if (theme.document.tokens.size() > Core::Reflect::kMaxVectorElements)
    {
        return std::unexpected(At(root, "a theme holds at most " + std::to_string(Core::Reflect::kMaxVectorElements) +
                                            " tokens."));
    }
    if (const std::expected<void, MarkupError> read = ReadStyles(styles, theme); !read)
    {
        return std::unexpected(read.error());
    }
    return theme;
}

std::expected<CompiledTheme, MarkupError> CompileThemeSource(std::string_view text)
{
    const std::expected<MarkupElement, MarkupError> parsed = ParseMarkup(text);
    if (!parsed)
    {
        return std::unexpected(parsed.error());
    }
    return CompileTheme(*parsed);
}

std::expected<std::vector<std::byte>, MarkupError> CompileThemeText(std::string_view text)
{
    const std::expected<CompiledTheme, MarkupError> theme = CompileThemeSource(text);
    if (!theme)
    {
        return std::unexpected(theme.error());
    }
    Core::BitWriter writer;
    WriteCookedTheme(writer, theme->document);
    const std::span<const std::byte> bytes = writer.Data();
    return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

} // namespace Assisi::Mondrian::Import
