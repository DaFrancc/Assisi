/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/* Provide the stb_dxt function bodies in exactly this translation unit. It
   carries BC1, BC3, BC4 and BC5; bc7enc below carries the one it does not.
   stb_dxt.h calls memcpy without including a header that declares it. */
#include <cstring>
#define STB_DXT_IMPLEMENTATION
#include <stb_dxt.h>

#include <bc7decomp.h>
#include <bc7enc.h>
#include <rgbcx.h>

#include <Assisi/Image/Compress.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/Logger.hpp>

#include <algorithm>
#include <array>
#include <mutex>
#include <vector>

namespace Assisi::Image
{

namespace
{
/// Texels in one 4x4 block.
constexpr std::size_t kTexelsPerBlock = kBlockExtent * kBlockExtent;

/// Bytes one block's worth of RGBA8 texels occupies, unpacked.
constexpr std::size_t kBlockRgba8Bytes = kTexelsPerBlock * kRgba8BytesPerTexel;

/// How many of BC7's partition patterns the fast tier searches. Zero leaves only
/// the single-subset modes, which is what makes it fast; the partition search is
/// where most of the encode time goes.
constexpr std::uint32_t kBc7FastPartitions = 0;

/// The full partition search, for an encode that happens once offline.
constexpr std::uint32_t kBc7BestPartitions = BC7ENC_MAX_PARTITIONS;

/// How hard the fast tier refines each block it has chosen. Zero is the
/// encoder's own default and does no extra refinement passes.
constexpr std::uint32_t kBc7FastUberLevel = 0;

/// The deepest refinement, paired with the full partition search.
constexpr std::uint32_t kBc7BestUberLevel = BC7ENC_MAX_UBER_LEVEL;

/// bc7enc builds global lookup tables on first use and the call is not
/// re-entrant, so workers encoding concurrently must not race into it.
void EnsureBc7Initialized()
{
    static std::once_flag initialized;
    std::call_once(initialized, [] { bc7enc_compress_block_init(); });
}

/// Copies the 4x4 block whose top-left texel is (blockX, blockY) out of an RGBA8
/// level into `out`.
///
/// A block that overhangs the level's edge repeats the last real row and column
/// into the remainder. Clamping rather than zero-filling matters: the encoder
/// fits endpoints across all sixteen texels, and padding with black would drag
/// those endpoints toward a colour the image does not contain, staining the edge
/// of every non-multiple-of-four level.
void ExtractBlock(const std::vector<unsigned char> &level, std::uint32_t width, std::uint32_t height,
                  std::uint32_t blockX, std::uint32_t blockY, std::array<unsigned char, kBlockRgba8Bytes> &out)
{
    for (std::uint32_t row = 0; row < kBlockExtent; ++row)
    {
        const std::uint32_t sourceY = std::min(blockY + row, height - 1);
        for (std::uint32_t column = 0; column < kBlockExtent; ++column)
        {
            const std::uint32_t sourceX = std::min(blockX + column, width - 1);

            const std::size_t from = (static_cast<std::size_t>(sourceY) * width + sourceX) * kRgba8BytesPerTexel;
            const std::size_t to   = (static_cast<std::size_t>(row) * kBlockExtent + column) * kRgba8BytesPerTexel;
            std::memcpy(out.data() + to, level.data() + from, kRgba8BytesPerTexel);
        }
    }
}

/// Writes one block of `format` from `block` into `dest`.
void EncodeBlock(PixelFormat format, const std::array<unsigned char, kBlockRgba8Bytes> &block,
                 const bc7enc_compress_block_params &bc7Params, unsigned char *dest)
{
    switch (format)
    {
    case PixelFormat::Bc4:
    {
        // stb's BC4 takes one byte per texel, so the red channel is gathered first.
        std::array<unsigned char, kTexelsPerBlock> red{};
        for (std::size_t texel = 0; texel < kTexelsPerBlock; ++texel)
        {
            red[texel] = block[texel * kRgba8BytesPerTexel];
        }
        stb_compress_bc4_block(dest, red.data());
        return;
    }
    case PixelFormat::Bc5:
    {
        // Two bytes per texel, red then green — the two channels a normal map
        // stores. Z is not stored at all; it is rebuilt at sample time.
        std::array<unsigned char, kTexelsPerBlock * 2> redGreen{};
        for (std::size_t texel = 0; texel < kTexelsPerBlock; ++texel)
        {
            redGreen[texel * 2 + 0] = block[texel * kRgba8BytesPerTexel + 0];
            redGreen[texel * 2 + 1] = block[texel * kRgba8BytesPerTexel + 1];
        }
        stb_compress_bc5_block(dest, redGreen.data());
        return;
    }
    case PixelFormat::Bc7:
        bc7enc_compress_block(dest, block.data(), &bc7Params);
        return;
    // Compress rejects a non-block target before any block is encoded, so
    // reaching this is a format that was added without an encoder beside it.
    // Left loud: the quiet alternative is a texture of zeroed blocks.
    default:
        ASSISI_ASSERT(false, "EncodeBlock reached a format with no encoder");
        Core::Log::Error("Image: no encoder for this pixel format; the block is left unwritten");
        return;
    }
}

/// Marks every texel of an unpacked block opaque. The single- and two-channel
/// formats carry no alpha, and a zero one would make the block read as invisible.
void SetBlockAlphaOpaque(std::array<unsigned char, kBlockRgba8Bytes> &block)
{
    for (std::size_t texel = 0; texel < kTexelsPerBlock; ++texel)
    {
        block[texel * kRgba8BytesPerTexel + 3] = 255;
    }
}

/// Unpacks one block of `format` into 16 RGBA8 texels.
void DecodeBlock(PixelFormat format, const unsigned char *source,
                 std::array<unsigned char, kBlockRgba8Bytes> &out)
{
    out.fill(0);
    switch (format)
    {
    // Writes the one stored channel straight into R, leaving G and B zero.
    case PixelFormat::Bc4:
        rgbcx::unpack_bc4(source, out.data(), static_cast<std::uint32_t>(kRgba8BytesPerTexel));
        SetBlockAlphaOpaque(out);
        return;
    // Channels 0 and 1 into R and G, which is where a normal map's X and Y live.
    // B stays zero: Z is not stored, it is rebuilt at sample time.
    case PixelFormat::Bc5:
        rgbcx::unpack_bc5(source, out.data(), 0, 1, static_cast<std::uint32_t>(kRgba8BytesPerTexel));
        SetBlockAlphaOpaque(out);
        return;
    case PixelFormat::Bc7:
        bc7decomp::unpack_bc7(source, reinterpret_cast<bc7decomp::color_rgba *>(out.data()));
        return;
    // Decompress rejects a non-block source before any block is read, so the
    // same reasoning as EncodeBlock's default applies.
    default:
        ASSISI_ASSERT(false, "DecodeBlock reached a format with no decoder");
        Core::Log::Error("Image: no decoder for this pixel format; the block is left black");
        return;
    }
}

/// Whether `format` carries data rather than colour, and so has no sRGB form.
bool IsDataFormat(PixelFormat format)
{
    return format == PixelFormat::Bc4 || format == PixelFormat::Bc5;
}
} // namespace

std::expected<DecodedImage, Core::AssetError> Compress(const DecodedImage &source, PixelFormat format,
                                                       CompressQuality quality)
{
    if (!IsBlockCompressed(format))
    {
        Core::Log::Error("Image: Compress target is not a block format");
        return std::unexpected(Core::AssetError::FileReadFailed);
    }
    if (source.format != PixelFormat::Rgba8)
    {
        Core::Log::Error("Image: Compress source must be RGBA8");
        return std::unexpected(Core::AssetError::FileReadFailed);
    }
    if (IsDataFormat(format) && source.colorSpace == ColorSpace::Srgb)
    {
        Core::Log::Error("Image: an sRGB source cannot be encoded as a data format, which has no sRGB form");
        return std::unexpected(Core::AssetError::FileReadFailed);
    }
    if (!ValidateMipChain(source))
    {
        Core::Log::Error("Image: Compress source has a malformed mip chain");
        return std::unexpected(Core::AssetError::FileReadFailed);
    }

    bc7enc_compress_block_params bc7Params{};
    if (format == PixelFormat::Bc7)
    {
        EnsureBc7Initialized();
        bc7enc_compress_block_params_init(&bc7Params);
        // An sRGB source is perceptually weighted; a linear one is not, because
        // its values are data whose error budget is uniform.
        if (source.colorSpace == ColorSpace::Srgb)
        {
            bc7enc_compress_block_params_init_perceptual_weights(&bc7Params);
        }
        const bool best              = quality == CompressQuality::Best;
        bc7Params.m_max_partitions   = best ? kBc7BestPartitions : kBc7FastPartitions;
        bc7Params.m_uber_level       = best ? kBc7BestUberLevel : kBc7FastUberLevel;
    }

    DecodedImage encoded;
    encoded.width      = source.width;
    encoded.height     = source.height;
    encoded.format     = format;
    encoded.colorSpace = source.colorSpace;
    encoded.mips.resize(source.mips.size());

    const std::size_t blockBytes = BytesPerBlock(format);
    for (std::size_t level = 0; level < source.mips.size(); ++level)
    {
        if (source.mips[level].empty())
        {
            continue;
        }
        const std::uint32_t mipWidth  = MipExtent(source.width, static_cast<std::uint32_t>(level));
        const std::uint32_t mipHeight = MipExtent(source.height, static_cast<std::uint32_t>(level));
        const LevelLayout layout      = LayoutFor(format, mipWidth, mipHeight);

        encoded.mips[level].assign(layout.byteSize, 0u);

        std::size_t offset = 0;
        for (std::uint32_t blockY = 0; blockY < mipHeight; blockY += kBlockExtent)
        {
            for (std::uint32_t blockX = 0; blockX < mipWidth; blockX += kBlockExtent)
            {
                std::array<unsigned char, kBlockRgba8Bytes> block{};
                ExtractBlock(source.mips[level], mipWidth, mipHeight, blockX, blockY, block);
                EncodeBlock(format, block, bc7Params, encoded.mips[level].data() + offset);
                offset += blockBytes;
            }
        }
    }
    return encoded;
}

std::expected<DecodedImage, Core::AssetError> Decompress(const DecodedImage &source)
{
    if (!IsBlockCompressed(source.format))
    {
        Core::Log::Error("Image: Decompress source is not block compressed");
        return std::unexpected(Core::AssetError::FileReadFailed);
    }
    if (!ValidateMipChain(source))
    {
        Core::Log::Error("Image: Decompress source has a malformed mip chain");
        return std::unexpected(Core::AssetError::FileReadFailed);
    }

    DecodedImage decoded;
    decoded.width      = source.width;
    decoded.height     = source.height;
    decoded.format     = PixelFormat::Rgba8;
    decoded.colorSpace = source.colorSpace;
    decoded.mips.resize(source.mips.size());

    const std::size_t blockBytes = BytesPerBlock(source.format);
    for (std::size_t level = 0; level < source.mips.size(); ++level)
    {
        if (source.mips[level].empty())
        {
            continue;
        }
        const std::uint32_t mipWidth  = MipExtent(source.width, static_cast<std::uint32_t>(level));
        const std::uint32_t mipHeight = MipExtent(source.height, static_cast<std::uint32_t>(level));

        decoded.mips[level].assign(LayoutFor(PixelFormat::Rgba8, mipWidth, mipHeight).byteSize, 0u);

        std::size_t offset = 0;
        for (std::uint32_t blockY = 0; blockY < mipHeight; blockY += kBlockExtent)
        {
            for (std::uint32_t blockX = 0; blockX < mipWidth; blockX += kBlockExtent)
            {
                std::array<unsigned char, kBlockRgba8Bytes> block{};
                DecodeBlock(source.format, source.mips[level].data() + offset, block);
                offset += blockBytes;

                // The blocks at the right and bottom edges carry padding texels
                // that the level itself does not have; only the real ones land.
                for (std::uint32_t row = 0; row < kBlockExtent; ++row)
                {
                    const std::uint32_t targetY = blockY + row;
                    if (targetY >= mipHeight)
                    {
                        break;
                    }
                    for (std::uint32_t column = 0; column < kBlockExtent; ++column)
                    {
                        const std::uint32_t targetX = blockX + column;
                        if (targetX >= mipWidth)
                        {
                            break;
                        }
                        const std::size_t from =
                            (static_cast<std::size_t>(row) * kBlockExtent + column) * kRgba8BytesPerTexel;
                        const std::size_t to =
                            (static_cast<std::size_t>(targetY) * mipWidth + targetX) * kRgba8BytesPerTexel;
                        std::memcpy(decoded.mips[level].data() + to, block.data() + from, kRgba8BytesPerTexel);
                    }
                }
            }
        }
    }
    return decoded;
}

} // namespace Assisi::Image
