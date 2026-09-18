/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Image/Compress.hpp
/// @brief RGBA8 to block-compressed, and back.
///
/// Block compression is fixed-rate: encoded size follows from the dimensions and
/// the format alone. Only quality varies with the content, which is why the
/// format is chosen per channel — a colour map and a normal map fail differently
/// under the wrong one.
///
/// Nothing here reads a file or touches a device, so one call serves a
/// build-time tool and a worker thread.

#include <cstdint>
#include <expected>

#include <Assisi/Core/Errors.hpp>
#include <Assisi/Image/Image.hpp>

namespace Assisi::Image
{

/// @brief How hard the encoder searches. Same output format either way; only
///        encode time and quality differ.
enum class CompressQuality : std::uint8_t
{
    Fast, ///< For an encode somebody is waiting on.
    Best, ///< For an encode that happens once, offline.
    Count,
};

/// @brief Encode every level of @p source into @p format.
///
/// A partial block at an edge repeats the level's last row and column rather than
/// zero-filling, so the padding cannot drag a block's fitted endpoints toward a
/// colour the image does not contain.
///
/// @return the encoded image, or FileReadFailed if @p source is not Rgba8 or
///         fails ValidateMipChain, or if an sRGB source is asked to become Bc4 or
///         Bc5 — those carry data, and there is no gamma to undo.
[[nodiscard]] std::expected<DecodedImage, Core::AssetError> Compress(const DecodedImage &source, PixelFormat format,
                                                                     CompressQuality quality);

/// @brief Decode @p source back to Rgba8, as far as a lossy format allows.
///
/// The only honest test of an encoder is to decode what it produced and compare,
/// and a cooker verifies its own output the same way. Channels the format does
/// not carry come back zero, with alpha opaque.
///
/// @return the decoded image, or FileReadFailed if @p source is not block
///         compressed or fails ValidateMipChain.
[[nodiscard]] std::expected<DecodedImage, Core::AssetError> Decompress(const DecodedImage &source);

} // namespace Assisi::Image
