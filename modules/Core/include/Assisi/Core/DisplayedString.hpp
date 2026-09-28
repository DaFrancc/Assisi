/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file DisplayedString.hpp
/// @brief Words a player reads: a string-table key or literal text, turned into
///        the current language's words when shown.

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Assisi::Core
{

/// @brief What marks text as a key into a string table, at the start of it:
/// `#pause:title`. Written twice at the start, it is itself; anywhere else it is
/// an ordinary character. Also what a key no table gives anything shows behind,
/// so it reads as the key it is.
inline constexpr char kKeyMark = '#';

/// @brief What separates a table's name from a key in it: `pause:title`.
///
/// Not '.', so a key can be dotted to group its own strings: `pause:menu.title`.
inline constexpr char kTableSeparator = ':';

/// @brief A key split at kTableSeparator.
struct TableKey
{
    std::string_view table;
    std::string_view key;
};

/// @brief @p qualified split into its table and its key, or nothing when it has
/// no separator or either side of it is empty.
[[nodiscard]] std::optional<TableKey> SplitKey(std::string_view qualified);

/// @brief The words @p key has in the table called @p table, or nothing when no
/// table gives it any.
///
/// The view must stay valid until the next resolver call or table change; the
/// caller copies it at once.
using DisplayedStringResolver =
    std::function<std::optional<std::string_view>(std::string_view table, std::string_view key)>;

/// @brief Install the resolver every DisplayedString goes through, returning the
/// one it replaces.
///
/// Core sits below the UI that owns the string tables, so the tables reach it
/// this way: whatever loads them installs a resolver over them. Install at
/// startup, before the first string resolves: the resolver is read without a
/// lock.
DisplayedStringResolver SetDisplayedStringResolver(DisplayedStringResolver resolver);

/// @brief Text a player reads, held as it is written in a file.
///
/// One spelling carries both forms, the one screen markup uses: `#table:key` is a
/// key, `##` at the start stands for a literal `#`, and anything else is literal
/// text. Keeping the written form, rather than splitting it, means a file
/// round-trips byte for byte and a key the tables lack still shows as the key.
class DisplayedString
{
public:
    DisplayedString() = default;

    /// @brief Text as a file writes it, in the spelling above.
    [[nodiscard]] static DisplayedString FromSource(std::string_view source);

    /// @brief The key @p key in the table @p table.
    [[nodiscard]] static DisplayedString FromKey(std::string_view table, std::string_view key);

    /// @brief @p text shown as it is, escaped if it starts with the key mark.
    [[nodiscard]] static DisplayedString FromLiteral(std::string_view text);

    /// @brief The written form: what a file stores.
    [[nodiscard]] const std::string &Source() const { return _source; }

    /// @brief The key without its mark, or nothing when this is literal text.
    ///
    /// A mark followed by something SplitKey cannot split is still a key, just
    /// one no table can hold, so Resolve shows it as written.
    [[nodiscard]] std::optional<std::string_view> Key() const;

    /// @brief The words to show: a key's words through the installed resolver,
    /// or the key itself behind its mark when there are none; literal text with
    /// its escape removed.
    [[nodiscard]] std::string Resolve() const;

    [[nodiscard]] bool Empty() const { return _source.empty(); }

    bool operator==(const DisplayedString &other) const = default;

private:
    std::string _source;
};

} // namespace Assisi::Core
