/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/StringTableReader.hpp>

#include <Assisi/Core/AssetProvider.hpp>
#include <Assisi/Core/Logger.hpp>

#include <cstddef>
#include <utility>
#include <vector>

namespace Assisi::Mondrian
{
namespace
{

StringTableReader &InstalledReader()
{
    static StringTableReader reader;
    return reader;
}

} // namespace

std::string_view ToString(StringTableReadError error) noexcept
{
    switch (error)
    {
    case StringTableReadError::Missing:
        return "no string table at that path";
    case StringTableReadError::Unreadable:
        return "the string table could not be read";
    case StringTableReadError::Invalid:
        return "the string table is not usable";
    case StringTableReadError::NoReader:
        return "no string table reader is installed";
    case StringTableReadError::DuplicateName:
        return "another listed table has the same name";
    case StringTableReadError::Count:
        break;
    }
    return "unknown";
}

StringTableReader SetStringTableReader(StringTableReader reader)
{
    return std::exchange(InstalledReader(), std::move(reader));
}

std::expected<StringTable, StringTableReadError> LoadStringTable(std::string_view vpath)
{
    const StringTableReader &reader = InstalledReader();
    if (!reader)
    {
        // No fallback to reading files: a game that quietly opened a loose table
        // would hide a package that is missing one.
        Core::Log::Error("Mondrian: no string table reader is installed to load '{}'.", vpath);
        return std::unexpected(StringTableReadError::NoReader);
    }
    return reader(vpath);
}

std::expected<StringTables, StringTableLoadError> LoadStringTables(const UiConfig &config)
{
    StringTables tables;
    for (const Core::AssetPath &path : config.stringTables)
    {
        const std::string_view vpath = path.View();
        const std::string name{TableName(vpath)};
        if (tables.byName.contains(name))
        {
            return std::unexpected(
                StringTableLoadError{.vpath = std::string{vpath}, .error = StringTableReadError::DuplicateName});
        }
        std::expected<StringTable, StringTableReadError> table = LoadStringTable(vpath);
        if (!table)
        {
            return std::unexpected(StringTableLoadError{.vpath = std::string{vpath}, .error = table.error()});
        }
        tables.byName.emplace(name, std::move(*table));
    }
    return tables;
}

std::expected<StringTable, StringTableReadError> ReadCookedStringTable(const Core::AssetProvider &provider,
                                                                       std::string_view vpath)
{
    const std::expected<Core::AssetId, Core::AssetError> id = provider.Resolve(vpath);
    if (!id)
    {
        return std::unexpected(id.error() == Core::AssetErrorCode::UnknownAssetId ? StringTableReadError::Missing
                                                                                  : StringTableReadError::Unreadable);
    }
    const std::expected<std::vector<std::byte>, Core::AssetError> bytes = provider.Open(*id);
    if (!bytes)
    {
        return std::unexpected(StringTableReadError::Unreadable);
    }
    std::expected<StringTable, CookedStringTableError> table = ReadCookedStringTable(*bytes);
    if (!table)
    {
        Core::Log::Error("Mondrian: '{}' is not a usable string table ({}).", vpath, ToString(table.error()));
        return std::unexpected(StringTableReadError::Invalid);
    }
    return std::move(*table);
}

} // namespace Assisi::Mondrian
