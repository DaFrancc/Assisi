/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PakCodec.hpp
/// @brief Compressing and decompressing one pak slice.
///
/// Per slice rather than per archive: a slice is read on whichever worker wants
/// that asset, and decompressing it must not depend on any other slice having
/// been read first.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace Assisi::Core
{

/// @brief How a slice's bytes are stored. Recorded per slice in the pak index.
enum class PakCodec : std::uint8_t
{
    None, ///< Stored as cooked.
    Lz4,  ///< Fast to decompress; the choice when load time matters most.
    Zstd, ///< Smaller than lz4, slower to decompress; the choice when size matters most.
    Count,
};

/// @brief Why a slice could not be compressed or decompressed.
enum class PakCodecError : std::uint8_t
{
    UnknownCodec, ///< A codec value this build does not have.
    Failed,       ///< The library refused the bytes: corrupt, truncated, or too large.
    SizeMismatch, ///< Decompressed to a size other than the one the index records.
};

/// @brief A short human-readable description, for a load failure's log line.
[[nodiscard]] std::string_view ToString(PakCodecError error) noexcept;

/// @brief The codec's name as the pack tool spells it, or empty for Count.
[[nodiscard]] std::string_view ToString(PakCodec codec) noexcept;

/// @brief @p bytes compressed with @p codec, at the codec's highest ratio: a pak
///        is written once, offline, and read many times.
[[nodiscard]] std::expected<std::vector<std::byte>, PakCodecError> CompressSlice(PakCodec codec,
                                                                                 std::span<const std::byte> bytes);

/// @brief @p bytes decompressed with @p codec, which must yield exactly
///        @p uncompressedSize bytes.
[[nodiscard]] std::expected<std::vector<std::byte>, PakCodecError>
DecompressSlice(PakCodec codec, std::span<const std::byte> bytes, std::size_t uncompressedSize);

} // namespace Assisi::Core
