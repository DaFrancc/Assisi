/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Core/PakFormat.hpp>

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
    case PakFormatError::Corrupt:
        return "the pak's index holds a value no writer produces";
    }
    return "unknown";
}

void WritePakHeader(BitWriter &writer, const PakHeader &header)
{
    writer.WriteUInt32(header.magic);
    writer.WriteUInt16(header.version);
    writer.WriteUInt32(header.entryCount);
    writer.WriteUInt64(header.indexOffset);
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

void WritePakEntry(BitWriter &writer, const PakEntry &entry)
{
    WriteAssetId(writer, entry.id);
    WriteAssetId(writer, entry.pathId);
    writer.WriteUInt64(entry.offset);
    writer.WriteUInt64(entry.storedSize);
    writer.WriteUInt64(entry.uncompressedSize);
    writer.WriteUInt64(entry.contentHash);
    writer.WriteUInt16(entry.archive);
    writer.WriteUInt8(static_cast<std::uint8_t>(entry.codec));
    writer.WriteUInt8(entry.flags);
    writer.WriteUInt8(static_cast<std::uint8_t>(entry.kind));
}

std::expected<PakEntry, PakFormatError> ReadPakEntry(BitReader &reader)
{
    PakEntry entry;
    entry.id                  = ReadAssetId(reader);
    entry.pathId              = ReadAssetId(reader);
    entry.offset              = reader.ReadUInt64();
    entry.storedSize          = reader.ReadUInt64();
    entry.uncompressedSize    = reader.ReadUInt64();
    entry.contentHash         = reader.ReadUInt64();
    entry.archive             = reader.ReadUInt16();
    const std::uint8_t codec  = reader.ReadUInt8();
    entry.flags               = reader.ReadUInt8();
    const std::uint8_t kind   = reader.ReadUInt8();
    if (reader.Failed())
    {
        return std::unexpected(PakFormatError::Truncated);
    }

    // Kept as values here rather than refused: an unknown codec or a set flag is
    // a property of one slice, and the provider refuses that slice when it is
    // opened while every other asset stays loadable. A kind outside the table is
    // different — no writer produces it, so the index itself is suspect.
    if (kind >= static_cast<std::uint8_t>(CookedKind::Count))
    {
        return std::unexpected(PakFormatError::Corrupt);
    }
    entry.codec = static_cast<PakCodec>(codec);
    entry.kind  = static_cast<CookedKind>(kind);
    return entry;
}

} // namespace Assisi::Core
