/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPakCodec.cpp
/// @brief Each slice codec returns the exact bytes it was given, and a slice that
/// does not decompress to its recorded size is refused rather than handed on.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <Assisi/Core/PakCodec.hpp>

using namespace Assisi::Core;

namespace
{

/// Repetitive enough that every codec actually shrinks it, with enough variety
/// that a codec copying one run over another would be caught.
std::vector<std::byte> Compressible()
{
    constexpr std::size_t kLength = 4096;
    constexpr std::size_t kPeriod = 37;
    std::vector<std::byte> bytes(kLength);
    for (std::size_t i = 0; i < kLength; ++i)
    {
        bytes[i] = static_cast<std::byte>((i % kPeriod) * 7);
    }
    return bytes;
}

} // namespace

TEST_CASE("Every slice codec round-trips its bytes exactly")
{
    const std::vector<std::byte> source = Compressible();
    for (const PakCodec codec : {PakCodec::None, PakCodec::Lz4, PakCodec::Zstd})
    {
        CAPTURE(static_cast<std::uint32_t>(codec));
        const std::expected<std::vector<std::byte>, PakCodecError> packed = CompressSlice(codec, source);
        REQUIRE(packed.has_value());
        if (codec != PakCodec::None)
        {
            CHECK(packed->size() < source.size());
        }

        const std::expected<std::vector<std::byte>, PakCodecError> unpacked =
            DecompressSlice(codec, *packed, source.size());
        REQUIRE(unpacked.has_value());
        CHECK(*unpacked == source);
    }
}

TEST_CASE("A slice that does not decompress to its recorded size is refused")
{
    // The size comes from the index and the bytes from the slice. A disagreement
    // means one of them is corrupt, and a loader handed the short buffer would
    // read the rest of an asset that is not there.
    const std::vector<std::byte> source = Compressible();
    for (const PakCodec codec : {PakCodec::None, PakCodec::Lz4, PakCodec::Zstd})
    {
        CAPTURE(static_cast<std::uint32_t>(codec));
        const std::expected<std::vector<std::byte>, PakCodecError> packed = CompressSlice(codec, source);
        REQUIRE(packed.has_value());
        CHECK_FALSE(DecompressSlice(codec, *packed, source.size() + 1).has_value());
        CHECK_FALSE(DecompressSlice(codec, *packed, source.size() - 1).has_value());
    }
}

TEST_CASE("Corrupt compressed bytes are refused, not decoded into garbage")
{
    const std::vector<std::byte> source = Compressible();
    for (const PakCodec codec : {PakCodec::Lz4, PakCodec::Zstd})
    {
        CAPTURE(static_cast<std::uint32_t>(codec));
        const std::expected<std::vector<std::byte>, PakCodecError> packed = CompressSlice(codec, source);
        REQUIRE(packed.has_value());

        const std::span<const std::byte> cut{packed->data(), packed->size() / 2};
        CHECK_FALSE(DecompressSlice(codec, cut, source.size()).has_value());
    }
}

TEST_CASE("A codec this build does not have is refused")
{
    const std::vector<std::byte> source = Compressible();
    const auto unknown = static_cast<PakCodec>(PakCodec::Count);

    const std::expected<std::vector<std::byte>, PakCodecError> unpacked =
        DecompressSlice(unknown, source, source.size());
    REQUIRE_FALSE(unpacked.has_value());
    CHECK(unpacked.error() == PakCodecError::UnknownCodec);
}
