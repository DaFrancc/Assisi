/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file StringTableReader.hpp
/// @brief Where a string table comes from, installed per executable.
///
/// A shipped game reads the tables the cook wrote into its package; the editor
/// compiles a source file in process and reads the blob it just produced. Both
/// end in ReadCookedStringTable, as a screen's two readers end in
/// ReadCookedScreen, so there is one path from bytes to a table.

#include <Assisi/Mondrian/StringTable.hpp>
#include <Assisi/Mondrian/UiConfig.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>

namespace Assisi::Core
{
class AssetProvider;
}

namespace Assisi::Mondrian
{

/// @brief Why a string table did not arrive, or did not read.
enum class StringTableReadError : uint8_t
{
    Missing,       ///< No table at that path.
    Unreadable,    ///< It exists and its bytes could not be read.
    Invalid,       ///< The bytes were read and are not a usable table.
    NoReader,      ///< No reader is installed.
    DuplicateName, ///< Two listed tables share a name, so a key could mean either.
    Count
};

[[nodiscard]] std::string_view ToString(StringTableReadError error) noexcept;

/// @brief A table that did not load, and which one.
struct StringTableLoadError
{
    std::string vpath;
    StringTableReadError error = StringTableReadError::Missing;
};

using StringTableReader = std::function<std::expected<StringTable, StringTableReadError>(std::string_view vpath)>;

/// @brief Install the reader every table load goes through, returning the one
/// it replaces. Install at startup, before the first table loads: the reader is
/// read without a lock.
StringTableReader SetStringTableReader(StringTableReader reader);

/// @brief The table at @p vpath, through the installed reader.
[[nodiscard]] std::expected<StringTable, StringTableReadError> LoadStringTable(std::string_view vpath);

/// @brief Every table @p config lists, each under its name. The first that
/// does not load is the error, and no tables are returned with it.
[[nodiscard]] std::expected<StringTables, StringTableLoadError> LoadStringTables(const UiConfig &config);

/// @brief The reader for tables cooked into @p provider.
[[nodiscard]] std::expected<StringTable, StringTableReadError> ReadCookedStringTable(
    const Core::AssetProvider &provider, std::string_view vpath);

} // namespace Assisi::Mondrian
