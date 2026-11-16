/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Image/Decode.hpp
/// @brief Image files to RGBA8 mip chains.
///
/// Pure CPU over bytes read through Core::AssetSystem, so it runs on a worker or
/// in an offline tool with no device in scope. Output is always Rgba8; block
/// compression is Compress.hpp's, so a caller may decode without encoding.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
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

/// @brief Decode the image file at @p file, a path on disk rather than an
/// asset, into a single RGBA8 level in sRGB: for files that live outside the
/// asset tree, such as a test's reference pictures.
///
/// @return the image, or FileReadFailed if the file cannot be read or decoded.
[[nodiscard]] std::expected<DecodedImage, Core::AssetError> DecodeImageFile(const std::filesystem::path &file) noexcept;

/// @brief Decode @p vpath as a material's texture: DecodeImage, then each mip
///        kept at the base level's coverage at @p alphaCutoff when that is not
///        zero (PreserveAlphaCoverage).
///
/// The cook and the editor both decode through this, so a texture cooked for
/// the game and one the editor loads from its source agree.
[[nodiscard]] std::expected<DecodedImage, Core::AssetError> DecodeTexture(std::string_view vpath,
                                                                          ColorSpace colorSpace,
                                                                          float alphaCutoff) noexcept;

/// @brief An image's size in pixels.
struct ImageExtent
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

/// @brief The size of the image file held in @p bytes, read from its header
/// without decoding a pixel.
///
/// @return the size, or CorruptAsset when the bytes are no image this build
///         decodes.
[[nodiscard]] std::expected<ImageExtent, Core::AssetError> ReadImageExtent(std::span<const std::byte> bytes) noexcept;

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

/// @brief Rescale the alpha of every mip below the base of RGBA8 @p image so the
///        share of texels at or above @p cutoff is the base level's.
///
/// What a cutout material needs from its colour map's mips. Averaging alpha
/// pulls thin solid strokes, a blade of grass or a leaf's edge, below the cutoff
/// one level at a time, so a cutout surface thins and vanishes with distance;
/// scaling each level's alpha back up keeps its apparent density (Castaño,
/// "Computing Alpha Mipmaps", 2010). Colour is untouched. Nothing changes for a
/// cutoff of zero or less, an image entirely cut or entirely kept at its base,
/// or an image that is not RGBA8.
void PreserveAlphaCoverage(DecodedImage &image, float cutoff);

} // namespace Assisi::Image
