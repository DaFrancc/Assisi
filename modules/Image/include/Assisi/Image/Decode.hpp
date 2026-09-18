/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Image/Decode.hpp
/// @brief Image files to RGBA8 mip chains.
///
/// Pure CPU over bytes read through Core::AssetSystem, so it runs on a worker or
/// in an offline tool with no device in scope. Output is always Rgba8; block
/// compression is Compress.hpp's, so a caller may decode without encoding.

#include <expected>
#include <string_view>
#include <vector>

#include <Assisi/Core/Errors.hpp>
#include <Assisi/Image/Image.hpp>

namespace Assisi::Image
{

/// @brief Decode the image at virtual asset path @p vpath into an RGBA8 mip chain.
///
/// Four channels, top-down to match Vulkan's V=0. @p colorSpace selects the space
/// the mips are filtered in — an sRGB source is filtered linearly — and travels
/// into the result. Each level resizes from the base, not from the level above,
/// so filter error does not compound.
///
/// @return the chain, or an AssetError if the file cannot be resolved, read or
///         decoded.
[[nodiscard]] std::expected<DecodedImage, Core::AssetError> DecodeImage(std::string_view vpath,
                                                                        ColorSpace colorSpace) noexcept;

/// @brief Decode every frame of an animated WebP at @p vpath, in playback order.
///
/// Disposal and blending are composited during the decode, so each frame is a
/// full canvas-sized RGBA image with one mip level — these are drawn near native
/// size. A still WebP gives a one-element list. A frame that fails partway keeps
/// what already decoded; only an empty result is an error.
[[nodiscard]] std::expected<std::vector<DecodedImage>, Core::AssetError>
DecodeAnimatedWebp(std::string_view vpath, ColorSpace colorSpace = ColorSpace::Srgb) noexcept;

/// @brief Fill @p image's chain from the RGBA8 texels at @p base. @p image must
///        already carry its dimensions and colour space.
///
/// A level that fails to downsample truncates the chain rather than leaving a
/// hole: an unwritten level still reaches the GPU, from memory nobody zeroed.
void BuildMipChain(DecodedImage &image, const unsigned char *base, bool generateMips);

} // namespace Assisi::Image
