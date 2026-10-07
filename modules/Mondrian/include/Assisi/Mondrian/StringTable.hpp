/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file StringTable.hpp
/// @brief The words a screen shows, looked up by key, and their cooked form.
///
/// A project has as many tables as it likes, each named after its file:
/// `ui/pause.csv` is the table `pause`. A screen names a string as
/// `#pause:title`, so where the words live is written where they are used.
///
/// The writer and the reader of the cooked form live together so the layout
/// has one definition. The cook writes through WriteCookedStringTable and every
/// load reads through ReadCookedStringTable, in the game and in the editor.

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/DisplayedString.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Assisi::Mondrian
{

// A screen writes a key the way a DisplayedString does, so the spelling has one
// definition, in Core, below every module that reads it.
using Core::kKeyMark;
using Core::kTableSeparator;
using Core::SplitKey;
using Core::TableKey;

/// @brief Version of the string table payload's layout, separate from the blob
/// envelope's.
///
/// Part of the string table cooker's cache key, so bumping it re-cooks every
/// table.
inline constexpr uint8_t kStringTablePayloadVersion = 1;

/// @brief The longest a key or its text may be, in bytes.
///
/// Room for a long description, and a bound on what a corrupt length prefix can
/// make the reader allocate. The cook refuses anything longer, so a table that
/// cooked always reads.
inline constexpr std::size_t kMaxStringTableEntryBytes = 65536;

/// @brief One table: each key's text.
struct StringTable
{
    std::unordered_map<std::string, std::string> entries;

    [[nodiscard]] friend bool operator==(const StringTable &, const StringTable &) = default;
};

/// @brief Every table a project has, by name.
struct StringTables
{
    std::unordered_map<std::string, StringTable> byName;

    /// @brief The text @p key has in the table called @p table, or null when
    /// there is no such table or it has no such key.
    [[nodiscard]] const std::string *Find(std::string_view table, std::string_view key) const;
};

/// @brief The name the table at @p vpath is known by: the file's name without
/// its folder or its last extension. `text/strings.fr.csv` is `strings.fr`.
[[nodiscard]] std::string_view TableName(std::string_view vpath);

/// @brief Why bytes did not read as a cooked string table.
enum class CookedStringTableError : uint8_t
{
    NotAStringTable,    ///< Not a cooked blob, or a blob of another kind.
    Truncated,          ///< The bytes ran out inside the payload.
    UnsupportedVersion, ///< A table layout this build does not read.
    Invalid,            ///< Framed correctly and holding a key twice.
    Count_
};

/// @brief A short human-readable description, for a load failure's log line.
[[nodiscard]] std::string_view ToString(CookedStringTableError error) noexcept;

/// @brief Write @p table as a complete cooked blob, envelope included.
void WriteCookedStringTable(Core::BitWriter &writer, const StringTable &table);

/// @brief Read a blob written by WriteCookedStringTable.
[[nodiscard]] std::expected<StringTable, CookedStringTableError> ReadCookedStringTable(
    std::span<const std::byte> bytes);

} // namespace Assisi::Mondrian
