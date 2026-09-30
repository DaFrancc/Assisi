/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCookedBlob.cpp
/// @brief The envelope every cooked asset carries: a round trip of any kind,
/// and a refusal for each of the three ways a blob can fail to be one.
///
/// A cooked blob is addressed by GUID and has no extension, so these refusals
/// are the only thing standing between "these bytes are not what you think" and
/// a decode that reads them as whatever the caller expected.

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <vector>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/CookedBlob.hpp>

using Assisi::Core::AssetKindId;
using Assisi::Core::BitReader;
using Assisi::Core::BitWriter;
using Assisi::Core::BuiltInKindName;
using Assisi::Core::CookedBlobError;
using Assisi::Core::kCookedFormatVersion;
using Assisi::Core::kCookedMagic;
using Assisi::Core::ReadCookedHeader;
using Assisi::Core::ToString;
using Assisi::Core::WriteCookedHeader;

namespace
{

constexpr std::array kBuiltInKinds{
    Assisi::Core::kReflectedKind, Assisi::Core::kSceneKind,  Assisi::Core::kMeshKind,
    Assisi::Core::kTextureKind,   Assisi::Core::kShaderKind, Assisi::Core::kVerbatimKind,
    Assisi::Core::kFontKind,      Assisi::Core::kScreenKind, Assisi::Core::kStringTableKind};

} // namespace

TEST_CASE("Every built-in kind survives the header round trip")
{
    for (const AssetKindId kind : kBuiltInKinds)
    {
        BitWriter writer;
        WriteCookedHeader(writer, kind);

        BitReader reader{writer.Data()};
        const std::expected<AssetKindId, CookedBlobError> read = ReadCookedHeader(reader);
        REQUIRE(read.has_value());
        CHECK(*read == kind);
        CHECK_FALSE(reader.Failed());
    }
}

TEST_CASE("A kind this build has never heard of survives the header round trip")
{
    // Whether a kind is known is the reader's question, not the envelope's: a
    // module adds kinds the engine never names.
    constexpr AssetKindId kUnheardOf{"a kind no module in this build registers"};

    BitWriter writer;
    WriteCookedHeader(writer, kUnheardOf);

    BitReader reader{writer.Data()};
    const std::expected<AssetKindId, CookedBlobError> read = ReadCookedHeader(reader);
    REQUIRE(read.has_value());
    CHECK(*read == kUnheardOf);
}

TEST_CASE("A kind is its name: one name gives one kind, and the built-ins are all different")
{
    CHECK(AssetKindId{"mesh"} == Assisi::Core::kMeshKind);
    for (std::size_t i = 0; i < kBuiltInKinds.size(); ++i)
    {
        for (std::size_t j = i + 1; j < kBuiltInKinds.size(); ++j)
        {
            CHECK_FALSE(kBuiltInKinds[i] == kBuiltInKinds[j]);
        }
    }
}

TEST_CASE("The payload begins immediately after the header")
{
    // The whole reason the header is framed rather than fixed-offset: a cooker
    // writes its payload straight after this call and a reader picks it up
    // straight after its own.
    constexpr std::uint32_t kPayload = 0xDEADBEEFU;

    BitWriter writer;
    WriteCookedHeader(writer, Assisi::Core::kMeshKind);
    writer.WriteUInt32(kPayload);

    BitReader reader{writer.Data()};
    const std::expected<AssetKindId, CookedBlobError> kind = ReadCookedHeader(reader);
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
    const std::expected<AssetKindId, CookedBlobError> read = ReadCookedHeader(reader);
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == CookedBlobError::BadMagic);
}

TEST_CASE("A blob shorter than a header is refused as truncated, not as garbage")
{
    BitWriter writer;
    WriteCookedHeader(writer, Assisi::Core::kTextureKind);
    const std::span<const std::byte> full = writer.Data();

    // Every length short of the whole header: inside the magic, at the version,
    // and inside the kind.
    for (std::size_t length = 0; length < full.size(); ++length)
    {
        CAPTURE(length);
        BitReader reader{full.first(length)};
        const std::expected<AssetKindId, CookedBlobError> read = ReadCookedHeader(reader);
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error() == CookedBlobError::Truncated);
    }
}

TEST_CASE("Another format version is refused rather than read as this one")
{
    BitWriter writer;
    writer.WriteUInt32(kCookedMagic);
    writer.WriteUInt8(kCookedFormatVersion + 1u);
    writer.WriteUInt64(Assisi::Core::kSceneKind.hash);

    BitReader reader{writer.Data()};
    const std::expected<AssetKindId, CookedBlobError> read = ReadCookedHeader(reader);
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == CookedBlobError::UnsupportedVersion);
}

TEST_CASE("A blob from before kinds had names is refused as another version, not as truncated")
{
    // The first envelope stored the kind in one byte. Such a blob is shorter
    // than this header, and the version is what says why it does not read.
    constexpr std::uint8_t kFirstEnvelopeVersion = 1;
    BitWriter writer;
    writer.WriteUInt32(kCookedMagic);
    writer.WriteUInt8(kFirstEnvelopeVersion);
    writer.WriteUInt8(0);

    BitReader reader{writer.Data()};
    const std::expected<AssetKindId, CookedBlobError> read = ReadCookedHeader(reader);
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == CookedBlobError::UnsupportedVersion);
}

TEST_CASE("Every built-in kind and every error has a name")
{
    // The names reach a cook failure, which is the only thing a build log shows
    // about a blob nobody can open.
    for (const AssetKindId kind : kBuiltInKinds)
    {
        CHECK_FALSE(BuiltInKindName(kind).empty());
    }
    CHECK(BuiltInKindName(AssetKindId{"not built in"}).empty());
    CHECK(ToString(CookedBlobError::Truncated) != "unknown");
    CHECK(ToString(CookedBlobError::BadMagic) != "unknown");
    CHECK(ToString(CookedBlobError::UnsupportedVersion) != "unknown");
}
