/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Theme.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/StructMeta.hpp>

#include <algorithm>
#include <cstddef>
#include <unordered_set>

ASSISI_REFLECTED_STRUCT(Assisi::Mondrian::ThemeDocument);

namespace Assisi::Mondrian
{
namespace
{

/// Bits in the layout hash that follows the version.
constexpr uint32_t kLayoutHashBits = 64;

const Core::Reflect::StructSpec &DocumentSpec()
{
    const Core::Reflect::StructSpec *spec = Core::Reflect::StructTraits<ThemeDocument>::Spec();
    ASSISI_ASSERT(spec != nullptr, "ThemeDocument's generated table is missing");
    return *spec;
}

void EraseToken(std::vector<TokenUse> &tokens, StyleProperty property)
{
    std::erase_if(tokens, [property](const TokenUse &use) { return use.property == property; });
}

/// The entry of @p items whose name is @p name, or null.
template <typename T> const T *Named(const std::vector<T> &items, std::string_view name)
{
    const typename std::vector<T>::const_iterator found =
        std::ranges::find_if(items, [name](const T &item) { return item.name.View() == name; });
    return found != items.end() ? &*found : nullptr;
}

std::string_view NameOf(const Core::InternedString &name)
{
    return name.View();
}

std::string_view NameOf(const ThemeToken &token)
{
    return token.name.View();
}

std::string_view NameOf(const ThemeStyle &style)
{
    return style.name.View();
}

/// Whether no two of @p items share a name, so a name finds one of them.
template <typename T> bool NamesDistinct(const std::vector<T> &items)
{
    std::unordered_set<std::string_view> seen;
    for (const T &item : items)
    {
        if (!seen.insert(NameOf(item)).second)
        {
            return false;
        }
    }
    return true;
}

/// Whether @p looks are ones a cook writes: at most one per state, which
/// otherwise would stack in whatever order they were kept, and each writing
/// only properties this build has.
bool LooksConsistent(const std::vector<ThemedPatch> &looks)
{
    StateSet seen{};
    for (const ThemedPatch &look : looks)
    {
        const std::size_t state = static_cast<std::size_t>(look.patch.state);
        if (state >= seen.size() || seen[state] || !look.patch.WritesOnlyKnown())
        {
            return false;
        }
        seen[state] = true;
    }
    return true;
}

/// Whether @p document is one a cook writes, so every lookup into it finds one
/// thing and every token has a value for whichever mode the theme is in.
bool IsConsistent(const ThemeDocument &document)
{
    if (document.modes.empty())
    {
        return false;
    }
    if (!NamesDistinct(document.modes) || !NamesDistinct(document.tokens) || !NamesDistinct(document.styles))
    {
        return false;
    }
    for (const ThemeToken &token : document.tokens)
    {
        if (token.values.size() != document.modes.size() ||
            !std::ranges::all_of(token.values, &StylePatch::WritesOnlyKnown))
        {
            return false;
        }
    }
    return std::ranges::all_of(document.styles, [](const ThemeStyle &style)
                               { return style.resting.patch.WritesOnlyKnown() && LooksConsistent(style.states); });
}

/// @p look with each token's value in @p theme filled in. A token the theme
/// does not have, or has no value for the property in, leaves the look's own.
StylePatch Bound(const ThemedPatch &look, const ThemeView &theme)
{
    StylePatch bound = look.patch;
    if (theme.theme == nullptr)
    {
        return bound;
    }
    const ThemeDocument &document = theme.theme->document;
    for (const TokenUse &use : look.tokens)
    {
        const ThemeToken *token = document.FindToken(use.token.View());
        if (token == nullptr || !token->values[theme.mode].Writes(use.property))
        {
            Core::Log::Error("Mondrian: the theme '{}' has no token '{}' a node can use there, so it keeps its "
                             "own value.",
                             theme.theme->vpath, use.token.View());
            continue;
        }
        bound.Take(use.property, token->values[theme.mode].values);
    }
    return bound;
}

} // namespace

void ThemedPatch::Take(StyleProperty property, const Style &from)
{
    patch.Take(property, from);
    EraseToken(tokens, property);
}

void ThemedPatch::Bind(const TokenUse &use)
{
    EraseToken(tokens, use.property);
    tokens.push_back(use);
    patch.written |= PropertyBit(use.property);
}

void ThemedPatch::Add(const ThemedPatch &over)
{
    patch.Add(over.patch);
    std::erase_if(tokens, [&over](const TokenUse &use) { return over.patch.Writes(use.property); });
    tokens.insert(tokens.end(), over.tokens.begin(), over.tokens.end());
}

const ThemeStyle *ThemeDocument::FindStyle(std::string_view name) const
{
    return Named(styles, name);
}

const ThemeToken *ThemeDocument::FindToken(std::string_view name) const
{
    return Named(tokens, name);
}

std::optional<uint32_t> ThemeDocument::FindMode(std::string_view name) const
{
    for (uint32_t index = 0; index < modes.size(); ++index)
    {
        if (modes[index].View() == name)
        {
            return index;
        }
    }
    return std::nullopt;
}

const ListedTheme *Themes::Find(std::string_view vpath) const
{
    const std::vector<ListedTheme>::const_iterator found = std::ranges::find(listed, vpath, &ListedTheme::vpath);
    return found != listed.end() ? &*found : nullptr;
}

ResolvedLook ResolveLook(const NodeLook &look, const ThemeView &theme)
{
    ASSISI_ASSERT(theme.theme == nullptr || theme.mode < theme.theme->document.modes.size(),
                  "a theme is put on in a mode it has");

    const ThemeStyle *named = nullptr;
    if (theme.theme != nullptr && !look.styleName.View().empty())
    {
        named = theme.theme->document.FindStyle(look.styleName.View());
        if (named == nullptr)
        {
            Core::Log::Error("Mondrian: the theme '{}' has no style '{}', so a node naming it wears its own look "
                             "alone.",
                             theme.theme->vpath, look.styleName.View());
        }
    }

    ResolvedLook resolved;
    resolved.resting.values = look.style;
    if (named != nullptr)
    {
        resolved.resting.Add(Bound(named->resting, theme));
    }
    const ThemedPatch own{.patch = StylePatch{.values = look.style, .written = look.written, .state = {}},
                          .tokens = look.tokens};
    resolved.resting.Add(Bound(own, theme));

    for (std::size_t index = 0; index < static_cast<std::size_t>(StyleState::Count_); ++index)
    {
        const StyleState state = static_cast<StyleState>(index);
        StylePatch merged;
        merged.state = state;
        if (named != nullptr)
        {
            for (const ThemedPatch &styled : named->states)
            {
                if (styled.patch.state == state)
                {
                    merged.Add(Bound(styled, theme));
                }
            }
        }
        for (const ThemedPatch &ownLook : look.states)
        {
            if (ownLook.patch.state == state)
            {
                merged.Add(Bound(ownLook, theme));
            }
        }
        if (merged.written != 0)
        {
            resolved.states.push_back(merged);
        }
    }
    return resolved;
}

std::string_view ToString(ThemeError error) noexcept
{
    switch (error)
    {
    case ThemeError::UnknownTheme:
        return "no listed theme was loaded from that path";
    case ThemeError::UnknownMode:
        return "the theme has no mode by that name";
    case ThemeError::Count_:
        break;
    }
    return "unknown";
}

uint64_t ThemeLayoutHash()
{
    return Core::Reflect::StructLayoutHash(DocumentSpec());
}

std::string_view ToString(CookedThemeError error) noexcept
{
    switch (error)
    {
    case CookedThemeError::NotATheme:
        return "not a cooked theme";
    case CookedThemeError::Truncated:
        return "ended part-way through";
    case CookedThemeError::UnsupportedVersion:
        return "a theme layout this build does not read";
    case CookedThemeError::Invalid:
        return "describes no usable theme";
    case CookedThemeError::Count_:
        break;
    }
    return "unknown";
}

void WriteCookedTheme(Core::BitWriter &writer, const ThemeDocument &document)
{
    Core::WriteCookedHeader(writer, Core::kThemeKind);
    writer.WriteUInt8(kThemePayloadVersion);
    writer.WriteBits64(ThemeLayoutHash(), kLayoutHashBits);
    (void)Core::Reflect::WriteStruct(DocumentSpec(), &document, writer);
}

std::expected<ThemeDocument, CookedThemeError> ReadCookedTheme(std::span<const std::byte> bytes)
{
    Core::BitReader reader{bytes};

    const std::expected<Core::AssetKindId, Core::CookedBlobError> blobKind = Core::ReadCookedHeader(reader);
    if (!blobKind || *blobKind != Core::kThemeKind)
    {
        if (!blobKind && blobKind.error() == Core::CookedBlobError::Truncated)
        {
            return std::unexpected(CookedThemeError::Truncated);
        }
        return std::unexpected(CookedThemeError::NotATheme);
    }

    const uint8_t version = reader.ReadUInt8();
    const uint64_t layout = reader.ReadBits64(kLayoutHashBits);
    if (reader.Failed())
    {
        return std::unexpected(CookedThemeError::Truncated);
    }
    // Either one differing means the fields that follow are not the ones this
    // build would read them into.
    if (version != kThemePayloadVersion || layout != ThemeLayoutHash())
    {
        return std::unexpected(CookedThemeError::UnsupportedVersion);
    }

    ThemeDocument document;
    const bool read = Core::Reflect::ReadStruct(DocumentSpec(), &document, reader);
    // Framing first: a document read out of bytes that ran out holds whatever
    // zeroes the reader handed back, and calling that inconsistent would name
    // the wrong fault.
    if (reader.Failed())
    {
        return std::unexpected(CookedThemeError::Truncated);
    }
    if (!read || !IsConsistent(document))
    {
        return std::unexpected(CookedThemeError::Invalid);
    }
    return document;
}

} // namespace Assisi::Mondrian
