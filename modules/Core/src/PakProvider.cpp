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
        _byPath.emplace(DerivedAssetId(_entries[i].path), i);
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
        return std::unexpected(AssetErrorCode::CorruptArchive);
    }
    const std::expected<PakHeader, PakFormatError> header = ReadPakHeader(headerBytes);
    if (!header)
    {
        Log::Error("Pak: '{}': {}.", path.string(), ToString(header.error()));
        return std::unexpected(header.error() == PakFormatError::UnsupportedVersion
                                   ? AssetErrorCode::UnsupportedEncoding
                                   : AssetErrorCode::CorruptArchive);
    }

    // The index is the tail of the file, exactly: anything else means the file
    // was cut short, extended, or its header points somewhere invented.
    const std::uint64_t fileSize = file->Size();
    const std::uint64_t indexBytes =
        static_cast<std::uint64_t>(header->entryCount) * kPakEntryBytes + header->pathBytes;
    if (header->indexOffset < kPakHeaderBytes || header->indexOffset > fileSize ||
        fileSize - header->indexOffset != indexBytes)
    {
        Log::Error("Pak: '{}' does not end where its index says it should.", path.string());
        return std::unexpected(AssetErrorCode::CorruptArchive);
    }

    std::vector<std::byte> indexData(static_cast<std::size_t>(indexBytes));
    if (!file->ReadAt(header->indexOffset, indexData))
    {
        return std::unexpected(AssetErrorCode::CorruptArchive);
    }

    std::expected<std::vector<PakEntry>, PakFormatError> entries = ReadPakIndex(indexData, *header);
    if (!entries)
    {
        Log::Error("Pak: '{}': {}.", path.string(), ToString(entries.error()));
        return std::unexpected(AssetErrorCode::CorruptArchive);
    }
    for (const PakEntry &entry : *entries)
    {
        // Slices live between the header and the index. One reaching outside
        // would read header or index bytes as asset data.
        if (entry.archive == kThisArchive && (entry.offset < kPakHeaderBytes || entry.offset > header->indexOffset ||
                                              entry.storedSize > header->indexOffset - entry.offset))
        {
            Log::Error("Pak: '{}' entry '{}' points outside the slice region.", path.string(), entry.path);
            return std::unexpected(AssetErrorCode::CorruptArchive);
        }
    }

    PakProvider provider(std::move(*file), std::move(*entries));
    if (provider._byId.size() != provider._entries.size() || provider._byPath.size() != provider._entries.size())
    {
        Log::Error("Pak: '{}' holds two slices under one id or one path.", path.string());
        return std::unexpected(AssetErrorCode::CorruptArchive);
    }
    return provider;
}

std::expected<std::vector<std::byte>, AssetError> PakProvider::Open(AssetId id) const
{
    if (id.IsReserved())
    {
        return std::unexpected(AssetErrorCode::UnknownAssetId);
    }
    const auto found = _byId.find(id);
    if (found == _byId.end())
    {
        return std::unexpected(AssetErrorCode::UnknownAssetId);
    }
    const PakEntry &entry = _entries[found->second];

    // Refused before a byte is read: each of these names bytes this build would
    // misread, and ciphertext decompressed as if it were plain is garbage that
    // might still frame well enough to reach a loader.
    if (entry.archive != kThisArchive || HasFlag(entry, PakSliceFlag::Encrypted) || entry.codec >= PakCodec::Count_)
    {
        return std::unexpected(AssetErrorCode::UnsupportedEncoding);
    }

    std::vector<std::byte> stored(static_cast<std::size_t>(entry.storedSize));
    if (!_file.ReadAt(entry.offset, stored))
    {
        return std::unexpected(AssetErrorCode::FileReadFailed);
    }
    if (entry.codec == PakCodec::None)
    {
        if (stored.size() != entry.uncompressedSize)
        {
            return std::unexpected(AssetErrorCode::CorruptArchive);
        }
        return stored;
    }

    std::expected<std::vector<std::byte>, PakCodecError> bytes =
        DecompressSlice(entry.codec, stored, static_cast<std::size_t>(entry.uncompressedSize));
    if (!bytes)
    {
        return std::unexpected(AssetErrorCode::CorruptArchive);
    }
    return std::move(*bytes);
}

std::expected<AssetId, AssetError> PakProvider::Resolve(std::string_view vpath) const
{
    const PakEntry *entry = Find(vpath);
    if (entry == nullptr)
    {
        return std::unexpected(AssetErrorCode::UnknownAssetId);
    }
    return entry->id;
}

const PakEntry *PakProvider::Find(std::string_view vpath) const
{
    const auto found = _byPath.find(DerivedAssetId(vpath));
    return found == _byPath.end() ? nullptr : &_entries[found->second];
}

std::vector<PakEntry> PakProvider::EntriesOfKind(AssetKindId kind) const
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
