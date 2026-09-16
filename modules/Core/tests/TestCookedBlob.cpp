/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCookedBlob.cpp
/// @brief The envelope every cooked asset carries: a round trip of each kind,
/// and a refusal for each of the four ways a blob can fail to be one.
///
/// A cooked blob is addressed by GUID and has no extension, so these four
/// refusals are the only thing standing between "these bytes are not what you
/// think" and a decode that reads them as whatever the caller expected.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/CookedBlob.hpp>

using Assisi::Core::BitReader;
using Assisi::Core::BitWriter;
using Assisi::Core::CookedBlobError;
using Assisi::Core::CookedKind;
using Assisi::Core::kCookedFormatVersion;
using Assisi::Core::kCookedMagic;
using Assisi::Core::ReadCookedHeader;
using Assisi::Core::ToString;
using Assisi::Core::WriteCookedHeader;

TEST_CASE("Every kind survives the header round trip")
{
    for (std::uint8_t raw = 0; raw < static_cast<std::uint8_t>(CookedKind::Count); ++raw)
    {
        const auto kind = static_cast<CookedKind>(raw);

        BitWriter writer;
        WriteCookedHeader(writer, kind);

        BitReader reader{writer.Data()};
        const auto read = ReadCookedHeader(reader);
        REQUIRE(read.has_value());
        CHECK(*read == kind);
        CHECK_FALSE(reader.Failed());
    }
}

TEST_CASE("The payload begins immediately after the header")
{
    // The whole reason the header is framed rather than fixed-offset: a cooker
    // writes its payload straight after this call and a reader picks it up
    // straight after its own.
    constexpr std::uint32_t kPayload = 0xDEADBEEFU;

    BitWriter writer;
    WriteCookedHeader(writer, CookedKind::Mesh);
    writer.WriteUInt32(kPayload);

    BitReader reader{writer.Data()};
    const auto kind = ReadCookedHeader(reader);
    REQUIRE(kind.has_value());
    CHECK(reader.ReadBits(32) == kPayload);
    CHECK_FALSE(reader.Failed());
}

TEST_CASE("A file that is not a cooked blob is refused on its first bytes")
{
    // A source asset copied into the cooked tree is the case this catches: it
    // would otherwise be read as whatever kind the caller went looking for.
    const std::vector<std::byte> notABlob{std::byte{'{'}, std::byte{'"'}, std::byte{'v'}, std::byte{'e'},
                                          std::byte{'r'}, std::byte{'s'}, std::byte{'i'}, std::byte{'o'}};

    BitReader reader{notABlob};
    const auto read = ReadCookedHeader(reader);
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == CookedBlobError::BadMagic);
}

TEST_CASE("A blob shorter than a header is refused as truncated, not as garbage")
{
    BitWriter writer;
    WriteCookedHeader(writer, CookedKind::Texture);

    const std::span<const std::byte> full = writer.Data();
    // Two bytes: inside the magic, so the read fails before there is anything
    // to compare against.
    const std::span<const std::byte> cut = full.first(2);

    BitReader reader{cut};
    const auto read = ReadCookedHeader(reader);
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == CookedBlobError::Truncated);
}

TEST_CASE("A future format version is refused rather than read as this one")
{
    BitWriter writer;
    writer.WriteUInt32(kCookedMagic);
    writer.WriteUInt8(kCookedFormatVersion + 1u);
    writer.WriteUInt8(static_cast<std::uint8_t>(CookedKind::Scene));

    BitReader reader{writer.Data()};
    const auto read = ReadCookedHeader(reader);
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == CookedBlobError::UnsupportedVersion);
}

TEST_CASE("A kind this build does not have is refused rather than cast")
{
    // Casting an unknown enumerator is the failure here: it would reach a switch
    // that has no case for it, and the value would be whatever the default did.
    BitWriter writer;
    writer.WriteUInt32(kCookedMagic);
    writer.WriteUInt8(kCookedFormatVersion);
    writer.WriteUInt8(static_cast<std::uint8_t>(CookedKind::Count));

    BitReader reader{writer.Data()};
    const auto read = ReadCookedHeader(reader);
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == CookedBlobError::UnknownKind);
}

TEST_CASE("Every kind and every error has a name")
{
    // The names reach a cook failure, which is the only thing a build log shows
    // about a blob nobody can open.
    for (std::uint8_t raw = 0; raw < static_cast<std::uint8_t>(CookedKind::Count); ++raw)
    {
        CHECK(ToString(static_cast<CookedKind>(raw)) != "unknown");
    }
    CHECK(ToString(CookedBlobError::Truncated) != "unknown");
    CHECK(ToString(CookedBlobError::BadMagic) != "unknown");
    CHECK(ToString(CookedBlobError::UnsupportedVersion) != "unknown");
    CHECK(ToString(CookedBlobError::UnknownKind) != "unknown");
}
