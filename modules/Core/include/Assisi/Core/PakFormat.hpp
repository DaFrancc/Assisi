/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PakFormat.hpp
/// @brief The pak archive's layout: a fixed header, the slices, then the index.
///
/// The header comes first and is a fixed size, so a reader finds everything else
/// with one small read. The index comes last because a slice's offset is only
/// known once every slice before it has been written, and writing the index at
/// the end lets the writer stream slices without seeking back.
///
/// Every field is written at a fixed width, so the index is a flat array a reader
/// takes in one read and addresses by position.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Core/PakCodec.hpp>

namespace Assisi::Core
{

/// @brief Leading bytes of every pak, as a little-endian `uint32`: "APAK" in a
///        hex dump. A file that is not a pak fails on its first four bytes.
inline constexpr std::uint32_t kPakMagic = 0x4B415041U;

/// @brief Version of the layout below, bumped when any field changes.
inline constexpr std::uint16_t kPakFormatVersion = 1;

/// @brief Bytes the header occupies on disk.
inline constexpr std::size_t kPakHeaderBytes = sizeof(std::uint32_t) + sizeof(std::uint16_t) +
                                               sizeof(std::uint32_t) + sizeof(std::uint64_t);

/// @brief Bytes one index entry occupies on disk.
inline constexpr std::size_t kPakEntryBytes =
    sizeof(AssetId::bytes) + sizeof(AssetId::bytes) +                    // id, pathId
    sizeof(std::uint64_t) + sizeof(std::uint64_t) +                      // offset, storedSize
    sizeof(std::uint64_t) + sizeof(std::uint64_t) +                      // uncompressedSize, contentHash
    sizeof(std::uint16_t) +                                              // archive
    sizeof(std::uint8_t) + sizeof(std::uint8_t) + sizeof(std::uint8_t); // codec, flags, kind

/// @brief Bits in a slice's flags byte.
enum class PakSliceFlag : std::uint8_t
{
    /// The stored bytes are encrypted, after compression. No cipher exists yet,
    /// so a reader refuses such a slice rather than decompressing ciphertext.
    Encrypted = 1U << 0U,
};

/// @brief The fixed block at the start of a pak.
struct PakHeader
{
    std::uint64_t indexOffset = 0; ///< Where the index begins.
    std::uint32_t magic       = kPakMagic;
    std::uint32_t entryCount  = 0;
    std::uint16_t version     = kPakFormatVersion;
};

/// @brief One slice's row in the index.
struct PakEntry
{
    AssetId id;     ///< The asset this slice holds.
    AssetId pathId; ///< DerivedAssetId of its virtual path, so a lookup by path needs no string.

    std::uint64_t offset           = 0; ///< Where the stored bytes start in the archive.
    std::uint64_t storedSize       = 0; ///< Bytes on disk, after compression.
    std::uint64_t uncompressedSize = 0; ///< Bytes a loader receives.

    /// Hash of the cooked bytes, as the cook recorded it. Lets a content check
    /// compare two paks without reading a single slice.
    std::uint64_t contentHash = 0;

    /// Which archive holds the slice. One archive exists today, so anything else
    /// is refused rather than read from the wrong file.
    std::uint16_t archive = 0;

    PakCodec codec     = PakCodec::None;
    std::uint8_t flags = 0;
    CookedKind kind    = CookedKind::Verbatim;
};

/// @brief Why bytes did not read as a pak header or index.
enum class PakFormatError : std::uint8_t
{
    NotAPak,            ///< The magic is wrong.
    UnsupportedVersion, ///< A layout version this build does not read.
    Truncated,          ///< Fewer bytes than the header or the index needs.
    Corrupt,            ///< A field holds a value no writer produces.
};

/// @brief A short human-readable description, for a load failure's log line.
[[nodiscard]] std::string_view ToString(PakFormatError error) noexcept;

void WritePakHeader(BitWriter &writer, const PakHeader &header);

/// @brief Reads and validates the header from the first kPakHeaderBytes of a pak.
[[nodiscard]] std::expected<PakHeader, PakFormatError> ReadPakHeader(std::span<const std::byte> bytes);

void WritePakEntry(BitWriter &writer, const PakEntry &entry);

/// @brief Reads one entry.
///
/// A codec value or flag this build does not know is kept, because it describes
/// one slice and only that slice's open should fail. A kind outside the table is
/// refused, because no writer produces one.
[[nodiscard]] std::expected<PakEntry, PakFormatError> ReadPakEntry(BitReader &reader);

} // namespace Assisi::Core
