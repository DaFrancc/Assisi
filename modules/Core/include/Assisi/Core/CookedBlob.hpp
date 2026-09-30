/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file CookedBlob.hpp
/// @brief The header every cooked asset starts with: magic, format version, and
///        what kind of payload follows.
///
/// A cooked blob is addressed by GUID and carries no file extension, so the
/// bytes are the only thing that says what they are. Without a header a mesh
/// handed to the texture path would be read as dimensions and mip data, and the
/// first sign of trouble would be a GPU upload of nonsense.
///
/// The version is separate from any payload's own versioning on purpose: this
/// one covers the envelope, so a change to how kinds are named or framed is
/// caught even when the payload inside is untouched.
///
/// Nothing here touches the filesystem — the cooker writes these bytes and a
/// provider reads them back, and neither has to agree about where they live.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/BitStream.hpp>

namespace Assisi::Core
{

/// @brief Leading bytes of every cooked blob, as a little-endian `uint32`.
///
/// Spells "ACKD" read as ASCII in a hex dump. Its job is to make a file that is
/// not a cooked blob at all — a source asset copied into the cooked tree, a
/// truncated write, a blob from a tool that is not this one — fail on its first
/// four bytes instead of somewhere inside a decode.
inline constexpr std::uint32_t kCookedMagic = 0x444B4341U;

/// @brief Version of the envelope below, bumped when its framing changes.
inline constexpr std::uint8_t kCookedFormatVersion = 2;

/// @brief Bytes the envelope occupies: magic, version, kind. The payload starts here.
inline constexpr std::size_t kCookedHeaderBytes = sizeof(std::uint32_t) + sizeof(std::uint8_t) + sizeof(std::uint64_t);

/// @brief Which kind of asset a cooked blob holds — mesh, texture, sound — so a
///        reader dispatches on the bytes rather than on where they were found.
///
/// A hash of the kind's name rather than a number from one list, so a module
/// can add a kind without editing this file. The name is the identity: two
/// kinds with one name are one kind.
struct AssetKindId
{
    std::uint64_t hash = 0;

    constexpr AssetKindId() = default;

    /// @brief The id of the kind called @p name: 64-bit FNV-1a over its bytes.
    constexpr explicit AssetKindId(std::string_view name) : hash(kFnvOffsetBasis)
    {
        for (const char c : name)
        {
            hash ^= static_cast<std::uint8_t>(c);
            hash *= kFnvPrime;
        }
    }

    friend constexpr bool operator==(AssetKindId, AssetKindId) = default;

  private:
    static constexpr std::uint64_t kFnvOffsetBasis = 0xCBF29CE484222325ULL;
    static constexpr std::uint64_t kFnvPrime = 0x100000001B3ULL;
};

// The kinds the engine cooks itself. A module's own kind is declared beside
// its registration, the same way.

/// A reflected asset type's fields (`.amat`, the config files).
inline constexpr AssetKindId kReflectedKind{"reflected"};
/// A level or a blueprint: entities, components, instances.
inline constexpr AssetKindId kSceneKind{"scene"};
/// Vertex and index arenas with the submesh, LOD and slot tables.
inline constexpr AssetKindId kMeshKind{"mesh"};
/// Block-compressed mips, ready for upload with no decode.
inline constexpr AssetKindId kTextureKind{"texture"};
/// SPIR-V, exactly as the compiler emitted it.
inline constexpr AssetKindId kShaderKind{"shader"};
/// Bytes with no cooker of their own — an animated WebP, a licence text.
inline constexpr AssetKindId kVerbatimKind{"verbatim"};
/// A glyph atlas with its metrics and kerning.
inline constexpr AssetKindId kFontKind{"font"};
/// A flat node table: one UI screen, with the markup compiled away.
inline constexpr AssetKindId kScreenKind{"screen"};
/// One table of UI strings, by key.
inline constexpr AssetKindId kStringTableKind{"string table"};

/// @brief The name of a kind the engine cooks itself, or empty for any other kind.
[[nodiscard]] std::string_view BuiltInKindName(AssetKindId kind) noexcept;

/// @brief A kind's name for a log line or a cook failure: the built-in name, or
///        the name AssetKindRegistry holds, or the hash when this build has no
///        such kind.
[[nodiscard]] std::string DescribeKind(AssetKindId kind);

/// @brief Why a cooked blob's header could not be read.
enum class CookedBlobError : std::uint8_t
{
    Truncated,          ///< Fewer bytes than a header needs.
    BadMagic,           ///< The leading bytes are not kCookedMagic.
    UnsupportedVersion, ///< A version this build does not read.
};

/// @brief A short human-readable description of a header failure.
[[nodiscard]] std::string_view ToString(CookedBlobError error) noexcept;

/// @brief Write the envelope. The payload follows immediately, written by
///        whichever cooker owns @p kind.
void WriteCookedHeader(BitWriter &writer, AssetKindId kind);

/// @brief Read and validate the envelope, leaving @p reader positioned at the
///        payload.
///
/// Every failure is a value rather than a partially-consumed reader the caller
/// has to reason about: on an error the reader has been advanced and must not be
/// used, which is why the kind comes back by value and not through an out param.
///
/// Any kind reads back, including one this build has never heard of: whether a
/// kind is known is the question of whoever reads the payload, and a package can
/// hold kinds only some builds load.
[[nodiscard]] std::expected<AssetKindId, CookedBlobError> ReadCookedHeader(BitReader &reader);

/// @brief Write an asset id as its sixteen raw bytes, in string order.
void WriteAssetId(BitWriter &writer, const AssetId &id);

/// @brief Read an asset id written by WriteAssetId. On a short read the reader
///        reports Failed() and the id is partial, so check the reader first.
[[nodiscard]] AssetId ReadAssetId(BitReader &reader);

} // namespace Assisi::Core
