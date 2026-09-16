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
/// one covers the envelope, so a change to how kinds are numbered or framed is
/// caught even when the payload inside is untouched.
///
/// Nothing here touches the filesystem — the cooker writes these bytes and a
/// provider reads them back, and neither has to agree about where they live.

#include <cstdint>
#include <expected>
#include <string_view>

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
inline constexpr std::uint8_t kCookedFormatVersion = 1;

/// @brief What a cooked blob's payload is, so a reader dispatches on the bytes
///        rather than on where the bytes were found.
enum class CookedKind : std::uint8_t
{
    Reflected, ///< A reflected asset type's fields (`.amat`, the config files).
    Scene,     ///< A level or a blueprint: entities, components, instances.
    Mesh,      ///< Vertex and index arenas with the submesh, LOD and slot tables.
    Texture,   ///< Block-compressed mips, ready for upload with no decode.
    Shader,    ///< SPIR-V, exactly as the compiler emitted it.
    Verbatim,  ///< Bytes with no cooker of their own — a font, an animated WebP.
    Count,
};

/// @brief A short human-readable name for a kind (for logs and cook failures).
[[nodiscard]] std::string_view ToString(CookedKind kind) noexcept;

/// @brief Why a cooked blob's header could not be read.
enum class CookedBlobError : std::uint8_t
{
    Truncated,          ///< Fewer bytes than a header needs.
    BadMagic,           ///< The leading bytes are not kCookedMagic.
    UnsupportedVersion, ///< A version this build does not read.
    UnknownKind,        ///< A kind enumerator this build does not have.
};

/// @brief A short human-readable description of a header failure.
[[nodiscard]] std::string_view ToString(CookedBlobError error) noexcept;

/// @brief Write the envelope. The payload follows immediately, written by
///        whichever cooker owns @p kind.
void WriteCookedHeader(BitWriter &writer, CookedKind kind);

/// @brief Read and validate the envelope, leaving @p reader positioned at the
///        payload.
///
/// Every failure is a value rather than a partially-consumed reader the caller
/// has to reason about: on an error the reader has been advanced and must not be
/// used, which is why the kind comes back by value and not through an out param.
[[nodiscard]] std::expected<CookedKind, CookedBlobError> ReadCookedHeader(BitReader &reader);

} // namespace Assisi::Core
