/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Core/PakCodec.hpp>

#include <limits>

#include <lz4.h>
#include <lz4hc.h>
#include <zstd.h>

namespace Assisi::Core
{

namespace
{

/// The highest lz4 HC level. Compression time is paid once at pack time, and
/// decompression speed does not depend on the level.
constexpr std::int32_t kLz4Level = LZ4HC_CLEVEL_MAX;

/// A high zstd level short of the ultra range, whose extra memory at decompress
/// time a game would pay on every load.
constexpr std::int32_t kZstdLevel = 19;

/// lz4's API takes sizes as a 32-bit signed count, so a slice past this cannot go
/// through it.
constexpr std::size_t kLz4MaxBytes = static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());

std::expected<std::vector<std::byte>, PakCodecError> CompressLz4(std::span<const std::byte> bytes)
{
    if (bytes.size() > kLz4MaxBytes)
    {
        return std::unexpected(PakCodecError::Failed);
    }
    const std::int32_t bound = LZ4_compressBound(static_cast<std::int32_t>(bytes.size()));
    std::vector<std::byte> out(static_cast<std::size_t>(bound));
    const std::int32_t written =
        LZ4_compress_HC(reinterpret_cast<const char *>(bytes.data()), reinterpret_cast<char *>(out.data()),
                        static_cast<std::int32_t>(bytes.size()), bound, kLz4Level);
    if (written <= 0)
    {
        return std::unexpected(PakCodecError::Failed);
    }
    out.resize(static_cast<std::size_t>(written));
    return out;
}

std::expected<std::vector<std::byte>, PakCodecError> DecompressLz4(std::span<const std::byte> bytes,
                                                                   std::size_t uncompressedSize)
{
    if (bytes.size() > kLz4MaxBytes || uncompressedSize > kLz4MaxBytes)
    {
        return std::unexpected(PakCodecError::Failed);
    }
    std::vector<std::byte> out(uncompressedSize);
    const std::int32_t read =
        LZ4_decompress_safe(reinterpret_cast<const char *>(bytes.data()), reinterpret_cast<char *>(out.data()),
                            static_cast<std::int32_t>(bytes.size()), static_cast<std::int32_t>(uncompressedSize));
    if (read < 0)
    {
        return std::unexpected(PakCodecError::Failed);
    }
    if (static_cast<std::size_t>(read) != uncompressedSize)
    {
        return std::unexpected(PakCodecError::SizeMismatch);
    }
    return out;
}

std::expected<std::vector<std::byte>, PakCodecError> CompressZstd(std::span<const std::byte> bytes)
{
    std::vector<std::byte> out(ZSTD_compressBound(bytes.size()));
    const std::size_t written = ZSTD_compress(out.data(), out.size(), bytes.data(), bytes.size(), kZstdLevel);
    if (ZSTD_isError(written) != 0)
    {
        return std::unexpected(PakCodecError::Failed);
    }
    out.resize(written);
    return out;
}

std::expected<std::vector<std::byte>, PakCodecError> DecompressZstd(std::span<const std::byte> bytes,
                                                                    std::size_t uncompressedSize)
{
    // The frame records its own size, which is checked against the index before
    // anything is allocated: a forged frame claiming gigabytes allocates nothing.
    const std::uint64_t frameSize = ZSTD_getFrameContentSize(bytes.data(), bytes.size());
    if (frameSize == ZSTD_CONTENTSIZE_ERROR || frameSize == ZSTD_CONTENTSIZE_UNKNOWN)
    {
        return std::unexpected(PakCodecError::Failed);
    }
    if (frameSize != uncompressedSize)
    {
        return std::unexpected(PakCodecError::SizeMismatch);
    }

    std::vector<std::byte> out(uncompressedSize);
    const std::size_t read = ZSTD_decompress(out.data(), out.size(), bytes.data(), bytes.size());
    if (ZSTD_isError(read) != 0)
    {
        return std::unexpected(PakCodecError::Failed);
    }
    if (read != uncompressedSize)
    {
        return std::unexpected(PakCodecError::SizeMismatch);
    }
    return out;
}

} // namespace

std::string_view ToString(PakCodecError error) noexcept
{
    switch (error)
    {
    case PakCodecError::UnknownCodec:
        return "a slice codec this build does not have";
    case PakCodecError::Failed:
        return "the slice's bytes did not compress or decompress";
    case PakCodecError::SizeMismatch:
        return "the slice decompressed to a size other than the index records";
    }
    return "unknown";
}

std::string_view ToString(PakCodec codec) noexcept
{
    switch (codec)
    {
    case PakCodec::None:
        return "none";
    case PakCodec::Lz4:
        return "lz4";
    case PakCodec::Zstd:
        return "zstd";
    case PakCodec::Count:
        break;
    }
    return {};
}

std::expected<std::vector<std::byte>, PakCodecError> CompressSlice(PakCodec codec, std::span<const std::byte> bytes)
{
    switch (codec)
    {
    case PakCodec::None:
        return std::vector<std::byte>{bytes.begin(), bytes.end()};
    case PakCodec::Lz4:
        return CompressLz4(bytes);
    case PakCodec::Zstd:
        return CompressZstd(bytes);
    case PakCodec::Count:
        break;
    }
    return std::unexpected(PakCodecError::UnknownCodec);
}

std::expected<std::vector<std::byte>, PakCodecError> DecompressSlice(PakCodec codec, std::span<const std::byte> bytes,
                                                                     std::size_t uncompressedSize)
{
    switch (codec)
    {
    case PakCodec::None:
        if (bytes.size() != uncompressedSize)
        {
            return std::unexpected(PakCodecError::SizeMismatch);
        }
        return std::vector<std::byte>{bytes.begin(), bytes.end()};
    case PakCodec::Lz4:
        return DecompressLz4(bytes, uncompressedSize);
    case PakCodec::Zstd:
        return DecompressZstd(bytes, uncompressedSize);
    case PakCodec::Count:
        break;
    }
    return std::unexpected(PakCodecError::UnknownCodec);
}

} // namespace Assisi::Core
