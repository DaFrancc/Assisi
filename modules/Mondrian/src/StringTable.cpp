/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Mondrian/StringTable.hpp>

#include <Assisi/Core/CookedBlob.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace Assisi::Mondrian
{
namespace
{

constexpr std::size_t kBitsPerByte = 8;

/// A floor on what one entry occupies: two empty strings, each a one-byte
/// length. It exists only to stop a count no file could hold from reaching a
/// reserve.
constexpr std::size_t kMinEntryBytes = 2;

/// What separates a file's name from its extension, and a folder from a file.
constexpr char kExtensionMark = '.';
constexpr char kFolderMark = '/';

} // namespace

const std::string *StringTables::Find(std::string_view table, std::string_view key) const
{
    const std::unordered_map<std::string, StringTable>::const_iterator named = byName.find(std::string{table});
    if (named == byName.end())
    {
        return nullptr;
    }
    const std::unordered_map<std::string, std::string>::const_iterator entry =
        named->second.entries.find(std::string{key});
    return entry == named->second.entries.end() ? nullptr : &entry->second;
}

std::string_view TableName(std::string_view vpath)
{
    const std::size_t folder = vpath.rfind(kFolderMark);
    std::string_view name = folder == std::string_view::npos ? vpath : vpath.substr(folder + 1);
    const std::size_t extension = name.rfind(kExtensionMark);
    if (extension != std::string_view::npos && extension > 0)
    {
        name = name.substr(0, extension);
    }
    return name;
}

std::string_view ToString(CookedStringTableError error) noexcept
{
    switch (error)
    {
    case CookedStringTableError::NotAStringTable:
        return "not a cooked string table";
    case CookedStringTableError::Truncated:
        return "ended part-way through";
    case CookedStringTableError::UnsupportedVersion:
        return "a string table layout this build does not read";
    case CookedStringTableError::Invalid:
        return "holds a key twice";
    case CookedStringTableError::Count:
        break;
    }
    return "unknown";
}

void WriteCookedStringTable(Core::BitWriter &writer, const StringTable &table)
{
    Core::WriteCookedHeader(writer, Core::kStringTableKind);
    writer.WriteUInt8(kStringTablePayloadVersion);

    // Sorted, so the same table always cooks to the same bytes whatever order
    // the map happens to hold its entries in.
    std::vector<const std::pair<const std::string, std::string> *> entries;
    entries.reserve(table.entries.size());
    for (const std::pair<const std::string, std::string> &entry : table.entries)
    {
        entries.push_back(&entry);
    }
    std::ranges::sort(entries,
                      [](const std::pair<const std::string, std::string> *one,
                         const std::pair<const std::string, std::string> *other) { return one->first < other->first; });

    writer.WriteVarUInt32(static_cast<uint32_t>(entries.size()));
    for (const std::pair<const std::string, std::string> *entry : entries)
    {
        writer.WriteString(entry->first);
        writer.WriteString(entry->second);
    }
}

std::expected<StringTable, CookedStringTableError> ReadCookedStringTable(std::span<const std::byte> bytes)
{
    Core::BitReader reader{bytes};

    const std::expected<Core::AssetKindId, Core::CookedBlobError> blobKind = Core::ReadCookedHeader(reader);
    if (!blobKind || *blobKind != Core::kStringTableKind)
    {
        if (!blobKind && blobKind.error() == Core::CookedBlobError::Truncated)
        {
            return std::unexpected(CookedStringTableError::Truncated);
        }
        return std::unexpected(CookedStringTableError::NotAStringTable);
    }

    const uint8_t version = reader.ReadUInt8();
    if (reader.Failed())
    {
        return std::unexpected(CookedStringTableError::Truncated);
    }
    if (version != kStringTablePayloadVersion)
    {
        return std::unexpected(CookedStringTableError::UnsupportedVersion);
    }

    const uint32_t count = reader.ReadVarUInt32();
    if (reader.Failed() || count > reader.BitsRemaining() / kBitsPerByte / kMinEntryBytes)
    {
        return std::unexpected(CookedStringTableError::Truncated);
    }

    StringTable table;
    table.entries.reserve(count);
    for (uint32_t index = 0; index < count; ++index)
    {
        std::string key = reader.ReadString(kMaxStringTableEntryBytes);
        std::string text = reader.ReadString(kMaxStringTableEntryBytes);
        if (reader.Failed())
        {
            return std::unexpected(CookedStringTableError::Truncated);
        }
        if (!table.entries.emplace(std::move(key), std::move(text)).second)
        {
            return std::unexpected(CookedStringTableError::Invalid);
        }
    }
    return table;
}

} // namespace Assisi::Mondrian
