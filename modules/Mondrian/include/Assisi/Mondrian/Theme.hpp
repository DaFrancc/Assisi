/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Theme.hpp
/// @brief Named styles and palette tokens a project's screens share, swapped
/// while the game runs.
///
/// A theme is a file of its own, cooked to a ThemeDocument. Its palettes give
/// each token a value in each of the theme's modes, and its styles are looks a
/// node takes by naming one. A node names a style and tokens and never a
/// theme, so the look it wears is resolved against whichever theme, in
/// whichever mode, the UI has: switching either restyles every screen with no
/// screen file changed.
///
/// Values are parsed when the theme cooks. A token carries, for each mode, its
/// value read into every style property that can hold it, so using one is a
/// copy and the game parses no text.
///
/// The writer and the reader of the cooked form live here together, as a
/// screen's do in ScreenBlob.hpp, so the layout has one definition.

#include <Assisi/Mondrian/Style.hpp>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/InternedString.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::Mondrian
{

/// @brief A property a look takes from the theme: the value the token named
/// has in whichever mode the theme is in.
ASTRUCT()
struct TokenUse
{
    AFIELD() Core::InternedString token;
    AFIELD() StyleProperty property = StyleProperty::Background;

    [[nodiscard]] friend bool operator==(const TokenUse &, const TokenUse &) = default;
};

/// @brief A look as a file writes it: values written out, and properties taken
/// from tokens. Both count as written; a token's property holds no value of
/// its own until the look is resolved against a theme.
ASTRUCT()
struct ThemedPatch
{
    AFIELD() StylePatch patch;
    /// At most one per property, each also in patch.written.
    AFIELD() std::vector<TokenUse> tokens;

    /// @brief @p property as @p from has it, written out, replacing any token
    /// it took before.
    void Take(StyleProperty property, const Style &from);

    /// @brief @p use's property taken from its token, replacing whatever it
    /// was before.
    void Bind(const TokenUse &use);

    /// @brief Every property @p over writes, value or token, over this one's.
    void Add(const ThemedPatch &over);

    [[nodiscard]] friend bool operator==(const ThemedPatch &, const ThemedPatch &) = default;
};

/// @brief One token of a theme's palettes.
ASTRUCT()
struct ThemeToken
{
    AFIELD() Core::InternedString name;
    /// One per mode, in the order the theme lists its modes: the value read
    /// into every property that can hold it, and written for those alone.
    AFIELD() std::vector<StylePatch> values;

    [[nodiscard]] friend bool operator==(const ThemeToken &, const ThemeToken &) = default;
};

/// @brief A named style: how a node naming it looks at rest and in each state,
/// with the style it extends already put under it.
ASTRUCT()
struct ThemeStyle
{
    AFIELD() Core::InternedString name;
    AFIELD() ThemedPatch resting;
    /// At most one per state.
    AFIELD() std::vector<ThemedPatch> states;

    [[nodiscard]] friend bool operator==(const ThemeStyle &, const ThemeStyle &) = default;
};

/// @brief A whole theme: its modes, its tokens and its styles.
ASTRUCT()
struct ThemeDocument
{
    /// By name, in the order the file writes them. At least one: the first is
    /// the mode a theme is put on in.
    AFIELD() std::vector<Core::InternedString> modes;
    AFIELD() std::vector<ThemeToken> tokens;
    AFIELD() std::vector<ThemeStyle> styles;

    /// @brief The style called @p name, or null.
    [[nodiscard]] const ThemeStyle *FindStyle(std::string_view name) const;
    /// @brief The token called @p name, or null.
    [[nodiscard]] const ThemeToken *FindToken(std::string_view name) const;
    /// @brief Where the mode called @p name is among the modes, or nullopt.
    [[nodiscard]] std::optional<uint32_t> FindMode(std::string_view name) const;

    [[nodiscard]] friend bool operator==(const ThemeDocument &, const ThemeDocument &) = default;
};

/// @brief A theme as the UI holds it: where it was loaded from, which is what
/// game code names it by, and what it holds.
struct ListedTheme
{
    std::string vpath;
    ThemeDocument document;
};

/// @brief Every theme a project lists, in the order it lists them.
struct Themes
{
    std::vector<ListedTheme> listed;

    /// @brief The theme loaded from @p vpath, or null.
    [[nodiscard]] const ListedTheme *Find(std::string_view vpath) const;
};

/// @brief A theme in one of its modes: what a look naming a style or a token is
/// resolved against. One with no theme resolves every look to the node's own.
struct ThemeView
{
    const ListedTheme *theme = nullptr;
    uint32_t mode = 0;
};

/// @brief How a node asks to look, before any theme says anything.
struct NodeLook
{
    /// At rest: what it writes itself, over the defaults.
    Style style;
    /// The resting properties it takes from tokens, each also in written.
    std::vector<TokenUse> tokens;
    /// Its own look in each state it has one for, at most one per state.
    std::vector<ThemedPatch> states;
    /// The style it names, or empty for none.
    Core::InternedString styleName;
    /// Which properties of style it writes, which its named style's give way
    /// to. One bit per StyleProperty.
    uint64_t written = 0;
};

/// @brief What a node wears under a theme.
struct ResolvedLook
{
    /// At rest, every value in place. Written is what anything wrote: the node,
    /// its style or a token. The rest are defaults, and the text colour among
    /// them is the parent's.
    StylePatch resting;
    /// At most one per state: its style's look with its own over it.
    std::vector<StylePatch> states;
};

/// @brief @p look as it is worn under @p theme. Weakest first: the style's
/// resting look, the node's own, then for each state the style's look and the
/// node's own over it. A style or a token the theme does not have is logged and
/// leaves what the node writes itself.
[[nodiscard]] ResolvedLook ResolveLook(const NodeLook &look, const ThemeView &theme);

/// @brief Why the UI could not put on the theme or mode asked for.
enum class ThemeError : uint8_t
{
    UnknownTheme, ///< no listed theme was loaded from that path
    UnknownMode,  ///< the theme has no mode by that name, or there is no theme
    Count_
};

[[nodiscard]] std::string_view ToString(ThemeError error) noexcept;

/// @brief Version of how the theme payload is framed around the document's
/// fields, separate from the blob envelope's. The fields themselves are covered
/// by ThemeLayoutHash.
///
/// Part of the theme cooker's cache key, so bumping it re-cooks every theme.
inline constexpr uint8_t kThemePayloadVersion = 1;

/// @brief Hash of the document's reflected layout, every nested field included.
[[nodiscard]] uint64_t ThemeLayoutHash();

/// @brief Why bytes did not read as a cooked theme.
enum class CookedThemeError : uint8_t
{
    NotATheme,          ///< Not a cooked blob, or a blob of another kind.
    Truncated,          ///< The bytes ran out inside the payload.
    UnsupportedVersion, ///< A payload version or field layout this build does not read.
    Invalid,            ///< Framed correctly and describing no usable theme.
    Count_
};

/// @brief A short human-readable description, for a load failure's log line.
[[nodiscard]] std::string_view ToString(CookedThemeError error) noexcept;

/// @brief Write @p document as a complete cooked blob, envelope included.
void WriteCookedTheme(Core::BitWriter &writer, const ThemeDocument &document);

/// @brief Read a blob written by WriteCookedTheme.
///
/// A theme with no mode, a token without one value per mode, two modes, tokens
/// or styles sharing a name, or a look for one state twice is Invalid.
[[nodiscard]] std::expected<ThemeDocument, CookedThemeError> ReadCookedTheme(std::span<const std::byte> bytes);

} // namespace Assisi::Mondrian
