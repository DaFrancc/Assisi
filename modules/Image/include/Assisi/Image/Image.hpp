/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Image/Image.hpp
/// @brief A CPU-side image and the arithmetic that says how its levels are laid
///        out in memory.
///
/// One type carries both an uncompressed RGBA8 image and a block-compressed one,
/// because both travel the same route: decoded or encoded on a worker, uploaded
/// on the main thread. Splitting them would duplicate that route for the sake of
/// a distinction the upload does not care about beyond a pitch.
///
/// The layout functions are here rather than at the upload because that is the
/// one place they can be tested. A graphics API derives its own pitch from the
/// texture's format and copies that many bytes out of whatever the caller hands
/// it — so a caller that computes the pitch wrongly produces a texture that
/// samples as noise, with no error anywhere. See LayoutFor.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Assisi::Image
{

/// @brief How an image's stored 8-bit values map to the linear values shaders
/// work in.
///
/// Colour and albedo maps are authored in sRGB (Srgb) so the GPU decodes them to
/// linear at sample time — which also makes hardware filtering and mip
/// generation correct, since both must happen in linear space. Data maps
/// (normals, metallic/roughness, masks) hold linear values already and must use
/// Linear so they are not gamma-mangled.
enum class ColorSpace : std::uint8_t
{
    Srgb,
    Linear,
};

/// @brief How an image's texels are stored.
///
/// Rgba8 is four bytes per texel, addressed by row. Every other value is a
/// block-compressed format: texels are grouped into 4x4 blocks, each block is a
/// fixed number of bytes whatever it contains, and a level is addressed by block
/// row. That fixed size is the point — a shader indexes a block in constant time,
/// so the encoded size depends only on the dimensions and never on the content.
enum class PixelFormat : std::uint8_t
{
    Rgba8, ///< Uncompressed, four bytes per texel.
    Bc4,   ///< One channel, 8 bytes per block.
    Bc5,   ///< Two channels, 16 bytes per block. Normal maps: X and Y stored, Z reconstructed.
    Bc7,   ///< Four channels, 16 bytes per block. The colour format.
    Count,
};

/// @brief Texels along each edge of one compressed block.
inline constexpr std::uint32_t kBlockExtent = 4;

/// @brief Bytes in one texel of an uncompressed image.
inline constexpr std::size_t kRgba8BytesPerTexel = 4;

/// @brief Bytes one 4x4 block of @p format occupies, or 0 for Rgba8.
[[nodiscard]] constexpr std::size_t BytesPerBlock(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::Bc4:
        return 8;
    case PixelFormat::Bc5:
    case PixelFormat::Bc7:
        return 16;
    case PixelFormat::Rgba8:
        return 0;
    // A block format missing from the cases above lands here and reads as
    // uncompressed, which is the wrong pitch rather than a refusal. This is
    // constexpr and cannot report, so a new format is added above or it is
    // silently wrong — IsBlockCompressed is defined in terms of this.
    default:
        return 0;
    }
}

/// @brief Whether @p format stores texels in 4x4 blocks rather than rows.
[[nodiscard]] constexpr bool IsBlockCompressed(PixelFormat format)
{
    return BytesPerBlock(format) != 0;
}

/// @brief Where one mip level's bytes sit: how many there are, and how far apart
///        consecutive rows are.
struct LevelLayout
{
    /// Total bytes the level occupies.
    std::size_t byteSize = 0;

    /// Bytes from the start of one row to the start of the next. For a
    /// block-compressed level a "row" is a row of blocks, covering four texel
    /// rows, so this is the pitch of a block row and not of a texel row.
    std::size_t rowPitch = 0;
};

/// @brief The layout of a @p width by @p height level of @p format.
///
/// A block-compressed level always covers a whole number of blocks, so its width
/// rounds up: a 5-texel-wide BC7 level is two blocks, and a 1-texel-wide one is
/// still a whole block. Every mip chain ends at 2x2 and 1x1, so that rounding is
/// not an edge case — it is the last two levels of every texture there is.
///
/// Rounding *down* would be the silent kind of wrong. For a 16-byte format at a
/// width divisible by four the uncompressed arithmetic `width * 4` gives exactly
/// the same answer as the block arithmetic, so a pitch computed the wrong way
/// agrees on every level of a power-of-two texture until the tail.
[[nodiscard]] constexpr LevelLayout LayoutFor(PixelFormat format, std::uint32_t width, std::uint32_t height)
{
    LevelLayout layout;
    if (IsBlockCompressed(format))
    {
        const std::size_t blocksWide = (static_cast<std::size_t>(width) + kBlockExtent - 1) / kBlockExtent;
        const std::size_t blocksHigh = (static_cast<std::size_t>(height) + kBlockExtent - 1) / kBlockExtent;
        layout.rowPitch = blocksWide * BytesPerBlock(format);
        layout.byteSize = layout.rowPitch * blocksHigh;
        return layout;
    }
    layout.rowPitch = static_cast<std::size_t>(width) * kRgba8BytesPerTexel;
    layout.byteSize = layout.rowPitch * height;
    return layout;
}

/// @brief Levels in a full chain from @p width by @p height down to 1x1.
[[nodiscard]] constexpr std::uint32_t MipLevelCount(std::uint32_t width, std::uint32_t height)
{
    std::uint32_t levels = 1;
    while (width > 1 || height > 1)
    {
        width  = width > 1 ? width >> 1U : 1U;
        height = height > 1 ? height >> 1U : 1U;
        ++levels;
    }
    return levels;
}

/// @brief The dimensions of mip @p level of a @p width by @p height image.
[[nodiscard]] constexpr std::uint32_t MipExtent(std::uint32_t extent, std::uint32_t level)
{
    const std::uint32_t shifted = extent >> level;
    return shifted > 1 ? shifted : 1U;
}

/// @brief A CPU-side image: its dimensions, how its texels are stored, and one
///        buffer per mip level.
///
/// `mips[0]` is the base level and each subsequent level halves, down to 1x1. A
/// level may legitimately be empty — a downsample that failed truncates the chain
/// rather than leaving a hole — but a non-empty one is exactly the size LayoutFor
/// gives for its dimensions.
struct DecodedImage
{
    std::vector<std::vector<unsigned char>> mips;

    std::uint32_t width  = 0;
    std::uint32_t height = 0;

    PixelFormat format     = PixelFormat::Rgba8;
    ColorSpace colorSpace = ColorSpace::Srgb;
};

/// @brief Whether every non-empty level of @p image is the size its format and
///        dimensions require.
///
/// The guard against an image whose buffers were produced for one format and
/// labelled another. Uploading one of those is not caught by the graphics API:
/// it reads the pitch the format implies and copies that many bytes per row out
/// of a buffer laid out differently, which samples as noise.
[[nodiscard]] bool ValidateMipChain(const DecodedImage &image);

} // namespace Assisi::Image
