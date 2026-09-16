/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPakFormat.cpp
/// @brief The pak header and index entries read back as written, at the sizes a
/// reader addresses them by, and refuse bytes that are not a pak.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/PakFormat.hpp>

using namespace Assisi::Core;

namespace
{

PakEntry FilledEntry()
{
    PakEntry entry;
    entry.id               = DerivedAssetId("levels/a.alvl");
    entry.pathId           = DerivedAssetId("path");
    entry.offset           = 0x0102030405060708ULL;
    entry.storedSize       = 11;
    entry.uncompressedSize = 13;
    entry.contentHash      = 0xF0E0D0C0B0A09080ULL;
    entry.archive          = 0;
    entry.codec            = PakCodec::Zstd;
    entry.flags            = static_cast<std::uint8_t>(PakSliceFlag::Encrypted);
    entry.kind             = CookedKind::Scene;
    return entry;
}

std::vector<std::byte> ToVector(std::span<const std::byte> bytes)
{
    return {bytes.begin(), bytes.end()};
}

} // namespace

TEST_CASE("A pak header reads back as written, at its declared size")
{
    PakHeader header;
    header.entryCount  = 42;
    header.indexOffset = 0x1122334455ULL;

    BitWriter writer;
    WritePakHeader(writer, header);
    // A reader takes exactly this many bytes before it knows anything else.
    REQUIRE(writer.Data().size() == kPakHeaderBytes);

    const std::expected<PakHeader, PakFormatError> read = ReadPakHeader(writer.Data());
    REQUIRE(read.has_value());
    CHECK(read->entryCount == header.entryCount);
    CHECK(read->indexOffset == header.indexOffset);
}

TEST_CASE("A pak entry reads back as written, at its declared size")
{
    const PakEntry entry = FilledEntry();

    BitWriter writer;
    WritePakEntry(writer, entry);
    // The index is addressed by position, so every entry is exactly this size.
    REQUIRE(writer.Data().size() == kPakEntryBytes);

    BitReader reader{writer.Data()};
    const std::expected<PakEntry, PakFormatError> read = ReadPakEntry(reader);
    REQUIRE(read.has_value());
    CHECK(read->id == entry.id);
    CHECK(read->pathId == entry.pathId);
    CHECK(read->offset == entry.offset);
    CHECK(read->storedSize == entry.storedSize);
    CHECK(read->uncompressedSize == entry.uncompressedSize);
    CHECK(read->contentHash == entry.contentHash);
    CHECK(read->archive == entry.archive);
    CHECK(read->codec == entry.codec);
    CHECK(read->flags == entry.flags);
    CHECK(read->kind == entry.kind);
}

TEST_CASE("Bytes that are not a pak are refused on the magic")
{
    std::vector<std::byte> bytes(kPakHeaderBytes, std::byte{0x5A});
    const std::expected<PakHeader, PakFormatError> read = ReadPakHeader(bytes);
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == PakFormatError::NotAPak);
}

TEST_CASE("A pak of a layout version this build does not read is refused")
{
    PakHeader header;
    header.version = kPakFormatVersion + 1;
    BitWriter writer;
    WritePakHeader(writer, header);

    const std::expected<PakHeader, PakFormatError> read = ReadPakHeader(writer.Data());
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == PakFormatError::UnsupportedVersion);
}

TEST_CASE("A short header or entry is refused")
{
    BitWriter header;
    WritePakHeader(header, PakHeader{});
    const std::vector<std::byte> headerBytes = ToVector(header.Data());
    CHECK_FALSE(ReadPakHeader(std::span<const std::byte>{headerBytes.data(), kPakHeaderBytes - 1}).has_value());

    BitWriter entry;
    WritePakEntry(entry, FilledEntry());
    const std::vector<std::byte> entryBytes = ToVector(entry.Data());
    BitReader reader{std::span<const std::byte>{entryBytes.data(), kPakEntryBytes - 1}};
    CHECK_FALSE(ReadPakEntry(reader).has_value());
}

TEST_CASE("An entry naming a kind no writer produces is refused")
{
    BitWriter writer;
    WritePakEntry(writer, FilledEntry());
    std::vector<std::byte> bytes = ToVector(writer.Data());
    bytes.back() = std::byte{static_cast<std::uint8_t>(CookedKind::Count)};

    BitReader reader{bytes};
    const std::expected<PakEntry, PakFormatError> read = ReadPakEntry(reader);
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == PakFormatError::Corrupt);
}
