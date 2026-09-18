/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file CookedTexture.hpp
/// @brief A texture as a cooked blob: the mip levels a device uploads directly.
///
/// The writer and the reader live together so the layout has one definition. The
/// cooker compresses once, offline, and writes through WriteCookedTexture; a load
/// reads through ReadCookedTexture and runs no decode or encode.

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Image/Image.hpp>

namespace Assisi::Image
{

/// @brief Version of the texture payload's layout, separate from the blob envelope's.
///
/// Part of the texture cooker's cache key, so bumping it re-cooks every texture.
inline constexpr std::uint8_t kTexturePayloadVersion = 1;

/// @brief Why bytes did not read as a cooked texture.
enum class CookedTextureError : std::uint8_t
{
    NotATexture,        ///< Not a cooked blob, or a blob of another kind.
    UnsupportedVersion, ///< A texture layout this build does not read.
    Truncated,          ///< The bytes end, or a length claims more than is left.
    Invalid,            ///< Framed correctly, but the levels do not fit the format and size.
};

/// @brief A short human-readable description, for a load failure's log line.
[[nodiscard]] std::string_view ToString(CookedTextureError error) noexcept;

/// @brief Write @p image as a complete cooked blob, envelope included.
void WriteCookedTexture(Core::BitWriter &writer, const DecodedImage &image);

/// @brief Read a blob written by WriteCookedTexture.
///
/// Every length is checked against the bytes left before anything is allocated,
/// and the mip chain is validated against the format and size it claims.
[[nodiscard]] std::expected<DecodedImage, CookedTextureError> ReadCookedTexture(std::span<const std::byte> bytes);

} // namespace Assisi::Image
