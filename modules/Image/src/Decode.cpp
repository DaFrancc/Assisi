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

#include <cstddef>
#include <string>
#include <vector>

namespace Assisi::Image
{

void BuildMipChain(DecodedImage &image, const unsigned char *base, bool generateMips)
{
    const std::uint32_t width     = image.width;
    const std::uint32_t height    = image.height;
    const std::uint32_t mipLevels = generateMips ? MipLevelCount(width, height) : 1;

    image.format = PixelFormat::Rgba8;
    image.mips.resize(mipLevels);
    image.mips[0].assign(base, base + LayoutFor(PixelFormat::Rgba8, width, height).byteSize);

    for (std::uint32_t level = 1; level < mipLevels; ++level)
    {
        const std::uint32_t mipWidth  = MipExtent(width, level);
        const std::uint32_t mipHeight = MipExtent(height, level);

        const LevelLayout layout = LayoutFor(PixelFormat::Rgba8, mipWidth, mipHeight);
        std::vector<unsigned char> mip(layout.byteSize);

        const int inStride  = static_cast<int>(LayoutFor(PixelFormat::Rgba8, width, 1).rowPitch);
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

std::expected<DecodedImage, Core::AssetError> DecodeImage(std::string_view vpath, ColorSpace colorSpace) noexcept
{
    const auto resolved = Core::AssetSystem::Resolve(vpath);
    if (!resolved)
    {
        return std::unexpected(resolved.error());
    }

    int width    = 0;
    int height   = 0;
    int channels = 0;
    unsigned char *data =
        stbi_load(resolved->string().c_str(), &width, &height, &channels,
                  static_cast<int>(kRgba8BytesPerTexel));
    if (data == nullptr)
    {
        Core::Log::Error("Image: stbi_load failed for '{}'", vpath);
        return std::unexpected(Core::AssetError::FileReadFailed);
    }

    DecodedImage image;
    image.width      = static_cast<std::uint32_t>(width);
    image.height     = static_cast<std::uint32_t>(height);
    image.colorSpace = colorSpace;
    BuildMipChain(image, data, /*generateMips=*/ true);

    stbi_image_free(data);
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
    webpData.size  = bytes->size();

    // AnimDecoder composites frame disposal/blending for us and hands back full
    // canvas-sized RGBA frames — exactly what we upload. MODE_RGBA matches our
    // top-down RGBA8 convention (WebP rows are top-down like Vulkan's V=0).
    WebPAnimDecoderOptions options;
    if (!WebPAnimDecoderOptionsInit(&options))
    {
        Core::Log::Error("Image: WebPAnimDecoderOptionsInit failed for '{}'", vpath);
        return std::unexpected(Core::AssetError::FileReadFailed);
    }
    options.color_mode  = MODE_RGBA;
    options.use_threads = 0;

    WebPAnimDecoder *decoder = WebPAnimDecoderNew(&webpData, &options);
    if (decoder == nullptr)
    {
        Core::Log::Error("Image: WebPAnimDecoderNew failed for '{}' (not a valid WebP?)", vpath);
        return std::unexpected(Core::AssetError::FileReadFailed);
    }

    WebPAnimInfo info;
    if (!WebPAnimDecoderGetInfo(decoder, &info))
    {
        Core::Log::Error("Image: WebPAnimDecoderGetInfo failed for '{}'", vpath);
        WebPAnimDecoderDelete(decoder);
        return std::unexpected(Core::AssetError::FileReadFailed);
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
        image.width      = info.canvas_width;
        image.height     = info.canvas_height;
        image.colorSpace = colorSpace;
        // No mip chain: these are drawn near their native size, and a chain per
        // frame would multiply the (already many) uploads for no visible gain.
        BuildMipChain(image, frameRgba, /*generateMips=*/ false);
        frames.push_back(std::move(image));
    }
    WebPAnimDecoderDelete(decoder);

    if (frames.empty())
    {
        Core::Log::Error("Image: decoded no frames from WebP '{}'", vpath);
        return std::unexpected(Core::AssetError::FileReadFailed);
    }
    return frames;
}

} // namespace Assisi::Image
