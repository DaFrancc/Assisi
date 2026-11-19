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
    entry.path             = "levels/a.alvl";
    entry.id               = DerivedAssetId("levels/a.alvl");
    entry.offset           = 0x0102030405060708ULL;
    entry.storedSize       = 11;
    entry.uncompressedSize = 13;
    entry.contentHash      = 0xF0E0D0C0B0A09080ULL;
    entry.archive          = 0;
    entry.codec            = PakCodec::Zstd;
    entry.flags            = static_cast<std::uint8_t>(PakSliceFlag::Encrypted);
    entry.kind             = kSceneKind;
    return entry;
}

std::vector<std::byte> ToVector(std::span<const std::byte> bytes)
{
    return {bytes.begin(), bytes.end()};
}

/// The header an index of @p entries is read with.
PakHeader HeaderFor(std::span<const PakEntry> entries)
{
    PakHeader header;
    header.entryCount = static_cast<std::uint32_t>(entries.size());
    header.pathBytes  = PakPathBytes(entries);
    return header;
}

/// @p entries written as an index and read back, or the reason it did not read.
std::expected<std::vector<PakEntry>, PakFormatError> RoundTrip(std::span<const PakEntry> entries)
{
    BitWriter writer;
    WritePakIndex(writer, entries);
    return ReadPakIndex(writer.Data(), HeaderFor(entries));
}

} // namespace

TEST_CASE("A pak header reads back as written, at its declared size")
{
    PakHeader header;
    header.entryCount  = 42;
    header.indexOffset = 0x1122334455ULL;
    header.pathBytes   = 0x0A0B0C0DU;

    BitWriter writer;
    WritePakHeader(writer, header);
    // A reader takes exactly this many bytes before it knows anything else.
    REQUIRE(writer.Data().size() == kPakHeaderBytes);

    const std::expected<PakHeader, PakFormatError> read = ReadPakHeader(writer.Data());
    REQUIRE(read.has_value());
    CHECK(read->entryCount == header.entryCount);
    CHECK(read->indexOffset == header.indexOffset);
    CHECK(read->pathBytes == header.pathBytes);
}

TEST_CASE("A pak entry reads back as written, at its declared size")
{
    const std::vector<PakEntry> entries{FilledEntry()};
    const PakEntry &entry = entries.front();

    BitWriter writer;
    WritePakIndex(writer, entries);
    // The entries are addressed by position, so every one is exactly this size,
    // and the path follows them.
    REQUIRE(writer.Data().size() == kPakEntryBytes + entry.path.size());

    const std::expected<std::vector<PakEntry>, PakFormatError> index = RoundTrip(entries);
    REQUIRE(index.has_value());
    REQUIRE(index->size() == 1);
    const PakEntry *read = &index->front();
    CHECK(read->path == entry.path);
    CHECK(read->id == entry.id);
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

TEST_CASE("A short header or index is refused")
{
    BitWriter header;
    WritePakHeader(header, PakHeader{});
    const std::vector<std::byte> headerBytes = ToVector(header.Data());
    CHECK_FALSE(ReadPakHeader(std::span<const std::byte>{headerBytes.data(), kPakHeaderBytes - 1}).has_value());

    const std::vector<PakEntry> entries{FilledEntry()};
    BitWriter index;
    WritePakIndex(index, entries);
    const std::vector<std::byte> indexBytes = ToVector(index.Data());
    const std::expected<std::vector<PakEntry>, PakFormatError> read =
        ReadPakIndex(std::span<const std::byte>{indexBytes.data(), indexBytes.size() - 1}, HeaderFor(entries));
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == PakFormatError::Truncated);
}

TEST_CASE("Every entry keeps its own path, whatever the order and lengths")
{
    // A path is found by its offset into the table, so an offset taken from the
    // wrong entry, or a length off by one, hands one asset another's name.
    std::vector<PakEntry> entries(3, FilledEntry());
    entries[0].path = "blueprints/Player.abp";
    entries[1].path = "a";
    entries[2].path = "levels/forest/Clearing.alvl";

    const std::expected<std::vector<PakEntry>, PakFormatError> read = RoundTrip(entries);
    REQUIRE(read.has_value());
    REQUIRE(read->size() == entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        CHECK((*read)[i].path == entries[i].path);
    }
}

TEST_CASE("An index whose path lies outside the path table is refused")
{
    // Written for a longer path than the table then holds: the header says the
    // table ends early, so the entry points past it.
    const std::vector<PakEntry> entries{FilledEntry()};
    BitWriter writer;
    WritePakIndex(writer, entries);
    PakHeader header = HeaderFor(entries);
    header.pathBytes -= 1;
    const std::vector<std::byte> bytes = ToVector(writer.Data());

    const std::expected<std::vector<PakEntry>, PakFormatError> read =
        ReadPakIndex(std::span<const std::byte>{bytes.data(), bytes.size() - 1}, header);
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == PakFormatError::BadPath);
}

TEST_CASE("An entry of a kind this build has never heard of reads back as written")
{
    // A module can add a kind the engine does not know, and the pak index must
    // carry it rather than refuse the whole package.
    constexpr AssetKindId kUnheardOf{"a kind no module in this build registers"};
    std::vector<PakEntry> entries{FilledEntry()};
    entries.front().kind = kUnheardOf;

    const std::expected<std::vector<PakEntry>, PakFormatError> read = RoundTrip(entries);
    REQUIRE(read.has_value());
    CHECK(read->front().kind == kUnheardOf);
}
