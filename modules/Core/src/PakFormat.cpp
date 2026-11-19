/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Core/PakFormat.hpp>

#include <string>
#include <utility>

namespace Assisi::Core
{

std::string_view ToString(PakFormatError error) noexcept
{
    switch (error)
    {
    case PakFormatError::NotAPak:
        return "not a pak";
    case PakFormatError::UnsupportedVersion:
        return "a pak layout version this build does not read";
    case PakFormatError::Truncated:
        return "the pak ends before its header or index does";
    case PakFormatError::BadPath:
        return "an index entry's path lies outside the path table";
    }
    return "unknown";
}

void WritePakHeader(BitWriter &writer, const PakHeader &header)
{
    writer.WriteUInt32(header.magic);
    writer.WriteUInt16(header.version);
    writer.WriteUInt32(header.entryCount);
    writer.WriteUInt64(header.indexOffset);
    writer.WriteUInt32(header.pathBytes);
}

std::expected<PakHeader, PakFormatError> ReadPakHeader(std::span<const std::byte> bytes)
{
    BitReader reader{bytes};
    PakHeader header;
    header.magic = reader.ReadUInt32();
    if (reader.Failed())
    {
        return std::unexpected(PakFormatError::Truncated);
    }
    if (header.magic != kPakMagic)
    {
        return std::unexpected(PakFormatError::NotAPak);
    }

    header.version     = reader.ReadUInt16();
    header.entryCount  = reader.ReadUInt32();
    header.indexOffset = reader.ReadUInt64();
    header.pathBytes   = reader.ReadUInt32();
    if (reader.Failed())
    {
        return std::unexpected(PakFormatError::Truncated);
    }
    if (header.version != kPakFormatVersion)
    {
        return std::unexpected(PakFormatError::UnsupportedVersion);
    }
    return header;
}

std::uint32_t PakPathBytes(std::span<const PakEntry> entries) noexcept
{
    std::uint64_t bytes = 0;
    for (const PakEntry &entry : entries)
    {
        bytes += entry.path.size();
    }
    return static_cast<std::uint32_t>(bytes);
}

namespace
{

void WritePakEntry(BitWriter &writer, const PakEntry &entry, std::uint32_t pathOffset)
{
    WriteAssetId(writer, entry.id);
    writer.WriteUInt32(pathOffset);
    writer.WriteUInt16(static_cast<std::uint16_t>(entry.path.size()));
    writer.WriteUInt64(entry.offset);
    writer.WriteUInt64(entry.storedSize);
    writer.WriteUInt64(entry.uncompressedSize);
    writer.WriteUInt64(entry.contentHash);
    writer.WriteUInt64(entry.kind.hash);
    writer.WriteUInt16(entry.archive);
    writer.WriteUInt8(static_cast<std::uint8_t>(entry.codec));
    writer.WriteUInt8(entry.flags);
}

std::expected<PakEntry, PakFormatError> ReadPakEntry(BitReader &reader, std::string_view paths)
{
    PakEntry entry;
    entry.id                       = ReadAssetId(reader);
    const std::uint32_t pathOffset = reader.ReadUInt32();
    const std::uint16_t pathLength = reader.ReadUInt16();
    entry.offset                   = reader.ReadUInt64();
    entry.storedSize          = reader.ReadUInt64();
    entry.uncompressedSize    = reader.ReadUInt64();
    entry.contentHash         = reader.ReadUInt64();
    entry.kind.hash = reader.ReadUInt64();
    entry.archive             = reader.ReadUInt16();
    const std::uint8_t codec  = reader.ReadUInt8();
    entry.flags               = reader.ReadUInt8();
    if (reader.Failed())
    {
        return std::unexpected(PakFormatError::Truncated);
    }
    if (pathLength == 0 || pathOffset > paths.size() || pathLength > paths.size() - pathOffset)
    {
        return std::unexpected(PakFormatError::BadPath);
    }
    entry.path = std::string{paths.substr(pathOffset, pathLength)};

    // Kept as values here rather than refused: an unknown codec, flag or kind is
    // a property of one slice, and whoever opens that slice refuses it while
    // every other asset stays loadable.
    entry.codec = static_cast<PakCodec>(codec);
    return entry;
}

} // namespace

void WritePakIndex(BitWriter &writer, std::span<const PakEntry> entries)
{
    std::uint32_t pathOffset = 0;
    for (const PakEntry &entry : entries)
    {
        WritePakEntry(writer, entry, pathOffset);
        pathOffset += static_cast<std::uint32_t>(entry.path.size());
    }
    for (const PakEntry &entry : entries)
    {
        writer.WriteBytes(std::as_bytes(std::span{entry.path}));
    }
}

std::expected<std::vector<PakEntry>, PakFormatError> ReadPakIndex(std::span<const std::byte> bytes,
                                                                  const PakHeader &header)
{
    const std::uint64_t entryBytes = static_cast<std::uint64_t>(header.entryCount) * kPakEntryBytes;
    if (bytes.size() != entryBytes + header.pathBytes)
    {
        return std::unexpected(PakFormatError::Truncated);
    }
    const std::span<const std::byte> table = bytes.subspan(static_cast<std::size_t>(entryBytes));
    const std::string_view paths{reinterpret_cast<const char *>(table.data()), table.size()};

    std::vector<PakEntry> entries;
    entries.reserve(header.entryCount);
    BitReader reader{bytes.first(static_cast<std::size_t>(entryBytes))};
    for (std::uint32_t i = 0; i < header.entryCount; ++i)
    {
        std::expected<PakEntry, PakFormatError> entry = ReadPakEntry(reader, paths);
        if (!entry)
        {
            return std::unexpected(entry.error());
        }
        entries.push_back(std::move(*entry));
    }
    return entries;
}

} // namespace Assisi::Core
