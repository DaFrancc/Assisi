/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/* Provide stb_image / stb_image_resize function bodies in exactly this
   translation unit. */
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

#include <Assisi/Image/Decode.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>

#include <webp/decode.h>
#include <webp/demux.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace Assisi::Image
{

void BuildMipChain(DecodedImage &image, const unsigned char *base, bool generateMips)
{
    const std::uint32_t width = image.width;
    const std::uint32_t height = image.height;
    const std::uint32_t mipLevels = generateMips ? MipLevelCount(width, height) : 1;

    image.format = PixelFormat::Rgba8;
    image.mips.resize(mipLevels);
    image.mips[0].assign(base, base + LayoutFor(PixelFormat::Rgba8, width, height).byteSize);

    for (std::uint32_t level = 1; level < mipLevels; ++level)
    {
        const std::uint32_t mipWidth = MipExtent(width, level);
        const std::uint32_t mipHeight = MipExtent(height, level);

        const LevelLayout layout = LayoutFor(PixelFormat::Rgba8, mipWidth, mipHeight);
        std::vector<unsigned char> mip(layout.byteSize);

        const int inStride = static_cast<int>(LayoutFor(PixelFormat::Rgba8, width, 1).rowPitch);
        const int outStride = static_cast<int>(layout.rowPitch);

        // The resizer is colour-space-aware: an sRGB source is filtered in linear
        // space (RGB treated as sRGB, alpha as linear), which is what makes mips
        // generated from an sRGB image correct. Each level resizes from the base
        // rather than from the level above, so filter error does not compound.
        const unsigned char *ok =
            image.colorSpace == ColorSpace::Srgb
                ? stbir_resize_uint8_srgb(base, static_cast<int>(width), static_cast<int>(height), inStride, mip.data(),
                                          static_cast<int>(mipWidth), static_cast<int>(mipHeight), outStride,
                                          STBIR_RGBA)
                : stbir_resize_uint8_linear(base, static_cast<int>(width), static_cast<int>(height), inStride,
                                            mip.data(), static_cast<int>(mipWidth), static_cast<int>(mipHeight),
                                            outStride, STBIR_RGBA);
        if (ok == nullptr)
        {
            // Don't leave a hole: an unwritten level still gets created on the GPU,
            // and texture memory is not zeroed, so it would sample as whatever the
            // driver left there. Truncating gives the texture only the levels that
            // were actually written, and sampling falls back to the coarsest real one.
            Core::Log::Warn("Image: mip level {} downsample failed; truncating the chain to {} level(s)", level, level);
            image.mips.resize(level);
            return;
        }
        image.mips[level] = std::move(mip);
    }
}

namespace
{
/// How many values an 8-bit channel takes.
constexpr std::size_t kAlphaLevels = 256;
/// The largest 8-bit channel value, as a float.
constexpr float kAlphaMax = 255.f;
/// Bytes per RGBA8 texel, and where its alpha sits.
constexpr std::size_t kRgbaStride = 4;
constexpr std::size_t kAlphaOffset = 3;
/// Halvings of the scale's range: 24 leave it known to one part in sixteen
/// million of the widest range, far finer than an 8-bit alpha can show.
constexpr int kScaleSearchSteps = 24;

using AlphaHistogram = std::array<std::uint32_t, kAlphaLevels>;

/// How many of @p mip's texels hold each alpha value.
AlphaHistogram HistogramOf(const std::vector<unsigned char> &mip)
{
    AlphaHistogram histogram{};
    for (std::size_t i = kAlphaOffset; i < mip.size(); i += kRgbaStride)
    {
        ++histogram[mip[i]];
    }
    return histogram;
}

/// Alpha byte @p value scaled by @p scale, as the byte stored.
unsigned char ScaledAlpha(std::size_t value, float scale)
{
    return static_cast<unsigned char>(std::min(std::round(static_cast<float>(value) * scale), kAlphaMax));
}

/// The share of @p histogram's texels whose alpha, scaled by @p scale, is at
/// or above @p cutoff: kept rather than cut, as the alpha test decides it.
float CoverageAt(const AlphaHistogram &histogram, float cutoff, float scale)
{
    std::uint64_t kept = 0;
    std::uint64_t total = 0;
    for (std::size_t value = 0; value < kAlphaLevels; ++value)
    {
        total += histogram[value];
        if (static_cast<float>(ScaledAlpha(value, scale)) / kAlphaMax >= cutoff)
        {
            kept += histogram[value];
        }
    }
    return total == 0 ? 0.f : static_cast<float>(kept) / static_cast<float>(total);
}
} // namespace

void PreserveAlphaCoverage(DecodedImage &image, float cutoff)
{
    if (cutoff <= 0.f || image.format != PixelFormat::Rgba8 || image.mips.empty() || image.mips[0].empty())
    {
        return;
    }
    const float wanted = CoverageAt(HistogramOf(image.mips[0]), cutoff, 1.f);
    if (wanted <= 0.f || wanted >= 1.f)
    {
        return;
    }

    for (std::size_t level = 1; level < image.mips.size(); ++level)
    {
        std::vector<unsigned char> &mip = image.mips[level];
        const AlphaHistogram histogram = HistogramOf(mip);
        // Coverage only grows with the scale, so it is found by halving. Up to
        // the scale that lifts the faintest alpha a byte holds past any cutoff.
        float low = 0.f;
        float high = kAlphaMax;
        for (int step = 0; step < kScaleSearchSteps; ++step)
        {
            const float middle = 0.5f * (low + high);
            if (CoverageAt(histogram, cutoff, middle) < wanted)
            {
                low = middle;
            }
            else
            {
                high = middle;
            }
        }
        // Coverage moves in steps, one per alpha value the level holds, so the
        // nearer of the two ends is the closer answer.
        const float scale = std::abs(CoverageAt(histogram, cutoff, low) - wanted) <
                            std::abs(CoverageAt(histogram, cutoff, high) - wanted)
                                ? low
                                : high;
        for (std::size_t i = kAlphaOffset; i < mip.size(); i += kRgbaStride)
        {
            mip[i] = ScaledAlpha(mip[i], scale);
        }
    }
}

std::expected<ImageExtent, Core::AssetError> ReadImageExtent(std::span<const std::byte> bytes) noexcept
{
    // stb measures its buffer in int.
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return std::unexpected(Core::AssetErrorCode::CorruptAsset);
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    if (stbi_info_from_memory(reinterpret_cast<const stbi_uc *>(bytes.data()), static_cast<int>(bytes.size()), &width,
                              &height, &channels) == 0 ||
        width <= 0 || height <= 0)
    {
        return std::unexpected(Core::AssetErrorCode::CorruptAsset);
    }
    return ImageExtent{.width = static_cast<std::uint32_t>(width), .height = static_cast<std::uint32_t>(height)};
}

std::expected<DecodedImage, Core::AssetError> DecodeImageFile(const std::filesystem::path &file) noexcept
{
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char *data =
        stbi_load(file.string().c_str(), &width, &height, &channels, static_cast<int>(kRgba8BytesPerTexel));
    if (data == nullptr)
    {
        Core::Log::Error("Image: could not read the image file '{}'", file.string());
        return std::unexpected(Core::AssetErrorCode::FileReadFailed);
    }

    DecodedImage image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    BuildMipChain(image, data, /*generateMips=*/ false);

    stbi_image_free(data);
    return image;
}

std::expected<DecodedImage, Core::AssetError> DecodeImage(std::string_view vpath, ColorSpace colorSpace) noexcept
{
    const auto resolved = Core::AssetSystem::Resolve(vpath);
    if (!resolved)
    {
        return std::unexpected(resolved.error());
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char *data =
        stbi_load(resolved->string().c_str(), &width, &height, &channels, static_cast<int>(kRgba8BytesPerTexel));
    if (data == nullptr)
    {
        Core::Log::Error("Image: stbi_load failed for '{}'", vpath);
        return std::unexpected(Core::AssetErrorCode::FileReadFailed);
    }

    DecodedImage image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.colorSpace = colorSpace;
    BuildMipChain(image, data, /*generateMips=*/true);

    stbi_image_free(data);
    return image;
}

std::expected<DecodedImage, Core::AssetError> DecodeTexture(std::string_view vpath, ColorSpace colorSpace,
                                                            float alphaCutoff) noexcept
{
    std::expected<DecodedImage, Core::AssetError> image = DecodeImage(vpath, colorSpace);
    if (image)
    {
        PreserveAlphaCoverage(*image, alphaCutoff);
    }
    return image;
}

std::expected<std::vector<DecodedImage>, Core::AssetError> DecodeAnimatedWebp(std::string_view vpath,
                                                                              ColorSpace colorSpace) noexcept
{
    const std::expected<std::vector<std::byte>, Core::AssetError> bytes = Core::AssetSystem::ReadBinary(vpath);
    if (!bytes)
    {
        return std::unexpected(bytes.error());
    }

    WebPData webpData;
    webpData.bytes = reinterpret_cast<const uint8_t *>(bytes->data());
    webpData.size = bytes->size();

    // AnimDecoder composites frame disposal/blending for us and hands back full
    // canvas-sized RGBA frames — exactly what we upload. MODE_RGBA matches our
    // top-down RGBA8 convention (WebP rows are top-down like Vulkan's V=0).
    WebPAnimDecoderOptions options;
    if (!WebPAnimDecoderOptionsInit(&options))
    {
        Core::Log::Error("Image: WebPAnimDecoderOptionsInit failed for '{}'", vpath);
        return std::unexpected(Core::AssetErrorCode::FileReadFailed);
    }
    options.color_mode = MODE_RGBA;
    options.use_threads = 0;

    WebPAnimDecoder *decoder = WebPAnimDecoderNew(&webpData, &options);
    if (decoder == nullptr)
    {
        Core::Log::Error("Image: WebPAnimDecoderNew failed for '{}' (not a valid WebP?)", vpath);
        return std::unexpected(Core::AssetErrorCode::FileReadFailed);
    }

    WebPAnimInfo info;
    if (!WebPAnimDecoderGetInfo(decoder, &info))
    {
        Core::Log::Error("Image: WebPAnimDecoderGetInfo failed for '{}'", vpath);
        WebPAnimDecoderDelete(decoder);
        return std::unexpected(Core::AssetErrorCode::FileReadFailed);
    }

    std::vector<DecodedImage> frames;
    frames.reserve(info.frame_count);
    while (WebPAnimDecoderHasMoreFrames(decoder))
    {
        uint8_t *frameRgba = nullptr; // owned by the decoder; valid only until the next call
        int timestamp = 0;            // milliseconds (unused: playback speed is set by the caller)
        if (!WebPAnimDecoderGetNext(decoder, &frameRgba, &timestamp))
        {
            Core::Log::Warn("Image: WebPAnimDecoderGetNext failed at frame {} of '{}'", frames.size(), vpath);
            break; // keep whatever decoded cleanly rather than dropping the whole animation
        }

        DecodedImage image;
        image.width = info.canvas_width;
        image.height = info.canvas_height;
        image.colorSpace = colorSpace;
        // No mip chain: these are drawn near their native size, and a chain per
        // frame would multiply the (already many) uploads for no visible gain.
        BuildMipChain(image, frameRgba, /*generateMips=*/false);
        frames.push_back(std::move(image));
    }
    WebPAnimDecoderDelete(decoder);

    if (frames.empty())
    {
        Core::Log::Error("Image: decoded no frames from WebP '{}'", vpath);
        return std::unexpected(Core::AssetErrorCode::FileReadFailed);
    }
    return frames;
}

} // namespace Assisi::Image
