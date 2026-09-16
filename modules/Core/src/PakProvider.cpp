/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Core/PakProvider.hpp>

#include <Assisi/Core/Logger.hpp>

#include <algorithm>
#include <utility>

namespace Assisi::Core
{

namespace
{

/// The only archive a pak refers to today: the file itself.
constexpr std::uint16_t kThisArchive = 0;

bool HasFlag(const PakEntry &entry, PakSliceFlag flag)
{
    return (entry.flags & static_cast<std::uint8_t>(flag)) != 0;
}

} // namespace

PakProvider::PakProvider(RandomAccessFile file, std::vector<PakEntry> entries)
    : _file(std::move(file)), _entries(std::move(entries))
{
    _byId.reserve(_entries.size());
    _byPath.reserve(_entries.size());
    for (std::size_t i = 0; i < _entries.size(); ++i)
    {
        _byId.emplace(_entries[i].id, i);
        _byPath.emplace(_entries[i].pathId, i);
    }
}

std::expected<PakProvider, AssetError> PakProvider::Mount(const std::filesystem::path &path)
{
    std::expected<RandomAccessFile, AssetError> file = RandomAccessFile::Open(path);
    if (!file)
    {
        Log::Error("Pak: cannot open '{}'.", path.string());
        return std::unexpected(file.error());
    }

    std::vector<std::byte> headerBytes(kPakHeaderBytes);
    if (!file->ReadAt(0, headerBytes))
    {
        Log::Error("Pak: '{}' is too short to be a pak.", path.string());
        return std::unexpected(AssetError::CorruptArchive);
    }
    const std::expected<PakHeader, PakFormatError> header = ReadPakHeader(headerBytes);
    if (!header)
    {
        Log::Error("Pak: '{}': {}.", path.string(), ToString(header.error()));
        return std::unexpected(header.error() == PakFormatError::UnsupportedVersion ? AssetError::UnsupportedEncoding
                                                                                    : AssetError::CorruptArchive);
    }

    // The index is the tail of the file, exactly: anything else means the file
    // was cut short, extended, or its header points somewhere invented.
    const std::uint64_t fileSize   = file->Size();
    const std::uint64_t indexBytes = static_cast<std::uint64_t>(header->entryCount) * kPakEntryBytes;
    if (header->indexOffset < kPakHeaderBytes || header->indexOffset > fileSize ||
        fileSize - header->indexOffset != indexBytes)
    {
        Log::Error("Pak: '{}' does not end where its index says it should.", path.string());
        return std::unexpected(AssetError::CorruptArchive);
    }

    std::vector<std::byte> indexData(static_cast<std::size_t>(indexBytes));
    if (!file->ReadAt(header->indexOffset, indexData))
    {
        return std::unexpected(AssetError::CorruptArchive);
    }

    std::vector<PakEntry> entries;
    entries.reserve(header->entryCount);
    BitReader reader{indexData};
    for (std::uint32_t i = 0; i < header->entryCount; ++i)
    {
        const std::expected<PakEntry, PakFormatError> entry = ReadPakEntry(reader);
        if (!entry)
        {
            Log::Error("Pak: '{}' entry {}: {}.", path.string(), i, ToString(entry.error()));
            return std::unexpected(AssetError::CorruptArchive);
        }
        // Slices live between the header and the index. One reaching outside
        // would read header or index bytes as asset data.
        if (entry->archive == kThisArchive &&
            (entry->offset < kPakHeaderBytes || entry->offset > header->indexOffset ||
             entry->storedSize > header->indexOffset - entry->offset))
        {
            Log::Error("Pak: '{}' entry {} points outside the slice region.", path.string(), i);
            return std::unexpected(AssetError::CorruptArchive);
        }
        entries.push_back(*entry);
    }

    PakProvider provider(std::move(*file), std::move(entries));
    if (provider._byId.size() != provider._entries.size() || provider._byPath.size() != provider._entries.size())
    {
        Log::Error("Pak: '{}' holds two slices under one id or one path.", path.string());
        return std::unexpected(AssetError::CorruptArchive);
    }
    return provider;
}

std::expected<std::vector<std::byte>, AssetError> PakProvider::Open(AssetId id) const
{
    if (id.IsReserved())
    {
        return std::unexpected(AssetError::UnknownAssetId);
    }
    const auto found = _byId.find(id);
    if (found == _byId.end())
    {
        return std::unexpected(AssetError::UnknownAssetId);
    }
    const PakEntry &entry = _entries[found->second];

    // Refused before a byte is read: each of these names bytes this build would
    // misread, and ciphertext decompressed as if it were plain is garbage that
    // might still frame well enough to reach a loader.
    if (entry.archive != kThisArchive || HasFlag(entry, PakSliceFlag::Encrypted) || entry.codec >= PakCodec::Count)
    {
        return std::unexpected(AssetError::UnsupportedEncoding);
    }

    std::vector<std::byte> stored(static_cast<std::size_t>(entry.storedSize));
    if (!_file.ReadAt(entry.offset, stored))
    {
        return std::unexpected(AssetError::FileReadFailed);
    }
    if (entry.codec == PakCodec::None)
    {
        if (stored.size() != entry.uncompressedSize)
        {
            return std::unexpected(AssetError::CorruptArchive);
        }
        return stored;
    }

    std::expected<std::vector<std::byte>, PakCodecError> bytes =
        DecompressSlice(entry.codec, stored, static_cast<std::size_t>(entry.uncompressedSize));
    if (!bytes)
    {
        return std::unexpected(AssetError::CorruptArchive);
    }
    return std::move(*bytes);
}

std::expected<AssetId, AssetError> PakProvider::Resolve(std::string_view vpath) const
{
    const auto found = _byPath.find(DerivedAssetId(vpath));
    if (found == _byPath.end())
    {
        return std::unexpected(AssetError::UnknownAssetId);
    }
    return _entries[found->second].id;
}

std::vector<PakEntry> PakProvider::EntriesOfKind(CookedKind kind) const
{
    std::vector<PakEntry> matching;
    for (const PakEntry &entry : _entries)
    {
        if (entry.kind == kind)
        {
            matching.push_back(entry);
        }
    }
    std::ranges::sort(matching, [](const PakEntry &a, const PakEntry &b) { return a.id < b.id; });
    return matching;
}

} // namespace Assisi::Core
